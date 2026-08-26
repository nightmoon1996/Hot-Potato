#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <string>
#include <cmath>
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
#include "HotPotato.h"
#include "MatchState.h"
#include "Dash.h"

// Defined above main(); forward-declared here so the smoke tests can exercise it.
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds, MatchState& match);
static void StartNewMatch(Session& session, MatchState& match, HotPotato& potato, float* chargeTimer);

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

// Regression test for the 3+ active player revive bug: channelTimer lives on the
// reviver, so a second UpdateRevive call for the same reviver against a non-Downed
// player later in the same tick used to reset it to 0, making a revive impossible to
// complete with 3 or more active players. Layout below is the exact bug shape:
// reviver slot 0, Downed target slot 1, irrelevant Alive player slot 2 (processed
// immediately after the target in ascending slot order).
void SmokeTestSimulationTick() {
    Session session;
    for (int i = 0; i < 3; i++) {
        session.slots[i].state = SlotState::Connected;
        session.slots[i].player = Player(kSpawnPoints[i]);
    }
    session.slots[3].state = SlotState::Empty;

    // Reviver (slot 0) next to the Downed target (slot 1), holding a potion.
    session.slots[0].player.position = Vector2{ 100.0f, 100.0f };
    session.slots[0].player.inventory.Add(ItemType::RevivePotion);

    session.slots[1].player.position = Vector2{ 105.0f, 100.0f };
    session.slots[1].player.ForceDown();
    assert(session.slots[1].player.state == PlayerState::Downed);

    // Third player: Alive, nearby, irrelevant to the revive. Before the fix, this
    // player's iteration clobbered slot 0's channelTimer every tick.
    session.slots[2].player.position = Vector2{ 110.0f, 100.0f };

    // Hazard placed far away so it can't interfere with HP/state.
    HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
    std::vector<WorldItem> items;
    float hazardCarry[kMaxPlayersPerSession] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool attack[kMaxPlayersPerSession] = { false, false, false, false };
    bool interact[kMaxPlayersPerSession] = { true, false, false, false };
    HotPotato potato{};
    float chargeTimer[kMaxPlayersPerSession] = { 0.0f, 0.0f, 0.0f, 0.0f };
    InputMsg latestInputs[kMaxPlayersPerSession] = {};
    Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
    MatchState match{};

    // ~2.5s at 60Hz: comfortably past Player::kChannelDuration (2.0s) but well under
    // kDownedDuration (15s), so the target can't die of the downed timer instead.
    const float dt = 1.0f / 60.0f;
    for (int tick = 0; tick < 150; tick++) {
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match);
        assert(active[0] && active[1] && active[2]);
        assert(active[3] == false);
    }

    assert(session.slots[1].player.state == PlayerState::Alive);
    assert(session.slots[1].player.hp == Player::kReviveHp);
    assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 0);
    assert(session.slots[2].player.state == PlayerState::Alive); // bystander untouched

    // Negative control: with no potion, the same 3-player setup must NOT revive.
    Session noPotion;
    for (int i = 0; i < 3; i++) {
        noPotion.slots[i].state = SlotState::Connected;
        noPotion.slots[i].player = Player(kSpawnPoints[i]);
    }
    noPotion.slots[0].player.position = Vector2{ 100.0f, 100.0f };
    noPotion.slots[1].player.position = Vector2{ 105.0f, 100.0f };
    noPotion.slots[1].player.ForceDown();
    noPotion.slots[2].player.position = Vector2{ 110.0f, 100.0f };
    float carry2[kMaxPlayersPerSession] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool attack2[kMaxPlayersPerSession] = { false, false, false, false };
    HotPotato potato2{};
    float chargeTimer2[kMaxPlayersPerSession] = { 0.0f, 0.0f, 0.0f, 0.0f };
    InputMsg latestInputs2[kMaxPlayersPerSession] = {};
    MatchState match2{};
    for (int tick = 0; tick < 150; tick++) {
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(noPotion, items, hazard, carry2, attack2, interact, dt, active,
                             potato2, chargeTimer2, latestInputs2, courtBounds, match2);
    }
    assert(noPotion.slots[1].player.state == PlayerState::Downed);
    assert(noPotion.slots[0].player.channelTimer == 0.0f);
}

