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

    // Case 1: fresh session created via empty name, first connector gets slot 0
    ConnectOutcome outcome1 = manager.HandleConnect("", 0, "127.0.0.1", 5000, 0.0);
    assert(outcome1.result == ConnectResult::Created);
    assert(outcome1.slotIndex == 0);
    assert(outcome1.sessionToken != 0);
    std::string game1Code = outcome1.roomCode;

    // Case 2: second connector joins the open slot via the generated code
    ConnectOutcome outcome2 = manager.HandleConnect(game1Code, 0, "127.0.0.1", 5001, 0.0);
    assert(outcome2.result == ConnectResult::Joined);
    assert(outcome2.slotIndex == 1);
    assert(outcome2.sessionToken != 0);
    assert(outcome2.sessionToken != outcome1.sessionToken);

    // Third and fourth connectors fill the remaining slots (sessions now hold 4 slots)
    ConnectOutcome outcome3a = manager.HandleConnect(game1Code, 0, "127.0.0.1", 5006, 0.0);
    assert(outcome3a.result == ConnectResult::Joined);
    assert(outcome3a.slotIndex == 2);

    ConnectOutcome outcome3b = manager.HandleConnect(game1Code, 0, "127.0.0.1", 5007, 0.0);
    assert(outcome3b.result == ConnectResult::Joined);
    assert(outcome3b.slotIndex == 3);

    // Case 3: fifth connector rejected, session full (now that sessions hold 4 slots)
    ConnectOutcome outcome3 = manager.HandleConnect(game1Code, 0, "127.0.0.1", 5002, 0.0);
    assert(outcome3.result == ConnectResult::Rejected);
    assert(outcome3.rejectReason == RejectReason::SessionFull);

    // Simulate slot 0 going disconnected (timeout), then reconnecting
    Session* session = manager.GetSession(game1Code);
    assert(session != nullptr);
    session->slots[0].lastPacketAtSeconds = 0.0;
    session->slots[1].lastPacketAtSeconds = 59.0; // recent packet: must not time out alongside slot 0
    session->CheckTimeouts(60.0); // 60s elapsed -> slot 0 becomes DisconnectedPending
    assert(session->slots[0].state == SlotState::DisconnectedPending);
    assert(session->slots[1].state == SlotState::Connected); // slot 1 untouched (recent packet)

    // Case 4: reconnect with the correct token succeeds, resumes the same slot
    ConnectOutcome outcome4 = manager.HandleConnect(game1Code, outcome1.sessionToken, "127.0.0.1", 5003, 61.0);
    assert(outcome4.result == ConnectResult::Reconnected);
    assert(outcome4.slotIndex == 0);
    assert(outcome4.sessionToken == outcome1.sessionToken);
    assert(session->slots[0].state == SlotState::Connected);

    // Reconnect with an EMPTY session name (what a create-path client actually
    // sends, since NetClient::Reconnect() resends whatever name was originally
    // used to connect — a room creator's original name was ""): must resume via
    // token lookup (which scans all sessions), not fall through to the
    // empty-name create path and spawn an orphan room.
    session->slots[0].lastPacketAtSeconds = 0.0;
    session->CheckTimeouts(60.0);
    ConnectOutcome outcomeEmptyReconnect = manager.HandleConnect("", outcome1.sessionToken, "127.0.0.1", 5005, 61.0);
    assert(outcomeEmptyReconnect.result == ConnectResult::Reconnected);
    assert(outcomeEmptyReconnect.roomCode == game1Code);
    assert(session->slots[0].state == SlotState::Connected);

    // Case 5: reconnect with a token that doesn't match anything falls back to fresh-connect logic
    // (session is now full again after the reconnect above, so this should be rejected)
    ConnectOutcome outcome5 = manager.HandleConnect(game1Code, 999999, "127.0.0.1", 5004, 61.0);
    assert(outcome5.result == ConnectResult::Rejected);

    // Full timeout-to-Empty cycle: disconnect slot 0 again, let both timeouts elapse, slot becomes Empty
    session->slots[0].lastPacketAtSeconds = 100.0;
    session->CheckTimeouts(160.0); // 60s -> DisconnectedPending
    assert(session->slots[0].state == SlotState::DisconnectedPending);
    session->CheckTimeouts(220.0); // another 60s (120s total from lastPacketAtSeconds) -> Empty
    assert(session->slots[0].state == SlotState::Empty);

    // A separate create call generates an independent session
    ConnectOutcome outcomeOther = manager.HandleConnect("", 0, "127.0.0.1", 6000, 0.0);
    assert(outcomeOther.result == ConnectResult::Created);
    assert(outcomeOther.slotIndex == 0);

    // Create-room path: empty name generates a 6-digit code
    ConnectOutcome createOutcome = manager.HandleConnect("", 0, "127.0.0.1", 7000, 300.0);
    assert(createOutcome.result == ConnectResult::Created);
    assert(createOutcome.roomCode.size() == 6);
    for (char c : createOutcome.roomCode) {
        assert(c >= '0' && c <= '9');
    }

    // Two sequential create requests generate different codes (overwhelmingly likely; not a hard guarantee)
    ConnectOutcome createOutcome2 = manager.HandleConnect("", 0, "127.0.0.1", 7001, 300.0);
    assert(createOutcome2.result == ConnectResult::Created);
    assert(createOutcome2.roomCode != createOutcome.roomCode);

    // Joining a non-empty name that doesn't exist is rejected with RoomNotFound
    ConnectOutcome joinMissing = manager.HandleConnect("999999", 0, "127.0.0.1", 7002, 300.0);
    assert(joinMissing.result == ConnectResult::Rejected);
    assert(joinMissing.rejectReason == RejectReason::RoomNotFound);

    // Joining a code that WAS created via the create path succeeds via existing join logic
    ConnectOutcome joinCreated = manager.HandleConnect(createOutcome.roomCode, 0, "127.0.0.1", 7003, 300.0);
    assert(joinCreated.result == ConnectResult::Joined);
    assert(joinCreated.slotIndex == 1);
    assert(joinCreated.roomCode == createOutcome.roomCode);

    // 4-player fill: a fresh room accepts exactly 4 joiners, rejects the 5th
    ConnectOutcome fill1 = manager.HandleConnect("", 0, "127.0.0.1", 8000, 400.0);
    assert(fill1.result == ConnectResult::Created);
    assert(fill1.slotIndex == 0);
    std::string fillCode = fill1.roomCode;

    ConnectOutcome fill2 = manager.HandleConnect(fillCode, 0, "127.0.0.1", 8001, 400.0);
    assert(fill2.result == ConnectResult::Joined);
    assert(fill2.slotIndex == 1);

    ConnectOutcome fill3 = manager.HandleConnect(fillCode, 0, "127.0.0.1", 8002, 400.0);
    assert(fill3.result == ConnectResult::Joined);
    assert(fill3.slotIndex == 2);

    ConnectOutcome fill4 = manager.HandleConnect(fillCode, 0, "127.0.0.1", 8003, 400.0);
    assert(fill4.result == ConnectResult::Joined);
    assert(fill4.slotIndex == 3);

    // 5th joiner rejected: all 4 slots full
    ConnectOutcome fill5 = manager.HandleConnect(fillCode, 0, "127.0.0.1", 8004, 400.0);
    assert(fill5.result == ConnectResult::Rejected);
    assert(fill5.rejectReason == RejectReason::SessionFull);
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
        for (int i = 0; i < 4; i++) {
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
    std::map<std::string, float[4]> sessionHazardCarry;
    std::map<std::string, uint32_t> sessionSnapshotSeq;
    std::map<std::string, bool[4]> pendingAttack;
    std::map<std::string, bool[4]> pendingInteract;

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
                            Session* session = sessionManager.GetSession(outcome.roomCode);
                            if (sessionWorldItems.find(outcome.roomCode) == sessionWorldItems.end()) {
                                sessionWorldItems[outcome.roomCode] = {
                                    WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
                                    WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
                                };
                                for (int i = 0; i < 4; i++) {
                                    sessionHazardCarry[outcome.roomCode][i] = 0.0f;
                                }
                                sessionSnapshotSeq[outcome.roomCode] = 1;
                            }

                            WelcomeMsg welcome{
                                (uint8_t)outcome.slotIndex, outcome.sessionToken,
                                Player::kMaxHp, Player::kDownedDuration, Player::kDeathRespawnDelay,
                                Player::kReviveHp, Player::kRespawnHp, Player::kAttackCooldown,
                                Player::kAttackRange, Player::kAttackDamage, Player::kChannelDuration,
                                kHazardDamagePerSecond, Inventory::kCapacity, {}
                            };
                            std::strncpy(welcome.roomCode, outcome.roomCode.c_str(), sizeof(welcome.roomCode) - 1);
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
                                        if (deliveredReq.targetSlot < 4) {
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

                std::vector<WorldItem>& items = sessionWorldItems[sessionEntry];
                float* hazardCarry = sessionHazardCarry[sessionEntry];
                bool* attack = pendingAttack.count(sessionEntry) ? pendingAttack[sessionEntry] : nullptr;
                bool* interact = pendingInteract.count(sessionEntry) ? pendingInteract[sessionEntry] : nullptr;

                // Only players occupying a genuinely Connected slot are simulated.
                // Empty and DisconnectedPending slots are frozen: no movement, pickup,
                // attack, revive, hazard damage, or timer updates.
                bool active[4];
                for (int i = 0; i < 4; i++) {
                    active[i] = session->slots[i].state == SlotState::Connected;
                }

                // Pickup: each active, Alive player who isn't currently a valid revive
                // channel target for anyone else may pick up an item. (A player who could
                // instead be revived should channel-revive, not pick up items, mirroring
                // the original 2-player behavior's "!canRevive" gate.)
                if (interact) {
                    for (int i = 0; i < 4; i++) {
                        if (!active[i] || session->slots[i].player.state != PlayerState::Alive || !interact[i]) continue;
                        bool isRevivable = false;
                        for (int j = 0; j < 4; j++) {
                            if (i == j || !active[j]) continue;
                            if (session->slots[j].player.state == PlayerState::Downed &&
                                DistanceBetween(session->slots[i].player.position, session->slots[j].player.position) <= kReviveRange) {
                                isRevivable = true;
                                break;
                            }
                        }
                        if (isRevivable) continue;
                        for (auto& item : items) { if (TryPickup(item, session->slots[i].player.position, session->slots[i].player.inventory, kPickupRadius)) break; }
                    }
                }

                // Attack: each active player with attackPressed hits the nearest active
                // opponent in range (TryAttack itself checks range/cooldown; this loop just
                // tries each active opponent in slot order and stops at the first hit, matching
                // the spirit of the original one-target 2-player check — with more than 2
                // players TryAttack's own range check means only an opponent actually in range
                // is affected, so trying all of them in order is safe and won't multi-hit
                // since TryAttack sets a cooldown on the attacker after the first success).
                if (attack) {
                    for (int i = 0; i < 4; i++) {
                        if (!active[i] || !attack[i]) continue;
                        for (int j = 0; j < 4; j++) {
                            if (i == j || !active[j]) continue;
                            if (TryAttack(session->slots[i].player, session->slots[j].player)) break;
                        }
                        attack[i] = false;
                    }
                }

                for (int i = 0; i < 4; i++) {
                    if (active[i]) UpdateAttackCooldown(session->slots[i].player, dt);
                }

                // Revive: every active pair is checked (i channels revive on j). This is an
                // O(4x4) loop, trivially cheap, and correctly generalizes the original
                // 2-player "each checks the other" logic to any number of active players.
                if (interact) {
                    for (int i = 0; i < 4; i++) {
                        if (!active[i]) continue;
                        for (int j = 0; j < 4; j++) {
                            if (i == j || !active[j]) continue;
                            UpdateRevive(session->slots[i].player, session->slots[j].player, interact[i], dt, kReviveRange);
                        }
                    }
                }

                for (int i = 0; i < 4; i++) {
                    if (active[i]) ApplyHazardDamage(hazard, session->slots[i].player, dt, hazardCarry[i]);
                }

                for (int i = 0; i < 4; i++) {
                    if (active[i]) session->slots[i].player.UpdateTimers(dt);
                }

                // state value 3 = "absent" (slot not Connected): not a real PlayerState,
                // repurposed on the wire so an inactive slot renders as not-present
                // instead of a fully-visible phantom player, without changing the
                // PlayerSnapshot layout.
                static constexpr uint8_t kSnapshotStateAbsent = 3;

                SnapshotMsg snap{};
                for (int i = 0; i < 4; i++) {
                    Player& p = session->slots[i].player;
                    snap.players[i] = PlayerSnapshot{ p.position.x, p.position.y, p.hp, active[i] ? (uint8_t)p.state : kSnapshotStateAbsent, p.inventory.Count(ItemType::RevivePotion), p.channelTimer };
                }
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
                for (int i = 0; i < 4; i++) {
                    if (session->slots[i].state == SlotState::Connected) {
                        SendUnreliable(socket, session->slots[i].clientIp, session->slots[i].clientPort, seq, MessageType::Snapshot, snapBytes.data(), snapBytes.size());
                    }
                }

                // Retransmit any unacked reliable messages for this session's slots
                for (int i = 0; i < 4; i++) {
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
