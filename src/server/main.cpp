#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include "../shared/Geometry.h" // only for Vector2/Rectangle math types, no window/raylib linking

#include "../shared/Protocol.h"
#include "../shared/Serialize.h"
#include "../shared/Socket.h"
#include "SessionManager.h"
#include "Item.h"
#include "Hazard.h"
#include "Combat.h"
#include "DebugActions.h"

struct ClientLocation {
    bool found;
    std::string sessionName;
    int slotIndex;
};

static ClientLocation FindClientByAddress(SessionManager& manager, const std::vector<std::string>& sessionNames,
                                           const std::string& ip, uint16_t port) {
    for (const auto& name : sessionNames) {
        Session* session = manager.GetSession(name);
        if (!session) continue;
        for (int i = 0; i < 2; i++) {
            if (session->slots[i].clientIp == ip && session->slots[i].clientPort == port &&
                session->slots[i].state == SlotState::Connected) {
                return { true, name, i };
            }
        }
    }
    return { false, "", -1 };
}

static const uint16_t kServerPort = 7777;
static const float kMoveSpeed = 200.0f;
static const float kPickupRadius = 24.0f;
static const float kReviveRange = 32.0f;

static double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void SendReliable(UdpSocket& socket, PlayerSlot& slot, MessageType type, const uint8_t* payload, size_t payloadLen, double now) {
    std::vector<uint8_t> buffer;
    buffer.push_back((uint8_t)type);
    buffer.insert(buffer.end(), payload, payload + payloadLen);

    uint32_t seq = slot.reliableSender.NextSeq();
    slot.reliableSender.TrackUnacked(seq, buffer, now);

    PacketHeader header{ 1, seq, (uint16_t)buffer.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), buffer.begin(), buffer.end());
    socket.SendTo(slot.clientIp, slot.clientPort, packet.data(), packet.size());
}

static void SendUnreliable(UdpSocket& socket, const std::string& ip, uint16_t port, uint32_t seq, MessageType type, const uint8_t* payload, size_t payloadLen) {
    std::vector<uint8_t> buffer;
    buffer.push_back((uint8_t)type);
    buffer.insert(buffer.end(), payload, payload + payloadLen);

    PacketHeader header{ 0, seq, (uint16_t)buffer.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), buffer.begin(), buffer.end());
    socket.SendTo(ip, port, packet.data(), packet.size());
}

