#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <string>
#include "../shared/Geometry.h" // only for Vector2/Rectangle math types, no window/raylib linking

#include "../shared/Protocol.h"
#include "../shared/Serialize.h"
#include "../shared/ReliableChannel.h"
#include "../shared/Socket.h"
#include "SessionManager.h"
#include "Item.h"
#include "Hazard.h"
#include "Combat.h"
#include "DebugActions.h"

void SmokeTestItems() {
    Inventory inv;
    assert(inv.Count(ItemType::RevivePotion) == 0);

    assert(inv.Add(ItemType::RevivePotion) == true);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Add(ItemType::RevivePotion) == true);
    assert(inv.Count(ItemType::RevivePotion) == 2);

    assert(inv.Remove(ItemType::RevivePotion, 1) == true);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Remove(ItemType::RevivePotion, 5) == false);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Remove(ItemType::RevivePotion, 1) == true);
    assert(inv.Count(ItemType::RevivePotion) == 0);
    assert(inv.Slots().size() == 0);

    // Pickup logic
    WorldItem item{ {100.0f, 100.0f}, ItemType::RevivePotion, true };
    Inventory pickupInv;

    // Too far away: no pickup
    bool pickedFar = TryPickup(item, Vector2{500.0f, 500.0f}, pickupInv, 24.0f);
    assert(pickedFar == false);
    assert(item.active == true);
    assert(pickupInv.Count(ItemType::RevivePotion) == 0);

    // Within radius: picks up
    bool pickedNear = TryPickup(item, Vector2{105.0f, 100.0f}, pickupInv, 24.0f);
    assert(pickedNear == true);
    assert(item.active == false);
    assert(pickupInv.Count(ItemType::RevivePotion) == 1);

    // Already inactive: no second pickup
    bool pickedAgain = TryPickup(item, Vector2{105.0f, 100.0f}, pickupInv, 24.0f);
    assert(pickedAgain == false);
    assert(pickupInv.Count(ItemType::RevivePotion) == 1);
}

void SmokeTestPlayerStateMachine() {
    Player p(Vector2{0.0f, 0.0f});
    assert(p.state == PlayerState::Alive);
    assert(p.hp == Player::kMaxHp);

    // Damage that doesn't kill
    p.TakeDamage(30);
    assert(p.state == PlayerState::Alive);
    assert(p.hp == 70);

    // Damage that downs
    p.TakeDamage(100);
    assert(p.state == PlayerState::Downed);
    assert(p.hp == 0);
    assert(p.downedTimer == Player::kDownedDuration);

    // Damage while downed has no effect
    p.TakeDamage(10);
    assert(p.state == PlayerState::Downed);

    // Downed timer expiring kills
    p.UpdateTimers(Player::kDownedDuration + 1.0f);
    assert(p.state == PlayerState::Dead);
    assert(p.deathRespawnTimer == Player::kDeathRespawnDelay);

    // Death respawn timer expiring respawns
    p.UpdateTimers(Player::kDeathRespawnDelay + 1.0f);
    assert(p.state == PlayerState::Alive);
    assert(p.hp == Player::kRespawnHp);
    assert(p.position.x == p.spawnPoint.x && p.position.y == p.spawnPoint.y);

    // Revive from downed
    Player p2(Vector2{0.0f, 0.0f});
    p2.ForceDown();
    assert(p2.state == PlayerState::Downed);
    p2.ReviveFromDowned();
    assert(p2.state == PlayerState::Alive);
    assert(p2.hp == Player::kReviveHp);
}

void SmokeTestHazard() {
    HazardZone zone{ Rectangle{0.0f, 0.0f, 100.0f, 100.0f} };
    Player p(Vector2{50.0f, 50.0f}); // inside the zone
    float carry = 0.0f;

    // 1 second at 5 HP/sec should deal 5 damage total, spread across calls
    for (int i = 0; i < 60; i++) {
        ApplyHazardDamage(zone, p, 1.0f / 60.0f, carry);
    }
    assert(p.hp <= Player::kMaxHp - 4);

    // Player outside the zone takes no damage
    Player p2(Vector2{500.0f, 500.0f});
    float carry2 = 0.0f;
    ApplyHazardDamage(zone, p2, 1.0f, carry2);
    assert(p2.hp == Player::kMaxHp);
}

