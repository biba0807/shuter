//
// Created by Neirokan on 30.04.2020
//

#include "ClientUDP.h"
#include "MsgType.h"
#include "../utils/Time.h"
#include "../utils/Log.h"
#include "../Consts.h"

ClientUDP::ClientUDP() {
    _socket.setTimeoutCallback([this](sf::Uint16 id) { return ClientUDP::timeout(id); });
}

bool ClientUDP::connected() const {
    return _socket.ownId();
}

bool ClientUDP::isWorking() const {
    return _working;
}

void ClientUDP::connect(sf::IpAddress ip, sf::Uint16 port, const std::string& playerName) {
    _playerName = playerName;
    _ip = ip;
    _port = port;

    sf::Packet packet;
    // Используем твой оператор << (он запишет Uint16)
    packet << MsgType::Connect;
    packet << (sf::Uint32)Consts::NETWORK_VERSION;
    packet << _playerName;

    _working = _socket.bind(0);
    _socket.addConnection(_socket.serverId(), ip, port);
    _socket.sendRely(packet, _socket.serverId());

    Log::log("ClientUDP::connect(): connecting " + _playerName + " to the server...");
}

void ClientUDP::update() {
    if (!isWorking()) {
        return;
    }

    while (isWorking() && process()) {}

    // Send new client information to server
    if (Time::time() - _lastBroadcast > 1.0 / Consts::NETWORK_WORLD_UPDATE_RATE && connected()) {
        updatePacket();
        _lastBroadcast = Time::time();
    }

    // Socket update
    _socket.update();
}

void ClientUDP::disconnect() {
    sf::Packet packet;
    packet << MsgType::Disconnect << _socket.ownId();
    _socket.send(packet, _socket.serverId());
    _socket.unbind();
    _working = false;

    Log::log("ClientUDP::disconnect(): disconnected from the server.");
    processDisconnected();
}

bool ClientUDP::timeout(sf::Uint16 id) {
    Log::log("ClientUDP::timeout(): timeout from the server.");

    if (id != _socket.serverId()) {
        return true;
    }
    disconnect();
    return false;
}

// Recive and process message.
// Returns true, if some message was received.
bool ClientUDP::process() {
    sf::Packet packet;
    sf::Uint16 senderId;

    // Получаем тип сообщения из сокета
    MsgType type = _socket.receive(packet, senderId);

    if (type == MsgType::Empty) {
        return false;
    }

    // Если мы еще не получили свой ID, игнорируем всё, кроме Init
    if (!connected() && type != MsgType::Init) {
        return true;
    }

    switch (type) {
        case MsgType::Init: {
            sf::Uint16 targetId;
            if (packet >> targetId) {
                _socket.setId(targetId);
                // Теперь сокет знает наш ID, и connected() вернет true
                Log::log("ClientUDP::process(): Connected! Assigned ID = " + std::to_string(targetId));
                processInit(packet);
            } else {
                Log::log("ClientUDP::process(): Failed to read ID from Init packet");
            }
            break;
        }

        case MsgType::ServerUpdate:
            processUpdate(packet);
            break;

        case MsgType::NewClient:
            Log::log("ClientUDP::process(): new client joined world");
            processNewClient(packet);
            break;

        case MsgType::Disconnect: {
            sf::Uint16 targetId;
            packet >> targetId;
            if (targetId == _socket.ownId()) {
                disconnect();
            } else {
                processDisconnect(targetId);
            }
            Log::log("ClientUDP::process(): client Id = " + std::to_string(targetId) + " disconnected.");
            break;
        }

        case MsgType::Custom:
            processCustomPacket(packet);
            break;

        case MsgType::Error:
            // Это то, что мы видели в логах.
            // Обычно это значит, что пришел пакет от неизвестного ID или дубликат.
            break;

        default:
            Log::log("ClientUDP::process(): unknown message type " + std::to_string(static_cast<int>(type)));
    }

    return true;
}