void SmokeTestHotPotato() {
    // Force scales with charge, clamped at max
    assert(ComputeThrowForce(0.0f) == kMinThrowForce);
    float midForce = ComputeThrowForce(kMaxChargeDuration / 2.0f);
    assert(midForce > kMinThrowForce && midForce < kMaxThrowForce);
    assert(ComputeThrowForce(kMaxChargeDuration) == kMaxThrowForce);
    assert(ComputeThrowForce(kMaxChargeDuration * 2.0f) == kMaxThrowForce); // overcharge clamps

    // Timer shrinks per catch, floors correctly
    assert(ComputeExplodeTimerForCatch(0) == kPotatoStartTimer);
    assert(ComputeExplodeTimerForCatch(1) == kPotatoStartTimer - kPotatoTimerShrink);
    assert(ComputeExplodeTimerForCatch(100) == kPotatoTimerFloor); // deep into the shrink, floored

    // Drag reduces speed over time, never reverses direction
    HotPotato p;
    p.velocity = Vector2{100.0f, 0.0f};
    float prevSpeed = 100.0f;
    for (int i = 0; i < 60; i++) {
        ApplyPotatoDrag(p, 1.0f / 60.0f);
        float speed = std::sqrt(p.velocity.x * p.velocity.x + p.velocity.y * p.velocity.y);
        assert(speed <= prevSpeed);
        assert(p.velocity.x >= 0.0f); // never flips sign under pure drag
        prevSpeed = speed;
    }

    // Catch detection: finds the nearest-in-range active player, skips inactive/excluded
    Vector2 positions[4] = { {0.0f, 0.0f}, {100.0f, 100.0f}, {5.0f, 0.0f}, {0.0f, 0.0f} };
    bool activeFlags[4] = { true, true, true, false };
    int found = FindCatchTarget(Vector2{0.0f, 0.0f}, positions, activeFlags, 4, -1);
    assert(found == 0); // slot 0 exactly at potato position, within radius
    int foundExcluded = FindCatchTarget(Vector2{0.0f, 0.0f}, positions, activeFlags, 4, 0);
    // Slot 0 excluded. Slot 2 is at (5,0); distance from (0,0) is 5.0, which IS within
    // kCatchRadius (20.0f) -- so excluding slot 0 should still find slot 2, NOT -1.
    assert(foundExcluded == 2);

    // --- Full integration: charge + release + flight + catch, via SimulateSessionTick ---
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[1].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[1].player = Player(Vector2{100.0f, 0.0f});

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };

        HotPotato potato{};
        potato.held = true;
        potato.holderSlot = 0;
        potato.position = session.slots[0].player.position;
        potato.explodeTimer = ComputeExplodeTimerForCatch(0);
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};

        const float dt = 1.0f / 60.0f;

        // Charge fully (slot 0 holds down chargingThrow for kMaxChargeDuration).
        latestInputs[0].chargingThrow = true;
        int chargeTicks = (int)(kMaxChargeDuration / dt) + 2;
        for (int i = 0; i < chargeTicks; i++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
        }
        assert(chargeTimer[0] == kMaxChargeDuration);
        assert(potato.held == true); // still held, not yet released

        // Release, aiming at +X toward slot 1.
        latestInputs[0].chargingThrow = false;
        latestInputs[0].releaseThrow = true;
        latestInputs[0].aimDirX = 1.0f;
        latestInputs[0].aimDirY = 0.0f;
        {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
        }
        assert(potato.held == false);
        assert(chargeTimer[0] == 0.0f); // reset after release
        latestInputs[0].releaseThrow = false; // one-shot event, clear like a real client would

        // Simulate flight until it reaches slot 1 (at x=100) and gets caught.
        bool caught = false;
        for (int i = 0; i < 600 && !caught; i++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
            if (potato.held && potato.holderSlot == 1) caught = true;
        }
        assert(caught);
        assert(potato.holderSlot == 1);
        assert(potato.catchCount == 1);
        // The catch and the "held potato ticks down its explode timer" logic run within
        // the same SimulateSessionTick call on the tick of the catch, so by the time we
        // observe it here the timer has already been decremented by one dt from the
        // freshly-assigned ComputeExplodeTimerForCatch(1) value.
        assert(potato.explodeTimer <= ComputeExplodeTimerForCatch(1));
        assert(potato.explodeTimer > ComputeExplodeTimerForCatch(1) - dt - 0.001f);
        assert(potato.explodeTimer < kPotatoStartTimer);
    }

    // --- Explosion downs the holder and resets the round ---
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[1].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[1].player = Player(Vector2{100.0f, 0.0f});

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };

        HotPotato potato{};
        potato.held = true;
        potato.holderSlot = 0;
        potato.position = session.slots[0].player.position;
        potato.explodeTimer = 0.01f; // about to explode
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};

        const float dt = 1.0f / 60.0f;
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match);

        assert(session.slots[0].player.state == PlayerState::Downed);
        assert(potato.held == true);
        // Round-reset hands the potato to the first ALIVE active slot in ascending order.
        // Slot 0 was just downed by the explosion, so it must be skipped -- otherwise the
        // potato would loop back onto them and "explode" repeatedly with no effect.
        assert(potato.holderSlot == 1);
        assert(session.slots[1].player.state == PlayerState::Alive);
        assert(potato.catchCount == 0);
        assert(potato.explodeTimer == ComputeExplodeTimerForCatch(0));

        // Real end-to-end wiring check: the explosion round-end above must have scored
        // via ScoreRoundEnd (crediting every active, non-exploded player) and advanced
        // the round via AdvanceRoundOrEndMatch, entirely through SimulateSessionTick --
        // not just via the pure MatchState.h functions tested in isolation elsewhere.
        assert(match.roundScore[0] == 0); // slot 0 exploded: excluded from scoring
        assert(match.roundScore[1] == 1); // slot 1 was active and not excluded: scored
        assert(match.roundNumber == 2); // AdvanceRoundOrEndMatch was invoked
        assert(match.matchOver == false);
    }

    // --- Multi-round: two full throw/catch cycles, then an explosion clears stale charge ---
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[1].state = SlotState::Connected;
        session.slots[2].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[1].player = Player(Vector2{100.0f, 0.0f});
        session.slots[2].player = Player(Vector2{500.0f, 300.0f}); // far away, never catches

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };

        HotPotato potato{};
        potato.held = true;
        potato.holderSlot = 0;
        potato.position = session.slots[0].player.position;
        potato.explodeTimer = ComputeExplodeTimerForCatch(0);
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};

        const float dt = 1.0f / 60.0f;
        bool active[kMaxPlayersPerSession];

        // Two full throw -> catch cycles: 0 -> 1, then 1 -> 0.
        for (int cycle = 0; cycle < 2; cycle++) {
            int thrower = potato.holderSlot;
            assert(thrower == (cycle == 0 ? 0 : 1));
            int receiver = (thrower == 0) ? 1 : 0;
            float aimX = (thrower == 0) ? 1.0f : -1.0f;

            latestInputs[thrower].chargingThrow = true;
            for (int i = 0; i < 30; i++) {
                SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                     potato, chargeTimer, latestInputs, courtBounds, match);
            }
            assert(chargeTimer[thrower] > 0.0f);

            latestInputs[thrower].chargingThrow = false;
            latestInputs[thrower].releaseThrow = true;
            latestInputs[thrower].aimDirX = aimX;
            latestInputs[thrower].aimDirY = 0.0f;
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
            latestInputs[thrower].releaseThrow = false;

            bool caught = false;
            for (int i = 0; i < 600 && !caught; i++) {
                SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                     potato, chargeTimer, latestInputs, courtBounds, match);
                if (potato.held && potato.holderSlot == receiver) caught = true;
            }
            assert(caught);
            assert(potato.catchCount == cycle + 1);
        }

        // Potato is now held by slot 0 (after two cycles). Slot 2 charges but never
        // releases: that stale charge must not survive the round reset below.
        assert(potato.holderSlot == 0);
        chargeTimer[2] = 1.2f;
        assert(chargeTimer[2] > 0.0f);

        // Force the explosion on slot 0.
        potato.explodeTimer = 0.01f;
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match);

        assert(session.slots[0].player.state == PlayerState::Downed);
        // (a) new holder is a genuinely Alive player...
        assert(potato.held == true);
        assert(potato.holderSlot >= 0);
        assert(session.slots[potato.holderSlot].player.state == PlayerState::Alive);
        // (c) ...and it is NOT the player who was just downed.
        assert(potato.holderSlot != 0);
        // (b) stale charge cleared for every slot, including the never-released slot 2.
        assert(chargeTimer[2] == 0.0f);
        for (int i = 0; i < kMaxPlayersPerSession; i++) assert(chargeTimer[i] == 0.0f);
    }

    // --- Holder disconnects mid-hold: the potato is reclaimed, not frozen ---
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[1].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[1].player = Player(Vector2{100.0f, 0.0f});

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };

        HotPotato potato{};
        potato.held = true;
        potato.holderSlot = 0;
        potato.position = session.slots[0].player.position;
        potato.explodeTimer = ComputeExplodeTimerForCatch(0);
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};

        const float dt = 1.0f / 60.0f;
        bool active[kMaxPlayersPerSession];

        // Slot 0 drops out while still holding the potato.
        session.slots[0].state = SlotState::DisconnectedPending;
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match);

        assert(active[0] == false);
        assert(potato.held == true);
        assert(potato.holderSlot == 1); // moved to the remaining Alive+active player
        assert(session.slots[1].player.state == PlayerState::Alive);
        assert(potato.explodeTimer < ComputeExplodeTimerForCatch(0)); // fresh timer, already ticking
        assert(potato.explodeTimer > ComputeExplodeTimerForCatch(0) - dt - 0.001f);
        // Holder-vanished reclaim is NOT a scored round-end: no round score changes, no
        // round advance.
        assert(match.roundScore[0] == 0);
        assert(match.roundScore[1] == 0);
        assert(match.roundNumber == 1);
    }

    // --- Solo mode: wall bounce reflects velocity, downs nobody ---
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{980.0f, 300.0f});
        session.slots[1].state = SlotState::Empty;

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };

        HotPotato potato{};
        potato.held = true;
        potato.holderSlot = 0;
        potato.position = session.slots[0].player.position;
        potato.explodeTimer = ComputeExplodeTimerForCatch(0);
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};

        const float dt = 1.0f / 60.0f;

        // Charge fully and throw toward the right-hand wall (+X, out of bounds at x=1000).
        latestInputs[0].chargingThrow = true;
        int chargeTicks = (int)(kMaxChargeDuration / dt) + 2;
        for (int i = 0; i < chargeTicks; i++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
        }
        latestInputs[0].chargingThrow = false;
        latestInputs[0].releaseThrow = true;
        latestInputs[0].aimDirX = 1.0f;
        latestInputs[0].aimDirY = 0.0f;
        {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
        }
        latestInputs[0].releaseThrow = false;
        assert(potato.inFlight == true);
        assert(potato.velocity.x > 0.0f);

        bool bounced = false;
        for (int i = 0; i < 60 && !bounced; i++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
            assert(session.slots[0].player.state != PlayerState::Downed); // never downed in solo mode
            if (potato.velocity.x < 0.0f) bounced = true; // reflected off the +X wall
        }
        assert(bounced);
        assert(potato.position.x <= courtBounds.x + courtBounds.width + 0.01f); // clamped back inside
        assert(session.slots[0].player.state == PlayerState::Alive);
    }
}

