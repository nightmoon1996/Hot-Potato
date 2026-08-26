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

    const double tickInterval = 1.0 / 60.0;
    double lastTick = NowSeconds();

    uint8_t recvBuffer[1024];

    while (true) {
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
                        double now = NowSeconds();
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
                    // For all other message types, find which session/slot this address belongs to.
                    // (A small linear scan is fine at this project's scale: at most a handful of
                    // sessions, 2 slots each.)
                    // This requires iterating sessionManager's sessions, so we expose a helper loop here
                    // rather than adding a by-address index to SessionManager (out of scope for this task).
                }
            }
        }

        // --- Fixed 60Hz simulation tick ---
        double now = NowSeconds();
        if (now - lastTick >= tickInterval) {
            float dt = (float)(now - lastTick);
            lastTick = now;
            sessionManager.CheckAllTimeouts(now);
            // Per-session simulation and snapshot broadcast happens here in Task 7's
            // continuation of this file (input processing, movement, combat, revive,
            // hazard, timers, snapshot send) — see Task 7.
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