void SmokeTestCombatAndRevive() {
    // Attack: in range, off cooldown -> succeeds
    Player attacker(Vector2{0.0f, 0.0f});
    Player target(Vector2{10.0f, 0.0f});
    bool hit = TryAttack(attacker, target);
    assert(hit == true);
    assert(target.hp == Player::kMaxHp - Player::kAttackDamage);
    assert(attacker.attackCooldownTimer == Player::kAttackCooldown);

    // Immediately again: on cooldown -> fails
    bool hit2 = TryAttack(attacker, target);
    assert(hit2 == false);
    assert(target.hp == Player::kMaxHp - Player::kAttackDamage); // unchanged

    // Out of range -> fails
    Player far(Vector2{1000.0f, 1000.0f});
    Player attacker2(Vector2{0.0f, 0.0f});
    bool hit3 = TryAttack(attacker2, far);
    assert(hit3 == false);

    // Revive: full sequence
    Player reviver(Vector2{0.0f, 0.0f});
    Player downed(Vector2{5.0f, 0.0f});
    downed.ForceDown();
    reviver.inventory.Add(ItemType::RevivePotion);

    // Not enough time yet: still downed
    bool completed1 = UpdateRevive(reviver, downed, true, 1.0f, 32.0f);
    assert(completed1 == false);
    assert(downed.state == PlayerState::Downed);

    // Cross the threshold
    bool completed2 = UpdateRevive(reviver, downed, true, 1.5f, 32.0f);
    assert(completed2 == true);
    assert(downed.state == PlayerState::Alive);
    assert(downed.hp == Player::kReviveHp);
    assert(reviver.inventory.Count(ItemType::RevivePotion) == 0);

    // Releasing the key resets progress
    Player reviver2(Vector2{0.0f, 0.0f});
    Player downed2(Vector2{5.0f, 0.0f});
    downed2.ForceDown();
    reviver2.inventory.Add(ItemType::RevivePotion);
    UpdateRevive(reviver2, downed2, true, 1.0f, 32.0f);
    assert(reviver2.channelTimer > 0.0f);
    UpdateRevive(reviver2, downed2, false, 0.0f, 32.0f); // key released
    assert(reviver2.channelTimer == 0.0f);
}

void SmokeTestReliableChannel() {
    // Sender: track, ack, retransmit
    ReliableSender sender;
    uint32_t seq1 = sender.NextSeq();
    uint32_t seq2 = sender.NextSeq();
    assert(seq1 == 1);
    assert(seq2 == 2);

    std::vector<uint8_t> payload1{ 0xAA };
    std::vector<uint8_t> payload2{ 0xBB };
    sender.TrackUnacked(seq1, payload1, 0.0);
    sender.TrackUnacked(seq2, payload2, 0.0);

    // Not enough time has passed: no retransmits yet
    auto toRetransmit1 = sender.GetMessagesToRetransmit(0.05, 0.1);
    assert(toRetransmit1.size() == 0);

    // 0.1s later: both should be flagged for retransmit
    auto toRetransmit2 = sender.GetMessagesToRetransmit(0.1, 0.1);
    assert(toRetransmit2.size() == 2);

    // Ack seq1: it should no longer be retransmitted
    sender.OnAckReceived(seq1);
    auto toRetransmit3 = sender.GetMessagesToRetransmit(0.3, 0.1);
    assert(toRetransmit3.size() == 1);
    assert(toRetransmit3[0].first == seq2);

    // Receiver: in-order delivery
    ReliableReceiver receiver;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ready;

    bool delivered1 = receiver.TryDeliverInOrder(1, payload1, ready);
    assert(delivered1 == true);
    assert(ready.size() == 1);
    assert(ready[0].first == 1);

    // Out-of-order arrival (seq 3 before seq 2): buffered, nothing new ready
    ready.clear();
    bool delivered2 = receiver.TryDeliverInOrder(3, payload1, ready);
    assert(delivered2 == false);
    assert(ready.size() == 0);

    // Now seq 2 arrives: both 2 and 3 become ready, in order
    ready.clear();
    bool delivered3 = receiver.TryDeliverInOrder(2, payload2, ready);
    assert(delivered3 == true);
    assert(ready.size() == 2);
    assert(ready[0].first == 2);
    assert(ready[1].first == 3);

    // Duplicate of already-delivered seq 1: not re-delivered
    ready.clear();
    bool delivered4 = receiver.TryDeliverInOrder(1, payload1, ready);
    assert(delivered4 == false);
    assert(ready.size() == 0);
}