void SmokeTestMatchState() {
    // ScoreRoundEnd credits every active player except the excluded one
    {
        MatchState match{};
        bool active[kMaxPlayersPerSession] = { true, true, true, false };
        ScoreRoundEnd(match, active, 1); // slot 1 excluded (the round's cause)
        assert(match.roundScore[0] == 1);
        assert(match.roundScore[1] == 0);
        assert(match.roundScore[2] == 1);
        assert(match.roundScore[3] == 0); // inactive, untouched
    }

    // 3 rounds played, clear single winner
    {
        MatchState match{};
        bool active[kMaxPlayersPerSession] = { true, true, false, false };
        // Round 1: slot 1 is the cause, slot 0 scores
        ScoreRoundEnd(match, active, 1);
        AdvanceRoundOrEndMatch(match, active);
        assert(match.roundNumber == 2);
        assert(match.matchOver == false);
        // Round 2: slot 1 is the cause again, slot 0 scores again
        ScoreRoundEnd(match, active, 1);
        AdvanceRoundOrEndMatch(match, active);
        assert(match.roundNumber == 3);
        // Round 3: slot 1 is the cause a third time, slot 0 scores a third time
        ScoreRoundEnd(match, active, 1);
        AdvanceRoundOrEndMatch(match, active);
        assert(match.matchOver == true);
        assert(match.winnerSlot == 0);
        assert(match.roundScore[0] == 3);
        assert(match.roundScore[1] == 0);
    }

    // Tie after 3 rounds triggers a tiebreak, which resolves on the next round-end
    {
        MatchState match{};
        bool active[kMaxPlayersPerSession] = { true, true, true, false };
        // Round 1: slot 2 is the cause, slots 0 and 1 both score
        ScoreRoundEnd(match, active, 2);
        AdvanceRoundOrEndMatch(match, active);
        // Round 2: slot 2 is the cause again, slots 0 and 1 both score again
        ScoreRoundEnd(match, active, 2);
        AdvanceRoundOrEndMatch(match, active);
        // Round 3: slot 2 is the cause a third time, slots 0 and 1 both score a third time
        ScoreRoundEnd(match, active, 2);
        AdvanceRoundOrEndMatch(match, active);
        // Slots 0 and 1 are tied at 3 each; slot 2 has 0. Tiebreak should trigger.
        assert(match.matchOver == false);
        assert(match.inTiebreak == true);
        assert(match.tiebreakEligible[0] == true);
        assert(match.tiebreakEligible[1] == true);
        assert(match.tiebreakEligible[2] == false); // not tied for the max, excluded from the tiebreak

        // Tiebreak round: slot 1 is the cause, only slot 0 (the other tiebreak-eligible
        // slot) gains a tiebreak-relevant point; slot 2 (not eligible) must NOT score even
        // though it's active and not the excluded slot.
        ScoreRoundEnd(match, active, 1);
        AdvanceRoundOrEndMatch(match, active);
        assert(match.matchOver == true);
        assert(match.winnerSlot == 0);
        assert(match.roundScore[2] == 0); // confirmed untouched by the tiebreak round
    }

    // Tiebreak wedge hardening: if every tiebreak-eligible slot goes inactive while a
    // non-eligible active player keeps triggering scored round-ends, the tiebreak must
    // END (no winner) rather than looping in inTiebreak forever with no resolution path.
    {
        MatchState match{};
        match.inTiebreak = true;
        match.tiebreakEligible[0] = true;
        match.tiebreakEligible[1] = true;
        match.roundScore[0] = 3;
        match.roundScore[1] = 3;
        // Slots 0 and 1 (the only eligible ones) disconnect; only slot 2 remains active.
        bool active[kMaxPlayersPerSession] = { false, false, true, false };
        ScoreRoundEnd(match, active, -1);
        AdvanceRoundOrEndMatch(match, active);
        assert(match.matchOver == true);
        assert(match.winnerSlot == -1); // no winner determinable
    }

    // No active players: does not crash, match simply doesn't resolve
    {
        MatchState match{};
        bool active[kMaxPlayersPerSession] = { false, false, false, false };
        for (int r = 0; r < kRoundsPerMatch; r++) {
            ScoreRoundEnd(match, active, -1);
            AdvanceRoundOrEndMatch(match, active);
        }
        assert(match.matchOver == false);
    }
}

