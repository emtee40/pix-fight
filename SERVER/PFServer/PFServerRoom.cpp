//
//  PFServerRoom.cpp
//  PFServer
//
//  Created by Marcin Małysz on 18/05/2018.
//  Copyright © 2018 Marcin Małysz. All rights reserved.
//

#include "PFServerRoom.hpp"

using namespace std;

static const double kMaximumRoomTime = 60 * 5;
static const double kMaximumPlayerTimeout = 30;

PFServerRoom::PFServerRoom(uint32_t port, bool privateRoom)
: _port(port)
, _private(privateRoom)
, _status(PFRoomStatusAwaiting)
, _roomInfo({})
, _lastUpdate(time(0))
, _roomSocket(new CPassiveSocket())
, _client(nullptr)
, _connectedPlayers({})
, _currentPlayerTurn(0) {

    memset(&_roomInfo, 0, sizeof(_roomInfo));

    //minimum players to allow connection so we can retrive map info
    _roomInfo.players = 2;

    _roomSocket->Initialize();
    _roomSocket->SetNonblocking();

    if (!_roomSocket->Listen("0.0.0.0", _port)) {

        cout << "Error starting room: " << _roomSocket->DescribeError() << endl;
        _roomSocket->Close();
        _status = PFRoomStatusTimeout;
    }
    else {

        cout << "Created room on port: " << _port << " private: "<< _private << endl;
    }
}

PFServerRoom::~PFServerRoom() noexcept {

    for (auto &player : _connectedPlayers) {

        player->close();
    }

    _roomSocket->Close();
}

void PFServerRoom::update() {

    bool allowsPlayers = true;

    if (_roomInfo.players > 0) {

        allowsPlayers = _connectedPlayers.size() < _roomInfo.players;
    }

    //finding clients
    if (((_client = _roomSocket->Accept()) != nullptr) && (_status == PFRoomStatusAwaiting) && allowsPlayers) {

        cout << "Player connected to room." << endl;

        _lastUpdate = time(0);

        //make non blocking
        _client->SetNonblocking();

        //assign new player
        auto player = make_shared<PFSocketClient>(_client);

        _connectedPlayers.push_back(player);

        //start looking for other one
        player = nullptr;
    }

    auto currentTime = time(0);

    //clients code
    for (auto it = _connectedPlayers.begin(); it != _connectedPlayers.end();) {

        auto player = *it;

        player->update();
        auto remove = handlePlayer(player);

        double diff = difftime(currentTime, player->getCurrentTime());

        //player timeout or need to be removed
        if (diff > kMaximumPlayerTimeout || remove) {

            player->close();
            player->dispose();

            it = _connectedPlayers.erase(it);

            _status = remove ? PFRoomStatusAwaiting : PFRoomStatusTimeout;
        }
        else {

            ++it;
        }
    }
}

bool PFServerRoom::isValid() {

    for (auto &player : _connectedPlayers) {

        if (!player->isValid()) {

            return false;
        }
    }

    return _roomSocket->IsSocketValid();
}

bool PFServerRoom::isUnused() {

    if (_status == PFRoomStatusTimeout || _status == PFRoomStatusFinished) {

        return true;
    }

    if (_status == PFRoomStatusAwaiting) {

        time_t current = time(0);
        double diff = difftime(current, _lastUpdate);

        //Timeout
        if (diff > kMaximumRoomTime) {

            _status = PFRoomStatusTimeout;
            return true;
        }
    }

    return false;
}