void SmokeTestSerialization() {
    InputMsg input{ 1.0f, -1.0f, true, false };
    std::vector<uint8_t> buffer;
    SerializeStruct(input, buffer);
    assert(buffer.size() == sizeof(InputMsg));

    InputMsg roundTripped{};
    bool ok = DeserializeStruct(buffer.data(), buffer.size(), roundTripped);
    assert(ok == true);
    assert(roundTripped.moveX == 1.0f);
    assert(roundTripped.moveY == -1.0f);
    assert(roundTripped.interactHeld == true);
    assert(roundTripped.attackPressed == false);

    // Too-short buffer fails
    InputMsg failed{};
    bool notOk = DeserializeStruct(buffer.data(), 2, failed);
    assert(notOk == false);

    // SnapshotMsg round trip (larger, nested struct)
    SnapshotMsg snap{};
    snap.players[0] = PlayerSnapshot{ 10.0f, 20.0f, 100, 0, 2, 0.5f };
    snap.players[1] = PlayerSnapshot{ 30.0f, 40.0f, 0, 1, 0, 0.0f };
    snap.items[0] = WorldItemSnapshot{ 5.0f, 5.0f, true };
    snap.items[1] = WorldItemSnapshot{ 6.0f, 6.0f, false };
    snap.hazardX = 100.0f;
    snap.hazardY = 200.0f;
    snap.hazardW = 50.0f;
    snap.hazardH = 60.0f;

    std::vector<uint8_t> snapBuffer;
    SerializeStruct(snap, snapBuffer);
    SnapshotMsg snapRoundTripped{};
    bool snapOk = DeserializeStruct(snapBuffer.data(), snapBuffer.size(), snapRoundTripped);
    assert(snapOk == true);
    assert(snapRoundTripped.players[0].posX == 10.0f);
    assert(snapRoundTripped.players[0].hp == 100);
    assert(snapRoundTripped.players[1].state == 1);
    assert(snapRoundTripped.items[0].active == true);
    assert(snapRoundTripped.items[1].active == false);
    assert(snapRoundTripped.hazardW == 50.0f);
}

void SmokeTestSessionManager() {
    SessionManager manager;

    // Case 1: fresh session created, first connector gets slot 0
    ConnectOutcome outcome1 = manager.HandleConnect("game1", 0, "127.0.0.1", 5000, 0.0);
    assert(outcome1.result == ConnectResult::Created);
    assert(outcome1.slotIndex == 0);
    assert(outcome1.sessionToken != 0);

    // Case 2: second connector joins the open slot
    ConnectOutcome outcome2 = manager.HandleConnect("game1", 0, "127.0.0.1", 5001, 0.0);
    assert(outcome2.result == ConnectResult::Joined);
    assert(outcome2.slotIndex == 1);
    assert(outcome2.sessionToken != 0);
    assert(outcome2.sessionToken != outcome1.sessionToken);

    // Case 3: third connector rejected, session full
    ConnectOutcome outcome3 = manager.HandleConnect("game1", 0, "127.0.0.1", 5002, 0.0);
    assert(outcome3.result == ConnectResult::Rejected);
    assert(outcome3.rejectReason == RejectReason::SessionFull);

    // Simulate slot 0 going disconnected (timeout), then reconnecting
    Session* session = manager.GetSession("game1");
    assert(session != nullptr);
    session->slots[0].lastPacketAtSeconds = 0.0;
    session->slots[1].lastPacketAtSeconds = 59.0; // recent packet: must not time out alongside slot 0
    session->CheckTimeouts(60.0); // 60s elapsed -> slot 0 becomes DisconnectedPending
    assert(session->slots[0].state == SlotState::DisconnectedPending);
    assert(session->slots[1].state == SlotState::Connected); // slot 1 untouched (recent packet)

    // Case 4: reconnect with the correct token succeeds, resumes the same slot
    ConnectOutcome outcome4 = manager.HandleConnect("game1", outcome1.sessionToken, "127.0.0.1", 5003, 61.0);
    assert(outcome4.result == ConnectResult::Reconnected);
    assert(outcome4.slotIndex == 0);
    assert(outcome4.sessionToken == outcome1.sessionToken);
    assert(session->slots[0].state == SlotState::Connected);

    // Case 5: reconnect with a token that doesn't match anything falls back to fresh-connect logic
    // (session "game1" is now full again after the reconnect above, so this should be rejected)
    ConnectOutcome outcome5 = manager.HandleConnect("game1", 999999, "127.0.0.1", 5004, 61.0);
    assert(outcome5.result == ConnectResult::Rejected);

    // Full timeout-to-Empty cycle: disconnect slot 0 again, let both timeouts elapse, slot becomes Empty
    session->slots[0].lastPacketAtSeconds = 100.0;
    session->CheckTimeouts(160.0); // 60s -> DisconnectedPending
    assert(session->slots[0].state == SlotState::DisconnectedPending);
    session->CheckTimeouts(220.0); // another 60s (120s total from lastPacketAtSeconds) -> Empty
    assert(session->slots[0].state == SlotState::Empty);

    // A new session name creates an independent session
    ConnectOutcome outcomeOther = manager.HandleConnect("game2", 0, "127.0.0.1", 6000, 0.0);
    assert(outcomeOther.result == ConnectResult::Created);
    assert(outcomeOther.slotIndex == 0);
}