void SmokeTestMatchOverFreezeAndNewMatch() {
    const float dt = 1.0f / 60.0f;

    // --- matchOver freezes ALL potato simulation, except holder position tracking ---
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[1].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[1].player = Player(Vector2{100.0f, 0.0f});

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };

        HotPotato potato{};
        potato.held = true;
        potato.holderSlot = 0;
        potato.position = session.slots[0].player.position;
        potato.explodeTimer = 0.01f; // would explode within one tick if the guard didn't hold
        potato.catchCount = 3;
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};

        // Full-throttle inputs: if any of the frozen logic ran, charge would accumulate
        // and the potato would be thrown.
        latestInputs[0].chargingThrow = true;
        latestInputs[0].releaseThrow = true;
        latestInputs[0].aimDirX = 1.0f;

        MatchState match{};
        match.matchOver = true;
        match.winnerSlot = 1;

        for (int i = 0; i < 30; i++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
        }

        // No charging, no throw, no catch, no explosion, no round advance.
        assert(chargeTimer[0] == 0.0f);
        assert(potato.held == true);
        assert(potato.inFlight == false);
        assert(potato.holderSlot == 0);
        assert(potato.catchCount == 3);
        assert(potato.explodeTimer == 0.01f); // timer never ticked down
        assert(session.slots[0].player.state == PlayerState::Alive); // never exploded/downed
        assert(match.roundNumber == 1);
        assert(match.roundScore[0] == 0 && match.roundScore[1] == 0);
        assert(match.winnerSlot == 1);

        // ...but the position sync-to-holder block lives OUTSIDE the matchOver guard, so
        // moving the holder must still drag the potato along.
        session.slots[0].player.position = Vector2{42.0f, 17.0f};
        {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
        }
        assert(potato.position.x == 42.0f && potato.position.y == 17.0f);
        assert(potato.held == true && potato.holderSlot == 0); // still frozen otherwise

        // --- New Match reset actually un-wedges the session ---
        StartNewMatch(session, match, potato, chargeTimer);

        assert(match.matchOver == false);
        assert(match.roundNumber == 1);
        assert(match.winnerSlot == -1);
        assert(match.inTiebreak == false);
        assert(match.roundScore[0] == 0 && match.roundScore[1] == 0);
        // Potato freshly held by the first Alive active player, timers reset.
        assert(potato.held == true);
        assert(potato.holderSlot == 0);
        assert(session.slots[0].player.state == PlayerState::Alive);
        assert(potato.inFlight == false);
        assert(potato.catchCount == 0);
        assert(potato.explodeTimer == ComputeExplodeTimerForCatch(0));
        assert(chargeTimer[0] == 0.0f);

        // And simulation is genuinely live again: the explode timer ticks down.
        latestInputs[0].chargingThrow = false;
        latestInputs[0].releaseThrow = false;
        float before = potato.explodeTimer;
        {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match);
        }
        assert(potato.explodeTimer < before);
    }

    // --- New Match skips non-Alive players when handing out the potato ---
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[1].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[1].player = Player(Vector2{100.0f, 0.0f});
        session.slots[0].player.ForceDown();

        HotPotato potato{};
        float chargeTimer[kMaxPlayersPerSession] = { 1.0f, 2.0f, 3.0f, 4.0f };
        MatchState match{};
        match.matchOver = true;
        match.roundNumber = 4;
        match.roundScore[1] = 2;

        StartNewMatch(session, match, potato, chargeTimer);

        assert(match.matchOver == false);
        assert(match.roundNumber == 1);
        assert(match.roundScore[1] == 0);
        assert(potato.held == true);
        assert(potato.holderSlot == 1); // slot 0 is Downed, so slot 1 gets it
        for (int i = 0; i < kMaxPlayersPerSession; i++) assert(chargeTimer[i] == 0.0f);
    }
}