bool PFServerRoom::handlePlayer(shared_ptr<PFSocketClient> &player) {

    auto command = player->getCurrentCommand();

    if (command == PFSocketCommandTypeUnknown) {

        return false;
    }

    switch (command) {

        case PFSocketCommandTypeMakeRoom:
        case PFSocketCommandTypeUnknown:
        case PFSocketCommandTypeDisconnect:
        case PFSocketCommandTypeRooms:
        case PFSocketCommandTypeOk: 
            break;

        case PFSocketCommandTypeHeartbeat: {

            player->ping();
            
            break;
        }
        case PFSocketCommandTypeLeaveRoom: {

            confirmPacket(player);
            player->dispose();

            return true;
        }
        case PFSocketCommandTypeRemoveRoom: {

            _status = PFRoomStatusFinished;
            confirmPacket(player);

            break;
        }
        case PFSocketCommandTypeGameInfo: {

            _lastUpdate = time(0);

            auto roomData = player->getPacketData();
            memcpy(&_roomInfo, roomData.data(), sizeof(_roomInfo));

            confirmPacket(player);

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeGameInfo;
            packet->size = (uint32_t)roomData.size();
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, roomData.data(), sizeof(_roomInfo));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &local : _connectedPlayers) {

                local->setReady(false);

                if (local == player) {
                    continue;
                }

                local->sendPacket(packet);
            }

            break;
        }
        case PFSocketCommandTypeGetGameInfo: {

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeGameInfo;
            packet->size = (uint32_t)sizeof(_roomInfo)/sizeof(uint8_t);
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, &_roomInfo, sizeof(_roomInfo));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &local : _connectedPlayers) {

                if (local == player) {

                    local->sendPacket(packet);
                }
            }

            break;
        }
        case PFSocketCommandTypeSendTurn: {

            _currentPlayerTurn++;

            if (_currentPlayerTurn >= _connectedPlayers.size()) {

                _currentPlayerTurn = 0;
            }

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeSendTurn;
            packet->size = sizeof(_currentPlayerTurn) / sizeof(uint8_t);
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, &_currentPlayerTurn, sizeof(_currentPlayerTurn));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &local : _connectedPlayers) {

                local->sendPacket(packet);
            }

            break;
        }
        case PFSocketCommandTypeEndGame: {

            //finish game on each side sending player who wins and close room
            auto result = player->getPacketData();
            uint32_t winnerID = 0;
            memcpy(&winnerID, result.data(), sizeof(winnerID));

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeEndGame;
            packet->size = sizeof(winnerID) / sizeof(uint8_t);
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, &winnerID, sizeof(winnerID));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &local : _connectedPlayers) {

                local->sendPacket(packet);
            }

            _status = PFRoomStatusFinished;

            break;
        }
        case PFSocketCommandTypeReady: {

            //if all players ready start game
            _lastUpdate = time(0);

            player->setReady(true);

            uint16_t playersReady = 0;

            for (auto &p : _connectedPlayers) {

                if (p->isReady()) {
                    playersReady++;
                }
            }

            auto size = _connectedPlayers.size();

            cout << "Ready (" << playersReady << ") connected: " << size << " roomsize: " << _roomInfo.players << endl;

            if (playersReady == size && _roomInfo.players == size) {

                _status = PFRoomStatusPlaying;

                for (auto &p : _connectedPlayers) {

                    auto it = find(_connectedPlayers.begin(), _connectedPlayers.end(), p);
                    uint32_t playerID = (uint32_t)distance(_connectedPlayers.begin(), it);
                  
                    auto packet = make_unique<PFPacket>();
                    packet->type = PFSocketCommandTypeLoad;
                    packet->size = sizeof(playerID) / sizeof(uint8_t);
                    packet->data = new uint8_t[packet->size];
                    memcpy(packet->data, &playerID, sizeof(playerID));
                    packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

                    p->sendPacket(packet);
                }
            }

            break;
        }
        case PFSocketCommandTypeLoad: {

            //TODO: all players need to send LOAD Info before start!

            player->setLoaded(true);

            uint16_t playersLoaded = 0;

            for (auto &p : _connectedPlayers) {

                if (p->isLoaded()) {
                    playersLoaded++;
                }
            }

            auto size = _connectedPlayers.size();

            cout << "Loaded (" << playersLoaded << ") connected: " << size << " roomsize: " << _roomInfo.players << endl;

            if (playersLoaded == size && _roomInfo.players == size) {

                //player loaded map send his turn info
                auto packet = make_unique<PFPacket>();
                packet->type = PFSocketCommandTypeSendTurn;
                packet->size = sizeof(_currentPlayerTurn) / sizeof(uint8_t);
                packet->data = new uint8_t[packet->size];
                memcpy(packet->data, &_currentPlayerTurn, sizeof(_currentPlayerTurn));
                packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

                for (auto &p : _connectedPlayers) {

                    p->sendPacket(packet);
                }
            }

            break;
        }
        case PFSocketCommandTypeFire: {

            //pass attacker id, attacked id and both size
            auto result = player->getPacketData();

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeFire;
            packet->size = (uint32_t)result.size();
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, result.data(), packet->size * sizeof(uint8_t));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &p : _connectedPlayers) {

                if (p == player) {
                    continue;
                }

                p->sendPacket(packet);
            }

            break;
        }
        case PFSocketCommandTypeMove: {

            //pass unit id and destination vector
            auto result = player->getPacketData();

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeMove;
            packet->size = (uint32_t)result.size();
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, result.data(), packet->size * sizeof(uint8_t));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &p : _connectedPlayers) {

                if (p == player) {
                    continue;
                }

                p->sendPacket(packet);
            }

            break;
        }
        case PFSocketCommandTypeBuild: {

            //pass base id, unit type to build

            auto result = player->getPacketData();

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeBuild;
            packet->size = (uint32_t)result.size();
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, result.data(), packet->size * sizeof(uint8_t));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &p : _connectedPlayers) {

                if (p == player) {
                    continue;
                }

                p->sendPacket(packet);
            }

            break;
        }
        case PFSocketCommandTypeCapture: {

            auto result = player->getPacketData();

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeCapture;
            packet->size = (uint32_t)result.size();
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, result.data(), packet->size * sizeof(uint8_t));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &p : _connectedPlayers) {

                if (p == player) {
                    continue;
                }

                p->sendPacket(packet);
            }

            break;
        }
        case PFSocketCommandTypeRepair: {

            auto result = player->getPacketData();

            auto packet = make_unique<PFPacket>();
            packet->type = PFSocketCommandTypeRepair;
            packet->size = (uint32_t)result.size();
            packet->data = new uint8_t[packet->size];
            memcpy(packet->data, result.data(), packet->size * sizeof(uint8_t));
            packet->crcsum = crc32c(packet->crcsum, packet->data, packet->size);

            for (auto &p : _connectedPlayers) {

                if (p == player) {
                    continue;
                }

                p->sendPacket(packet);
            }

            break;
        }
    }

    player->dispose();

    return false;
}

void PFServerRoom::confirmPacket(std::shared_ptr<PFSocketClient> &player) {

    auto packet = make_unique<PFPacket>();
    packet->type = PFSocketCommandTypeOk;
    player->sendPacket(packet);
}
