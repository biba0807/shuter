#include <algorithm>
#include "UDPSocket.h"
#include "../utils/Time.h"
#include "../Consts.h"
#include "../utils/Log.h"

UDPSocket::UDPSocket() : _ownId(0), _nextRelyMsgId(0) {
    _socket.setBlocking(false);
}

void UDPSocket::addConnection(sf::Uint16 id, sf::IpAddress ip, sf::Uint16 port) {
    _connections.insert({id, UDPConnection(id, ip, port)});
}

void UDPSocket::removeConnection(sf::Uint16 id) {
    _connections.erase(id);
}

bool UDPSocket::bind(sf::Uint16 port) {
    return _socket.bind(port) == sf::Socket::Status::Done;
}

void UDPSocket::unbind() {
    sf::Packet packet;
    packet << MsgType::Disconnect << _ownId;

    for (auto it = _connections.begin(); it != _connections.end();) {
        send(packet, it->first);
        _connections.erase(it++);
    }

    _relyPackets.clear();
    _confirmTimes.clear();
    _socket.unbind();
    setId(0);
}

void UDPSocket::setTimeoutCallback(std::function<bool(sf::Uint16)> callback) {
    _timeoutCallback = std::move(callback);
}

void UDPSocket::setId(sf::Uint16 id) {
    _ownId = id;
}

sf::Uint16 UDPSocket::ownId() const {
    return _ownId;
}

sf::Uint16 UDPSocket::serverId() const {
    return _serverId;
}

void UDPSocket::sendRely(const sf::Packet &packet, const sf::IpAddress &ip, sf::Uint16 port) {
    sf::Packet finalPacket;
    finalPacket << _ownId << true << _nextRelyMsgId;
    finalPacket.append(packet.getData(), packet.getDataSize());
    _relyPackets.insert({_nextRelyMsgId++, ReliableMsg(finalPacket, ip, port)});
}

void UDPSocket::sendRely(const sf::Packet &packet, sf::Uint16 id) {
    if (!_connections.count(id)) return;
    this->sendRely(packet, _connections.at(id).ip(), _connections.at(id).port());
}

void UDPSocket::send(const sf::Packet &packet, const sf::IpAddress &ip, sf::Uint16 port) {
    sf::Packet finalPacket;
    // Формируем заголовок: ID, флаг надежности(false), msgId(0)
    finalPacket << _ownId << false << (sf::Uint16)0;
    finalPacket.append(packet.getData(), packet.getDataSize());
    _socket.send(finalPacket, ip, port);
}

void UDPSocket::send(const sf::Packet &packet, sf::Uint16 id) {
    if (!_connections.count(id)) return;
    this->send(packet, _connections.at(id).ip(), _connections.at(id).port());
}

void UDPSocket::update() {
    for (auto it = _connections.begin(); it != _connections.end();) {
        if (!it->second.timeout()) {
            ++it;
        } else {
            if (_timeoutCallback && !_timeoutCallback(it->first)) return;
            _connections.erase(it++);
        }
    }

    for (auto it = _relyPackets.begin(); it != _relyPackets.end();) {
        if (!it->second.trySend(_socket)) {
            _relyPackets.erase(it++);
        } else {
            ++it;
        }
    }

    for (auto it = _confirmTimes.begin(); it != _confirmTimes.end();) {
        if (Time::time() - it->second > Consts::NETWORK_TIMEOUT) {
            _confirmTimes.erase(it++);
        } else {
            ++it;
        }
    }
}

MsgType UDPSocket::receive(sf::Packet &packet, sf::Uint16 &senderId) {
    sf::IpAddress ip;
    sf::Uint16 port;

    packet.clear();
    if (_socket.receive(packet, ip, port) != sf::Socket::Status::Done) {
        return MsgType::Empty;
    }

    bool reply = false;
    sf::Uint16 msgId = 0;
    MsgType type = MsgType::Empty;
    senderId = 0;

    if (!(packet >> senderId >> reply >> msgId >> type)) {
        Log::log("UDPSocket::receive: Bad header");
        return MsgType::Error;
    }

    // 1. Обработка CONNECT (только сервер)
    if (type == MsgType::Connect) {
        bool found = false;
        for (auto& [id, conn] : _connections) {
            if (conn.same(ip, port)) {
                senderId = id;
                found = true;
                break;
            }
        }
        if (!found) {
            for (sf::Uint16 tmp = 1; tmp <= Consts::NETWORK_MAX_CLIENTS; tmp++) {
                if (!_connections.count(tmp)) {
                    senderId = tmp;
                    _connections.insert({senderId, UDPConnection(senderId, ip, port)});
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            confirmed(msgId, senderId);
            return MsgType::Connect;
        }
        return MsgType::Error;
    }

    // 2. Обработка CONFIRM (технический пакет)
    if (type == MsgType::Confirm) {
        _relyPackets.erase(msgId);
        return MsgType::Empty;
    }

    // 3. Обработка INIT (только клиент)
    if (type == MsgType::Init) {
        // Если клиент получает Init, он должен запомнить сервер в _connections
        if (!_connections.count(senderId)) {
            _connections.insert({senderId, UDPConnection(senderId, ip, port)});
        }
        confirmed(msgId, senderId);
        return MsgType::Init;
    }

    // 4. Проверка остальных типов
    if (!_connections.count(senderId) || !_connections.at(senderId).same(ip, port)) {
        return MsgType::Error;
    }

    _connections.at(senderId).update();

    // Подтверждаем надежные пакеты
    if (reply && confirmed(msgId, senderId)) {
        return MsgType::Empty; // Дубликат
    }

    return type;
}

bool UDPSocket::confirmed(sf::Uint16 msgId, sf::Uint16 senderId) {
    sf::Packet confirmPacket;
    confirmPacket << _ownId << false << msgId << MsgType::Confirm;
    _connections.at(senderId).send(_socket, confirmPacket);

    sf::Uint32 confirmId = (static_cast<sf::Uint32>(senderId) << 16) | msgId;
    bool repeat = _confirmTimes.count(confirmId);
    _confirmTimes[confirmId] = Time::time();
    return repeat;
}

UDPSocket::~UDPSocket() {
    unbind();
}