void SmokeTestDash() {
    // ResolveDashDirection: uses current movement input when non-zero
    {
        Vector2 dir = ResolveDashDirection(Vector2{1.0f, 0.0f}, Vector2{0.0f, 1.0f});
        assert(std::fabs(dir.x - 1.0f) < 0.001f);
        assert(std::fabs(dir.y - 0.0f) < 0.001f);
    }

    // ResolveDashDirection: falls back to facing direction when no movement input
    {
        Vector2 dir = ResolveDashDirection(Vector2{0.0f, 0.0f}, Vector2{0.0f, 1.0f});
        assert(std::fabs(dir.x - 0.0f) < 0.001f);
        assert(std::fabs(dir.y - 1.0f) < 0.001f);
    }

    // ResolveDashDirection: normalizes a non-unit movement input
    {
        Vector2 dir = ResolveDashDirection(Vector2{3.0f, 4.0f}, Vector2{1.0f, 0.0f});
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        assert(std::fabs(len - 1.0f) < 0.001f);
        assert(std::fabs(dir.x - 0.6f) < 0.001f);
        assert(std::fabs(dir.y - 0.8f) < 0.001f);
    }

    // ComputeDashDestination: moves kDashDistance in the given direction, unclamped case
    {
        Rectangle bounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        Vector2 dest = ComputeDashDestination(Vector2{500.0f, 300.0f}, Vector2{1.0f, 0.0f}, bounds);
        assert(std::fabs(dest.x - (500.0f + kDashDistance)) < 0.001f);
        assert(std::fabs(dest.y - 300.0f) < 0.001f);
    }

    // ComputeDashDestination: clamps to stay inside courtBounds
    {
        Rectangle bounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        Vector2 dest = ComputeDashDestination(Vector2{950.0f, 300.0f}, Vector2{1.0f, 0.0f}, bounds);
        assert(dest.x <= bounds.x + bounds.width);
        assert(std::fabs(dest.x - (bounds.x + bounds.width)) < 0.001f); // clamped exactly to the edge
    }

    // Full integration via the Input-message-handling path: dash moves the player,
    // respects cooldown (a second immediate dash attempt is a no-op), and the
    // stationary-dash-uses-facing-direction fallback works.
    // (Since the dash-application code lives in main()'s packet-receive loop rather than
    // a standalone testable function, this smoke test constructs the equivalent scenario
    // directly against a Player object using the same ResolveDashDirection/
    // ComputeDashDestination helpers Task 3 wires into that loop, rather than attempting
    // to simulate a full UDP packet round-trip — this still exercises the identical logic
    // path since the loop's dash-handling code is a thin, direct call into these two
    // functions plus a cooldown check, all of which are covered above and below.)
    {
        Player p(Vector2{100.0f, 100.0f});
        Rectangle bounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        assert(p.dashCooldownTimer <= 0.0f);

        // Simulate: player moving right, dash pressed
        Vector2 moveInput{1.0f, 0.0f};
        p.facingDirection = Vector2{1.0f, 0.0f}; // already facing right from prior movement
        if (p.dashCooldownTimer <= 0.0f) {
            Vector2 dashDir = ResolveDashDirection(moveInput, p.facingDirection);
            p.position = ComputeDashDestination(p.position, dashDir, bounds);
            p.dashCooldownTimer = kDashCooldown;
        }
        assert(std::fabs(p.position.x - (100.0f + kDashDistance)) < 0.001f);
        assert(p.dashCooldownTimer == kDashCooldown);

        // Immediately try to dash again: cooldown blocks it
        Vector2 posBeforeSecondAttempt = p.position;
        if (p.dashCooldownTimer <= 0.0f) {
            Vector2 dashDir = ResolveDashDirection(moveInput, p.facingDirection);
            p.position = ComputeDashDestination(p.position, dashDir, bounds);
            p.dashCooldownTimer = kDashCooldown;
        }
        assert(p.position.x == posBeforeSecondAttempt.x);
        assert(p.position.y == posBeforeSecondAttempt.y);

        // Tick the cooldown down to 0, confirm dash works again.
        // Use enough iterations to overshoot kDashCooldown (2s at 60Hz = 120 ticks
        // exactly) rather than the exact tick count: summing 120 individual
        // 1/60.0f float subtractions accumulates rounding error and can leave a
        // sub-epsilon positive residue instead of landing exactly on/below zero
        // (matches the overshoot pattern used by the UpdateTimers-based smoke
        // tests elsewhere in this file, e.g. SmokeTestPlayerStateMachine).
        for (int i = 0; i < 130; i++) { // slight overshoot past the 2s/120-tick cooldown
            if (p.dashCooldownTimer > 0.0f) {
                p.dashCooldownTimer -= 1.0f / 60.0f;
                if (p.dashCooldownTimer < 0.0f) p.dashCooldownTimer = 0.0f;
            }
        }
        assert(p.dashCooldownTimer <= 0.0f);
        Vector2 posBeforeThirdAttempt = p.position;
        if (p.dashCooldownTimer <= 0.0f) {
            Vector2 dashDir = ResolveDashDirection(Vector2{0.0f, 0.0f}, p.facingDirection); // stationary: uses facing fallback
            p.position = ComputeDashDestination(p.position, dashDir, bounds);
            p.dashCooldownTimer = kDashCooldown;
        }
        assert(std::fabs(p.position.x - (posBeforeThirdAttempt.x + kDashDistance)) < 0.001f); // still moved right, via facingDirection fallback
    }
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

    SmokeTestSimulationTick();
    std::printf("SmokeTestSimulationTick passed\n");

    SmokeTestHotPotato();
    std::printf("SmokeTestHotPotato passed\n");

    SmokeTestMatchState();
    std::printf("SmokeTestMatchState passed\n");

    SmokeTestMatchOverFreezeAndNewMatch();
    std::printf("SmokeTestMatchOverFreezeAndNewMatch passed\n");

    SmokeTestDash();
    std::printf("SmokeTestDash passed\n");

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
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
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

// Starts a fresh Hot Potato round: hands the potato to the first Alive, active player in
// ascending slot order and clears every player's accumulated charge.
static void ResetPotatoForNewRound(HotPotato& potato, Session& session, const bool* active, float* chargeTimer) {
    HotPotato reset{};
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (active[i] && session.slots[i].player.state == PlayerState::Alive) {
            reset.held = true;
            reset.holderSlot = i;
            reset.position = session.slots[i].player.position;
            reset.explodeTimer = ComputeExplodeTimerForCatch(0);
            break;
        }
    }
    // If no Alive player exists, `reset` stays held=false/holderSlot=-1 — the potato
    // idles rather than pinning to a corpse; it'll pick up a holder once someone respawns.
    potato = reset;

    // Clear every player's accumulated charge: a player who charged but never released
    // before this round ended must not carry stale charge into the next round (it would
    // produce an inflated-force "free" throw on their very first tap next time they hold).
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        chargeTimer[i] = 0.0f;
    }
}