void RunAllSmokeTests() {
    SmokeTestSerialization();
    std::printf("SmokeTestSerialization passed\n");

    SmokeTestReliableChannel();
    std::printf("SmokeTestReliableChannel passed\n");

    SmokeTestItems();
    std::printf("SmokeTestItems passed\n");

    SmokeTestPlayerStateMachine();
    std::printf("SmokeTestPlayerStateMachine passed\n");

    SmokeTestHazard();
    std::printf("SmokeTestHazard passed\n");

    SmokeTestCombatAndRevive();
    std::printf("SmokeTestCombatAndRevive passed\n");

    SmokeTestSessionManager();
    std::printf("SmokeTestSessionManager passed\n");

    std::printf("All smoke tests passed\n");
}

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

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--test") {
        RunAllSmokeTests();
        return 0;
    }
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
        // Drain every packet waiting in the OS socket buffer each pass, not just one:
        // with the 1ms sleep below, a single-packet-per-pass loop falls behind under
        // the input rate of two connected clients, growing input lag over time.
        std::string fromIp;
        uint16_t fromPort;
        int received;
        while ((received = socket.ReceiveFrom(recvBuffer, sizeof(recvBuffer), fromIp, fromPort)) > (int)sizeof(PacketHeader)) {
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
                                if (slot.player.state == PlayerState::Alive) {
                                    slot.player.position.x += input.moveX * kMoveSpeed * (float)tickInterval;
                                    slot.player.position.y += input.moveY * kMoveSpeed * (float)tickInterval;
                                }
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

                // Only players occupying a genuinely Connected slot are simulated.
                // Empty and DisconnectedPending slots are frozen: no movement, pickup,
                // attack, revive, hazard damage, or timer updates.
                bool p0Active = session->slots[0].state == SlotState::Connected;
                bool p1Active = session->slots[1].state == SlotState::Connected;

                // An inactive opponent cannot be revive-targeted (they're not really
                // "there" from a live-multiplayer standpoint), so an active player
                // checking for a valid revive target must also require the opponent
                // to be active.
                bool p0CanRevive = p1Active && p1.state == PlayerState::Downed && DistanceBetween(p0.position, p1.position) <= kReviveRange;
                bool p1CanRevive = p0Active && p0.state == PlayerState::Downed && DistanceBetween(p1.position, p0.position) <= kReviveRange;

                if (interact) {
                    if (p0Active && p0.state == PlayerState::Alive && interact[0] && !p0CanRevive) {
                        for (auto& item : items) { if (TryPickup(item, p0.position, p0.inventory, kPickupRadius)) break; }
                    }
                    if (p1Active && p1.state == PlayerState::Alive && interact[1] && !p1CanRevive) {
                        for (auto& item : items) { if (TryPickup(item, p1.position, p1.inventory, kPickupRadius)) break; }
                    }
                }
                if (attack) {
                    if (p0Active && attack[0]) { if (p1Active) TryAttack(p0, p1); attack[0] = false; }
                    if (p1Active && attack[1]) { if (p0Active) TryAttack(p1, p0); attack[1] = false; }
                }
                if (p0Active) UpdateAttackCooldown(p0, dt);
                if (p1Active) UpdateAttackCooldown(p1, dt);

                bool p0InteractHeld = interact ? interact[0] : false;
                bool p1InteractHeld = interact ? interact[1] : false;
                if (p0Active && p1Active) {
                    UpdateRevive(p0, p1, p0InteractHeld, dt, kReviveRange);
                    UpdateRevive(p1, p0, p1InteractHeld, dt, kReviveRange);
                }

                if (p0Active) ApplyHazardDamage(hazard, p0, dt, hazardCarry[0]);
                if (p1Active) ApplyHazardDamage(hazard, p1, dt, hazardCarry[1]);

                if (p0Active) p0.UpdateTimers(dt);
                if (p1Active) p1.UpdateTimers(dt);

                // state value 3 = "absent" (slot not Connected): not a real PlayerState,
                // repurposed on the wire so an inactive slot renders as not-present
                // instead of a fully-visible phantom player, without changing the
                // PlayerSnapshot layout.
                static constexpr uint8_t kSnapshotStateAbsent = 3;

                SnapshotMsg snap{};
                snap.players[0] = PlayerSnapshot{ p0.position.x, p0.position.y, p0.hp, p0Active ? (uint8_t)p0.state : kSnapshotStateAbsent, p0.inventory.Count(ItemType::RevivePotion), p0.channelTimer };
                snap.players[1] = PlayerSnapshot{ p1.position.x, p1.position.y, p1.hp, p1Active ? (uint8_t)p1.state : kSnapshotStateAbsent, p1.inventory.Count(ItemType::RevivePotion), p1.channelTimer };
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