int main() {
    std::srand((unsigned int)std::time(nullptr));

    UdpSocket socket;
    if (!socket.Bind(kServerPort)) {
        std::printf("Failed to bind UDP port %d\n", (int)kServerPort);
        return 1;
    }
    std::printf("Server listening on UDP port %d\n", (int)kServerPort);

    SessionManager sessionManager;
    HazardZone hazard{ Rectangle{450.0f, 200.0f, 100.0f, 200.0f} };

    // Track world items and unreliable-send sequence counters per session name,
    // since Session itself only holds player slots.
    std::map<std::string, std::vector<WorldItem>> sessionWorldItems;
    std::map<std::string, float[2]> sessionHazardCarry;
    std::map<std::string, uint32_t> sessionSnapshotSeq;
    std::map<std::string, bool[2]> pendingAttack;
    std::map<std::string, bool[2]> pendingInteract;

    const double tickInterval = 1.0 / 60.0;
    double lastTick = NowSeconds();

    uint8_t recvBuffer[1024];

    while (true) {
        double now = NowSeconds();

        // --- Receive and dispatch incoming packets ---
        std::string fromIp;
        uint16_t fromPort;
        int received = socket.ReceiveFrom(recvBuffer, sizeof(recvBuffer), fromIp, fromPort);
        if (received > (int)sizeof(PacketHeader)) {
            PacketHeader header{};
            DeserializeStruct(recvBuffer, received, header);
            const uint8_t* payload = recvBuffer + sizeof(PacketHeader);
            size_t payloadLen = (size_t)received - sizeof(PacketHeader);

            if (payloadLen >= 1) {
                MessageType type = (MessageType)payload[0];
                const uint8_t* body = payload + 1;
                size_t bodyLen = payloadLen - 1;

                if (header.channel == 1 && type == MessageType::ConnectRequest) {
                    ConnectRequestMsg msg{};
                    if (DeserializeStruct(body, bodyLen, msg)) {
                        std::string sessionName(msg.sessionName);
                        ConnectOutcome outcome = sessionManager.HandleConnect(sessionName, msg.reconnectToken, fromIp, fromPort, now);

                        if (outcome.result == ConnectResult::Rejected) {
                            RejectedMsg reject{ outcome.rejectReason };
                            std::vector<uint8_t> rejectBytes;
                            SerializeStruct(reject, rejectBytes);
                            std::vector<uint8_t> full;
                            full.push_back((uint8_t)MessageType::Rejected);
                            full.insert(full.end(), rejectBytes.begin(), rejectBytes.end());
                            PacketHeader rejectHeader{ 1, 0, (uint16_t)full.size() };
                            std::vector<uint8_t> packet;
                            SerializeStruct(rejectHeader, packet);
                            packet.insert(packet.end(), full.begin(), full.end());
                            socket.SendTo(fromIp, fromPort, packet.data(), packet.size());
                        } else {
                            Session* session = sessionManager.GetSession(sessionName);
                            if (sessionWorldItems.find(sessionName) == sessionWorldItems.end()) {
                                sessionWorldItems[sessionName] = {
                                    WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
                                    WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
                                };
                                sessionHazardCarry[sessionName][0] = 0.0f;
                                sessionHazardCarry[sessionName][1] = 0.0f;
                                sessionSnapshotSeq[sessionName] = 1;
                            }

                            WelcomeMsg welcome{
                                (uint8_t)outcome.slotIndex, outcome.sessionToken,
                                Player::kMaxHp, Player::kDownedDuration, Player::kDeathRespawnDelay,
                                Player::kReviveHp, Player::kRespawnHp, Player::kAttackCooldown,
                                Player::kAttackRange, Player::kAttackDamage, Player::kChannelDuration,
                                kHazardDamagePerSecond, Inventory::kCapacity
                            };
                            PlayerSlot& slot = session->slots[outcome.slotIndex];
                            std::vector<uint8_t> welcomeBytes;
                            SerializeStruct(welcome, welcomeBytes);
                            SendReliable(socket, slot, MessageType::Welcome, welcomeBytes.data(), welcomeBytes.size(), now);
                        }
                    }
                } else {
                    std::vector<std::string> sessionNames = sessionManager.GetSessionNames();
                    ClientLocation loc = FindClientByAddress(sessionManager, sessionNames, fromIp, fromPort);
                    if (loc.found) {
                        Session* session = sessionManager.GetSession(loc.sessionName);
                        PlayerSlot& slot = session->slots[loc.slotIndex];
                        slot.lastPacketAtSeconds = now;

                        if (header.channel == 0 && type == MessageType::Input) {
                            InputMsg input{};
                            if (DeserializeStruct(body, bodyLen, input)) {
                                slot.player.position.x += input.moveX * kMoveSpeed * (float)tickInterval;
                                slot.player.position.y += input.moveY * kMoveSpeed * (float)tickInterval;
                                pendingAttack[loc.sessionName][loc.slotIndex] = input.attackPressed;
                                pendingInteract[loc.sessionName][loc.slotIndex] = input.interactHeld;
                            }
                        } else if (header.channel == 1 && type == MessageType::Ack) {
                            AckMsg ack{};
                            if (DeserializeStruct(body, bodyLen, ack)) {
                                slot.reliableSender.OnAckReceived(ack.ackedSeq);
                            }
                        } else if (header.channel == 1 && type == MessageType::DebugActionRequest) {
                            DebugActionRequestMsg req{};
                            if (DeserializeStruct(body, bodyLen, req)) {
                                std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ready;
                                slot.reliableReceiver.TryDeliverInOrder(header.seq, std::vector<uint8_t>(body, body + bodyLen), ready);
                                for (auto& msg : ready) {
                                    DebugActionRequestMsg deliveredReq{};
                                    if (DeserializeStruct(msg.second.data(), msg.second.size(), deliveredReq)) {
                                        if (deliveredReq.targetSlot < 2) {
                                            ApplyDebugAction(session->slots[deliveredReq.targetSlot].player, deliveredReq.action);
                                        }
                                    }
                                }
                                AckMsg ackReply{ header.seq };
                                std::vector<uint8_t> ackBytes;
                                SerializeStruct(ackReply, ackBytes);
                                std::vector<uint8_t> ackFull;
                                ackFull.push_back((uint8_t)MessageType::Ack);
                                ackFull.insert(ackFull.end(), ackBytes.begin(), ackBytes.end());
                                PacketHeader ackHeader{ 1, 0, (uint16_t)ackFull.size() };
                                std::vector<uint8_t> ackPacket;
                                SerializeStruct(ackHeader, ackPacket);
                                ackPacket.insert(ackPacket.end(), ackFull.begin(), ackFull.end());
                                socket.SendTo(slot.clientIp, slot.clientPort, ackPacket.data(), ackPacket.size());
                            }
                        }
                    }
                }
            }
        }

        // --- Fixed 60Hz simulation tick ---
        now = NowSeconds();
        if (now - lastTick >= tickInterval) {
            float dt = (float)(now - lastTick);
            lastTick = now;
            sessionManager.CheckAllTimeouts(now);

            for (auto& sessionEntry : sessionManager.GetSessionNames()) {
                Session* session = sessionManager.GetSession(sessionEntry);
                if (!session) continue;

                Player& p0 = session->slots[0].player;
                Player& p1 = session->slots[1].player;
                std::vector<WorldItem>& items = sessionWorldItems[sessionEntry];
                float* hazardCarry = sessionHazardCarry[sessionEntry];
                bool* attack = pendingAttack.count(sessionEntry) ? pendingAttack[sessionEntry] : nullptr;
                bool* interact = pendingInteract.count(sessionEntry) ? pendingInteract[sessionEntry] : nullptr;

                bool p0CanRevive = p1.state == PlayerState::Downed && DistanceBetween(p0.position, p1.position) <= kReviveRange;
                bool p1CanRevive = p0.state == PlayerState::Downed && DistanceBetween(p1.position, p0.position) <= kReviveRange;

                if (interact) {
                    if (p0.state == PlayerState::Alive && interact[0] && !p0CanRevive) {
                        for (auto& item : items) { if (TryPickup(item, p0.position, p0.inventory, kPickupRadius)) break; }
                    }
                    if (p1.state == PlayerState::Alive && interact[1] && !p1CanRevive) {
                        for (auto& item : items) { if (TryPickup(item, p1.position, p1.inventory, kPickupRadius)) break; }
                    }
                }
                if (attack) {
                    if (attack[0]) { TryAttack(p0, p1); attack[0] = false; }
                    if (attack[1]) { TryAttack(p1, p0); attack[1] = false; }
                }
                UpdateAttackCooldown(p0, dt);
                UpdateAttackCooldown(p1, dt);

                bool p0InteractHeld = interact ? interact[0] : false;
                bool p1InteractHeld = interact ? interact[1] : false;
                UpdateRevive(p0, p1, p0InteractHeld, dt, kReviveRange);
                UpdateRevive(p1, p0, p1InteractHeld, dt, kReviveRange);

                ApplyHazardDamage(hazard, p0, dt, hazardCarry[0]);
                ApplyHazardDamage(hazard, p1, dt, hazardCarry[1]);

                p0.UpdateTimers(dt);
                p1.UpdateTimers(dt);

                SnapshotMsg snap{};
                snap.players[0] = PlayerSnapshot{ p0.position.x, p0.position.y, p0.hp, (uint8_t)p0.state, p0.inventory.Count(ItemType::RevivePotion), p0.channelTimer };
                snap.players[1] = PlayerSnapshot{ p1.position.x, p1.position.y, p1.hp, (uint8_t)p1.state, p1.inventory.Count(ItemType::RevivePotion), p1.channelTimer };
                for (int i = 0; i < 2 && i < (int)items.size(); i++) {
                    snap.items[i] = WorldItemSnapshot{ items[i].position.x, items[i].position.y, items[i].active };
                }
                snap.hazardX = hazard.bounds.x;
                snap.hazardY = hazard.bounds.y;
                snap.hazardW = hazard.bounds.width;
                snap.hazardH = hazard.bounds.height;

                std::vector<uint8_t> snapBytes;
                SerializeStruct(snap, snapBytes);
                uint32_t seq = sessionSnapshotSeq[sessionEntry]++;
                for (int i = 0; i < 2; i++) {
                    if (session->slots[i].state == SlotState::Connected) {
                        SendUnreliable(socket, session->slots[i].clientIp, session->slots[i].clientPort, seq, MessageType::Snapshot, snapBytes.data(), snapBytes.size());
                    }
                }

                // Retransmit any unacked reliable messages for this session's slots
                for (int i = 0; i < 2; i++) {
                    if (session->slots[i].state != SlotState::Connected) continue;
                    auto toResend = session->slots[i].reliableSender.GetMessagesToRetransmit(now, 0.1);
                    for (auto& msg : toResend) {
                        PacketHeader resendHeader{ 1, msg.first, (uint16_t)msg.second.size() };
                        std::vector<uint8_t> packet;
                        SerializeStruct(resendHeader, packet);
                        packet.insert(packet.end(), msg.second.begin(), msg.second.end());
                        socket.SendTo(session->slots[i].clientIp, session->slots[i].clientPort, packet.data(), packet.size());
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