// Starts a brand-new match for a session: clears all scores/round state (so matchOver
// goes back to false) and respawns the potato exactly like a normal round reset.
// Triggered out-of-band by the debug menu's "New Match" button (DebugAction::NewMatch);
// session-scoped, so it deliberately lives here rather than in ApplyDebugAction.
static void StartNewMatch(Session& session, MatchState& match, HotPotato& potato, float* chargeTimer) {
    match = MatchState{};
    bool active[kMaxPlayersPerSession];
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        active[i] = session.slots[i].state == SlotState::Connected;
    }
    ResetPotatoForNewRound(potato, session, active, chargeTimer);
}

// Pure gameplay-state mutation for one session's tick: pickup, hot potato, revive
// and timers. Deliberately free of socket/serialization concerns so it can be unit
// tested directly (see SmokeTestSimulationTick). `active` is filled in for the caller,
// which needs it to build the snapshot.
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds, MatchState& match) {
    // Only players occupying a genuinely Connected slot are simulated.
    // Empty and DisconnectedPending slots are frozen: no movement, pickup,
    // attack, revive, hazard damage, or timer updates.
    bool active[kMaxPlayersPerSession];
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        active[i] = session.slots[i].state == SlotState::Connected;
        if (activeOut) activeOut[i] = active[i];
    }

    // Pickup: each active, Alive player who isn't currently a valid revive
    // channel target for anyone else may pick up an item. (A player who could
    // instead be revived should channel-revive, not pick up items, mirroring
    // the original 2-player behavior's "!canRevive" gate.)
    if (interact) {
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i] || session.slots[i].player.state != PlayerState::Alive || !interact[i]) continue;
            bool isRevivable = false;
            for (int j = 0; j < kMaxPlayersPerSession; j++) {
                if (i == j || !active[j]) continue;
                if (session.slots[j].player.state == PlayerState::Downed &&
                    DistanceBetween(session.slots[i].player.position, session.slots[j].player.position) <= kReviveRange) {
                    isRevivable = true;
                    break;
                }
            }
            if (isRevivable) continue;
            for (auto& item : items) { if (TryPickup(item, session.slots[i].player.position, session.slots[i].player.inventory, kPickupRadius)) break; }
        }
    }

    // --- Hot Potato: charge tracking, throw, flight, catch, explosion ---
    bool soloMode = false;
    {
        int activeCount = 0;
        for (int i = 0; i < kMaxPlayersPerSession; i++) if (active[i]) activeCount++;
        soloMode = (activeCount == 1);
    }

    if (match.matchOver) {
        // Match decided: freeze the potato in whatever state it's in (typically unheld,
        // since the winning round-end already reset it) — no further charge/throw/catch/
        // explosion simulation runs. A new match starts via the debug menu's "New Match"
        // action (see DebugAction::NewMatch), which resets this session's MatchState and
        // respawns the potato via StartNewMatch.
    } else {
    // Charge tracking: accumulate while held, cap at kMaxChargeDuration, reset on release.
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (!active[i]) continue;
        if (potato.held && potato.holderSlot == i && latestInputs[i].chargingThrow) {
            chargeTimer[i] += dt;
            if (chargeTimer[i] > kMaxChargeDuration) chargeTimer[i] = kMaxChargeDuration;
        }
    }

    // Release: only the current holder's release matters.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot] &&
        latestInputs[potato.holderSlot].releaseThrow) {
        int holder = potato.holderSlot;
        float force = ComputeThrowForce(chargeTimer[holder]);
        Vector2 aim{ latestInputs[holder].aimDirX, latestInputs[holder].aimDirY };
        float aimLen = std::sqrt(aim.x * aim.x + aim.y * aim.y);
        if (aimLen < 0.0001f) { aim = Vector2{1.0f, 0.0f}; aimLen = 1.0f; } // degenerate aim: default to +X
        aim.x /= aimLen;
        aim.y /= aimLen;

        potato.velocity = Vector2{ aim.x * force, aim.y * force };
        potato.held = false;
        potato.inFlight = true;
        potato.lastThrowerSlot = holder;
        potato.holderSlot = -1;
        potato.justThrown = true; // grace flag: exclude the thrower from catch checks while the potato is still within kCatchRadius of them post-release (see the flight block below)
        chargeTimer[holder] = 0.0f;
    }

    // Flight: integrate position, apply drag, check catch, check bounds.
    if (potato.inFlight) {
        potato.position.x += potato.velocity.x * dt;
        potato.position.y += potato.velocity.y * dt;
        ApplyPotatoDrag(potato, dt);

        Vector2 positions[kMaxPlayersPerSession];
        for (int i = 0; i < kMaxPlayersPerSession; i++) positions[i] = session.slots[i].player.position;

        // Same-tick/near-tick self-catch fallback: measured that even a full-force throw
        // takes ~3 ticks at 60Hz to clear kCatchRadius(20) of a stationary thrower (8.3,
        // 16.6, 24.7 units at ticks 0/1/2 post-release), so a single-tick grace flag is
        // not enough — the thrower would still instantly re-catch their own throw one or
        // two ticks later. Instead, exclude the thrower from the catch check for as long
        // as the potato remains within kCatchRadius of the thrower's OWN position (i.e.
        // hasn't actually left their immediate vicinity yet); once it clears that radius
        // even once, clear the flag permanently so the thrower CAN catch their own throw
        // later (e.g. after a solo-mode wall bounce).
        int excludeSlot = -1;
        if (potato.justThrown && potato.lastThrowerSlot != -1 && active[potato.lastThrowerSlot]) {
            float distFromThrower = DistanceBetween2(potato.position, positions[potato.lastThrowerSlot]);
            if (distFromThrower <= kCatchRadius) {
                excludeSlot = potato.lastThrowerSlot;
            } else {
                potato.justThrown = false;
            }
        } else {
            potato.justThrown = false;
        }
        // Only active, Alive players can catch: a Downed or Dead player standing in the
        // flight path must not become the new holder (they can't throw it, which would
        // strand the potato). Filter here rather than changing FindCatchTarget's signature.
        bool catchEligible[kMaxPlayersPerSession];
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            catchEligible[i] = active[i] && session.slots[i].player.state == PlayerState::Alive;
        }
        int catcher = FindCatchTarget(potato.position, positions, catchEligible, kMaxPlayersPerSession, excludeSlot);
        if (catcher != -1) {
            potato.held = true;
            potato.inFlight = false;
            potato.holderSlot = catcher;
            potato.velocity = Vector2{0.0f, 0.0f};
            potato.position = positions[catcher];
            potato.catchCount += 1;
            potato.explodeTimer = ComputeExplodeTimerForCatch(potato.catchCount);
        } else {
            bool outOfBounds = potato.position.x < courtBounds.x || potato.position.x > courtBounds.x + courtBounds.width ||
                                potato.position.y < courtBounds.y || potato.position.y > courtBounds.y + courtBounds.height;
            if (outOfBounds) {
                if (soloMode) {
                    // Reflect off whichever boundary was crossed, clamp back inside.
                    if (potato.position.x < courtBounds.x) { potato.position.x = courtBounds.x; potato.velocity.x = -potato.velocity.x; }
                    if (potato.position.x > courtBounds.x + courtBounds.width) { potato.position.x = courtBounds.x + courtBounds.width; potato.velocity.x = -potato.velocity.x; }
                    if (potato.position.y < courtBounds.y) { potato.position.y = courtBounds.y; potato.velocity.y = -potato.velocity.y; }
                    if (potato.position.y > courtBounds.y + courtBounds.height) { potato.position.y = courtBounds.y + courtBounds.height; potato.velocity.y = -potato.velocity.y; }
                } else if (potato.lastThrowerSlot != -1 && active[potato.lastThrowerSlot]) {
                    // Multiplayer: leaving the court downs the last thrower, scores the
                    // round, and starts a new round (or ends the match).
                    session.slots[potato.lastThrowerSlot].player.ForceDown();
                    ScoreRoundEnd(match, active, potato.lastThrowerSlot);
                    AdvanceRoundOrEndMatch(match, active);
                    // Respawn held by the first Alive active player (deterministic slot
                    // order), via the shared ResetPotatoForNewRound helper.
                    ResetPotatoForNewRound(potato, session, active, chargeTimer);
                }
            }
        }
    }

    // Holder vanished (disconnected, downed, or dead): reclaim the potato immediately.
    // Must run BEFORE the explosion countdown, which also gates on active[holderSlot] —
    // otherwise a held potato whose holder went away would freeze forever, its timer
    // never ticking down and never expiring.
    if (potato.held && (potato.holderSlot < 0 || !active[potato.holderSlot] ||
                        session.slots[potato.holderSlot].player.state != PlayerState::Alive)) {
        ResetPotatoForNewRound(potato, session, active, chargeTimer);
    }

    // Explosion: timer expires while held.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot]) {
        potato.explodeTimer -= dt;
        if (potato.explodeTimer <= 0.0f) {
            int exploderSlot = potato.holderSlot;
            session.slots[exploderSlot].player.ForceDown();
            ScoreRoundEnd(match, active, exploderSlot);
            AdvanceRoundOrEndMatch(match, active);
            ResetPotatoForNewRound(potato, session, active, chargeTimer);
        }
    }
    }

    // Held potato tracks its holder's position each tick.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot]) {
        potato.position = session.slots[potato.holderSlot].player.position;
    }

    // Revive: each active reviver channels against AT MOST ONE target per tick — the
    // lowest-slot-indexed Downed candidate that UpdateRevive actually makes progress
    // against. channelTimer lives on the reviver, not on the (reviver, target) pair, so
    // calling UpdateRevive again for the same reviver against a different, non-valid
    // target would reset the timer to 0 and undo that progress within the same tick
    // (harmless with exactly 2 active players, fatal with 3+). Breaking on progress
    // prevents that; the explicit reset below preserves the original "no valid target
    // resets the timer" behavior, which is UpdateRevive's only observable effect when
    // canProgress is false.
    if (interact) {
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i]) continue;
            bool channeling = false;
            for (int j = 0; j < kMaxPlayersPerSession; j++) {
                if (i == j || !active[j]) continue;
                if (session.slots[j].player.state != PlayerState::Downed) continue;
                UpdateRevive(session.slots[i].player, session.slots[j].player, interact[i], dt, kReviveRange);
                if (session.slots[i].player.channelTimer > 0.0f) { channeling = true; break; }
            }
            if (!channeling) session.slots[i].player.channelTimer = 0.0f;
        }
    }

    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (active[i]) session.slots[i].player.UpdateTimers(dt);
    }

    // Dash cooldown decays unconditionally for every active player, independent of
    // match/potato state — it's a player-movement mechanic, not something that should
    // freeze when a match concludes.
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (active[i] && session.slots[i].player.dashCooldownTimer > 0.0f) {
            session.slots[i].player.dashCooldownTimer -= dt;
            if (session.slots[i].player.dashCooldownTimer < 0.0f) session.slots[i].player.dashCooldownTimer = 0.0f;
        }
    }
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
    HazardZone hazard{ Rectangle{450.0f, 200.0f, 100.0f, 200.0f} }; // retained per Global Constraints; not applied to Hot Potato sessions this phase
    Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };

    // Track world items and unreliable-send sequence counters per session name,
    // since Session itself only holds player slots.
    std::map<std::string, std::vector<WorldItem>> sessionWorldItems;
    std::map<std::string, float[kMaxPlayersPerSession]> sessionHazardCarry;
    std::map<std::string, uint32_t> sessionSnapshotSeq;
    std::map<std::string, bool[kMaxPlayersPerSession]> pendingAttack;
    std::map<std::string, bool[kMaxPlayersPerSession]> pendingInteract;
    std::map<std::string, HotPotato> sessionPotato;
    std::map<std::string, float[kMaxPlayersPerSession]> sessionChargeTimer;
    std::map<std::string, MatchState> sessionMatch;
    std::map<std::string, InputMsg[kMaxPlayersPerSession]> sessionLatestInput;

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
                                for (int i = 0; i < kMaxPlayersPerSession; i++) {
                                    sessionHazardCarry[outcome.roomCode][i] = 0.0f;
                                    sessionChargeTimer[outcome.roomCode][i] = 0.0f;
                                    sessionLatestInput[outcome.roomCode][i] = InputMsg{};
                                }
                                sessionSnapshotSeq[outcome.roomCode] = 1;
                                HotPotato freshPotato{};
                                freshPotato.held = true;
                                freshPotato.holderSlot = 0; // slot 0 (the room creator) always starts holding; simplest deterministic choice for this phase
                                freshPotato.position = session->slots[0].player.position;
                                freshPotato.explodeTimer = ComputeExplodeTimerForCatch(0);
                                sessionPotato[outcome.roomCode] = freshPotato;
                                sessionMatch[outcome.roomCode] = MatchState{};
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

                                    // Track facing direction whenever real movement input is
                                    // present, regardless of whether a dash happens this packet —
                                    // this is what a stationary dash falls back to.
                                    float moveLen = std::sqrt(input.moveX * input.moveX + input.moveY * input.moveY);
                                    if (moveLen > 0.0001f) {
                                        slot.player.facingDirection = Vector2{ input.moveX / moveLen, input.moveY / moveLen };
                                    }

                                    if (input.dashPressed && slot.player.dashCooldownTimer <= 0.0f) {
                                        Vector2 dashDir = ResolveDashDirection(Vector2{ input.moveX, input.moveY }, slot.player.facingDirection);
                                        slot.player.position = ComputeDashDestination(slot.player.position, dashDir, courtBounds);
                                        slot.player.dashCooldownTimer = kDashCooldown;
                                    }
                                }
                                pendingAttack[loc.sessionName][loc.slotIndex] = input.attackPressed;
                                pendingInteract[loc.sessionName][loc.slotIndex] = input.interactHeld;
                                sessionLatestInput[loc.sessionName][loc.slotIndex] = input;
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
                                        if (deliveredReq.action == DebugAction::NewMatch) {
                                            // Session-scoped action: resets this room's MatchState and
                                            // respawns the potato. targetSlot is ignored by design.
                                            StartNewMatch(*session, sessionMatch[loc.sessionName],
                                                          sessionPotato[loc.sessionName],
                                                          sessionChargeTimer[loc.sessionName]);
                                        } else if (deliveredReq.targetSlot < kMaxPlayersPerSession) {
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

                bool active[kMaxPlayersPerSession];
                HotPotato& potato = sessionPotato[sessionEntry];
                float* chargeTimer = sessionChargeTimer[sessionEntry];
                InputMsg* latestInputs = sessionLatestInput.count(sessionEntry) ? sessionLatestInput[sessionEntry] : nullptr;
                if (!latestInputs) continue; // session exists but no input map yet (shouldn't happen once creation-time init runs, but guards a null deref)
                MatchState& match = sessionMatch[sessionEntry];
                SimulateSessionTick(*session, items, hazard, hazardCarry, attack, interact, dt, active, potato, chargeTimer, latestInputs, courtBounds, match);

                // state value 3 = "absent" (slot not Connected): not a real PlayerState,
                // repurposed on the wire so an inactive slot renders as not-present
                // instead of a fully-visible phantom player, without changing the
                // PlayerSnapshot layout.
                static constexpr uint8_t kSnapshotStateAbsent = 3;

                SnapshotMsg snap{};
                for (int i = 0; i < kMaxPlayersPerSession; i++) {
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
                snap.potato = PotatoSnapshot{ potato.position.x, potato.position.y, potato.held, potato.inFlight, potato.holderSlot, potato.explodeTimer };

                MatchSnapshot matchSnap{};
                matchSnap.roundNumber = match.roundNumber;
                for (int i = 0; i < kMaxPlayersPerSession; i++) matchSnap.roundScore[i] = match.roundScore[i];
                matchSnap.matchOver = match.matchOver;
                matchSnap.winnerSlot = match.winnerSlot;
                matchSnap.inTiebreak = match.inTiebreak;
                snap.match = matchSnap;

                std::vector<uint8_t> snapBytes;
                SerializeStruct(snap, snapBytes);
                uint32_t seq = sessionSnapshotSeq[sessionEntry]++;
                for (int i = 0; i < kMaxPlayersPerSession; i++) {
                    if (session->slots[i].state == SlotState::Connected) {
                        SendUnreliable(socket, session->slots[i].clientIp, session->slots[i].clientPort, seq, MessageType::Snapshot, snapBytes.data(), snapBytes.size());
                    }
                }

                // Retransmit any unacked reliable messages for this session's slots
                for (int i = 0; i < kMaxPlayersPerSession; i++) {
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
