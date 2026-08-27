#pragma once

// All server smoke tests, extracted from main.cpp to keep that file's size manageable.
// This header is included DIRECTLY into main.cpp (not compiled as its own translation
// unit) because these tests call main.cpp's file-local (static) SimulateSessionTick and
// StartNewMatch — moving them into a separately-compiled .cpp would make those symbols
// unreachable. main.cpp forward-declares both statics before this #include, so they're
// already in scope here.

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
    snap.players[0] = PlayerSnapshot{ 10.0f, 20.0f, 100, 0, {}, 0.5f };
    snap.players[1] = PlayerSnapshot{ 30.0f, 40.0f, 0, 1, {}, 0.0f };
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
    bool usePressed[kMaxPlayersPerSession] = {};
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
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
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
        SimulateSessionTick(noPotion, items, hazard, carry2, attack2, interact, usePressed, dt, active,
                             potato2, chargeTimer2, latestInputs2, courtBounds, match2, GameMode::FFA);
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
    bool usePressed[kMaxPlayersPerSession] = {};
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
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
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
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
        }
        assert(potato.held == false);
        assert(chargeTimer[0] == 0.0f); // reset after release
        latestInputs[0].releaseThrow = false; // one-shot event, clear like a real client would

        // Simulate flight until it reaches slot 1 (at x=100) and gets caught.
        bool caught = false;
        for (int i = 0; i < 600 && !caught; i++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
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
    bool usePressed[kMaxPlayersPerSession] = {};
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
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);

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
    bool usePressed[kMaxPlayersPerSession] = {};
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
                SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                     potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
            }
            assert(chargeTimer[thrower] > 0.0f);

            latestInputs[thrower].chargingThrow = false;
            latestInputs[thrower].releaseThrow = true;
            latestInputs[thrower].aimDirX = aimX;
            latestInputs[thrower].aimDirY = 0.0f;
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
            latestInputs[thrower].releaseThrow = false;

            bool caught = false;
            for (int i = 0; i < 600 && !caught; i++) {
                SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                     potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
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
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);

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
    bool usePressed[kMaxPlayersPerSession] = {};
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
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);

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
    bool usePressed[kMaxPlayersPerSession] = {};
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
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
        }
        latestInputs[0].chargingThrow = false;
        latestInputs[0].releaseThrow = true;
        latestInputs[0].aimDirX = 1.0f;
        latestInputs[0].aimDirY = 0.0f;
        {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
        }
        latestInputs[0].releaseThrow = false;
        assert(potato.inFlight == true);
        assert(potato.velocity.x > 0.0f);

        bool bounced = false;
        for (int i = 0; i < 60 && !bounced; i++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
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

void SmokeTestTeamScoring() {
    // TeamForSlot mapping
    assert(TeamForSlot(0) == 0);
    assert(TeamForSlot(1) == 0);
    assert(TeamForSlot(2) == 1);
    assert(TeamForSlot(3) == 1);

    // ScoreRoundEndTeam credits the OPPOSITE team, not the excluded player's own team
    {
        MatchState match{};
        bool active[kMaxPlayersPerSession] = { true, true, true, true };
        ScoreRoundEndTeam(match, active, 0); // slot 0 (Team A) is the cause -> Team B scores
        assert(match.teamScore[0] == 0);
        assert(match.teamScore[1] == 1);
    }
    {
        MatchState match{};
        bool active[kMaxPlayersPerSession] = { true, true, true, true };
        ScoreRoundEndTeam(match, active, 2); // slot 2 (Team B) is the cause -> Team A scores
        assert(match.teamScore[0] == 1);
        assert(match.teamScore[1] == 0);
    }

    // Full 3-round match: Team A wins outright
    {
        MatchState match{};
        bool active[kMaxPlayersPerSession] = { true, true, true, true };
        for (int r = 0; r < kRoundsPerMatch; r++) {
            ScoreRoundEndTeam(match, active, 2); // Team B always the cause -> Team A scores every round
            AdvanceRoundOrEndMatchTeam(match);
        }
        assert(match.matchOver == true);
        assert(match.winnerSlot == 0); // Team A (index 0) won
        assert(match.teamScore[0] == 3);
        assert(match.teamScore[1] == 0);
    }

    // Tiebreak entry and resolution, tested via DIRECT state construction (see IMPORTANT NOTE below
    // for why a "3 rounds of alternating credit" approach cannot produce a tie and must not be used)
    {
        MatchState match{};
        match.teamScore[0] = 1;
        match.teamScore[1] = 1;
        match.roundNumber = kRoundsPerMatch; // about to advance past the last normal round, tied
        AdvanceRoundOrEndMatchTeam(match);
        assert(match.matchOver == false);
        assert(match.inTiebreak == true);

        // Next round-end during the tiebreak resolves it immediately in favor of whichever team scores
        bool active[kMaxPlayersPerSession] = { true, true, true, true };
        ScoreRoundEndTeam(match, active, 2); // Team B is the cause -> Team A scores -> Team A now leads 2-1
        AdvanceRoundOrEndMatchTeam(match);
        assert(match.matchOver == true);
        assert(match.winnerSlot == 0); // Team A won the tiebreak
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
    bool usePressed[kMaxPlayersPerSession] = {};
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
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
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
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
        }
        assert(potato.position.x == 42.0f && potato.position.y == 17.0f);
        assert(potato.held == true && potato.holderSlot == 0); // still frozen otherwise

        // --- New Match reset actually un-wedges the session ---
        StartNewMatch(session, match, potato, chargeTimer, GameMode::FFA);

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
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
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

        StartNewMatch(session, match, potato, chargeTimer, GameMode::FFA);

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

    // ClampToCourtBounds: shared by dash and ordinary movement.
    {
        Rectangle bounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        Vector2 clamped = ClampToCourtBounds(Vector2{-50.0f, 700.0f}, bounds);
        assert(std::fabs(clamped.x - 0.0f) < 0.001f);
        assert(std::fabs(clamped.y - 600.0f) < 0.001f);
        Vector2 inside = ClampToCourtBounds(Vector2{500.0f, 300.0f}, bounds);
        assert(std::fabs(inside.x - 500.0f) < 0.001f && std::fabs(inside.y - 300.0f) < 0.001f);
    }

    // TryApplyDash (the REAL production function main()'s Input handler calls): happy path,
    // cooldown gating, facing-direction tracking, and the stationary facing fallback.
    //
    // TryApplyDash no longer teleports position instantly — it starts a dash-in-progress
    // (Player::StartDash) that Player::AdvanceDash slides toward over kDashDuration, ticked
    // from the tick loop. So "dash applied" is now verified via IsDashing()/dashTargetPos
    // immediately after the call, and the actual arrival is verified by ticking AdvanceDash
    // to completion.
    {
        Player p(Vector2{100.0f, 100.0f});
        Rectangle bounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        assert(p.dashCooldownTimer <= 0.0f);

        // Moving right, dash pressed: dash starts (position not yet moved) and
        // facingDirection updates immediately.
        assert(TryApplyDash(p, Vector2{1.0f, 0.0f}, true, bounds) == true);
        assert(p.IsDashing() == true);
        assert(std::fabs(p.position.x - 100.0f) < 0.001f); // not yet moved
        assert(std::fabs(p.dashTargetPos.x - (100.0f + kDashDistance)) < 0.001f);
        assert(std::fabs(p.facingDirection.x - 1.0f) < 0.001f);
        assert(p.dashCooldownTimer == kDashCooldown);

        // Ticking AdvanceDash to completion actually arrives at the destination.
        for (int i = 0; i < 20; i++) p.AdvanceDash(1.0f / 60.0f); // overshoots kDashDuration(0.2s)
        assert(p.IsDashing() == false);
        assert(std::fabs(p.position.x - (100.0f + kDashDistance)) < 0.001f);

        // Immediately dashing again is blocked by the cooldown (no new dash starts).
        Vector2 posBeforeSecondAttempt = p.position;
        assert(TryApplyDash(p, Vector2{1.0f, 0.0f}, true, bounds) == false);
        assert(p.IsDashing() == false);
        assert(p.position.x == posBeforeSecondAttempt.x);
        assert(p.position.y == posBeforeSecondAttempt.y);

        // ...but facingDirection still tracks movement input while on cooldown.
        assert(TryApplyDash(p, Vector2{0.0f, 1.0f}, false, bounds) == false);
        assert(std::fabs(p.facingDirection.y - 1.0f) < 0.001f);
        p.facingDirection = Vector2{1.0f, 0.0f}; // restore for the fallback check below

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
        // Stationary dash: no movement input, so it falls back to facingDirection (+X).
        assert(TryApplyDash(p, Vector2{0.0f, 0.0f}, true, bounds) == true);
        assert(std::fabs(p.dashTargetPos.x - (posBeforeThirdAttempt.x + kDashDistance)) < 0.001f);
        for (int i = 0; i < 20; i++) p.AdvanceDash(1.0f / 60.0f);
        assert(std::fabs(p.position.x - (posBeforeThirdAttempt.x + kDashDistance)) < 0.001f);

        // dashPressed false on a ready cooldown is a no-op that does not set the cooldown.
        Player q(Vector2{100.0f, 100.0f});
        assert(TryApplyDash(q, Vector2{1.0f, 0.0f}, false, bounds) == false);
        assert(q.dashCooldownTimer == 0.0f);
        assert(q.IsDashing() == false);
        assert(q.position.x == 100.0f);
    }

    // TryApplyDash: a Downed or Dead player cannot dash, and the attempt must not touch
    // their position, dashCooldownTimer, or facingDirection.
    {
        Rectangle bounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        for (PlayerState blocked : { PlayerState::Downed, PlayerState::Dead }) {
            Player p(Vector2{100.0f, 100.0f});
            p.state = blocked;
            p.facingDirection = Vector2{1.0f, 0.0f};
            assert(TryApplyDash(p, Vector2{0.0f, 1.0f}, true, bounds) == false);
            assert(p.position.x == 100.0f && p.position.y == 100.0f);
            assert(p.dashCooldownTimer == 0.0f); // must NOT burn the cooldown on a rejected dash
            assert(p.IsDashing() == false); // and must not start a dash-in-progress either
            assert(std::fabs(p.facingDirection.x - 1.0f) < 0.001f); // facing untouched too
        }
    }

    // AdvanceDash: mid-flight, position is a linear interpolation between start and target,
    // not yet at either endpoint.
    {
        Player p(Vector2{0.0f, 0.0f});
        p.StartDash(Vector2{100.0f, 0.0f});
        assert(p.IsDashing() == true);
        p.AdvanceDash(Player::kDashDuration / 2.0f); // halfway through the dash's duration
        assert(p.IsDashing() == true); // not done yet
        assert(std::fabs(p.position.x - 50.0f) < 0.5f); // roughly halfway there
        assert(p.position.x > 0.5f && p.position.x < 99.5f); // definitely not still at either end
    }

    // DistancePointToSegment: measures to the SEGMENT, not the infinite line through it.
    {
        // Perpendicular foot lands inside the segment.
        assert(std::fabs(DistancePointToSegment(Vector2{50.0f, 30.0f}, Vector2{0.0f, 0.0f}, Vector2{100.0f, 0.0f}) - 30.0f) < 0.001f);
        // Beyond the far endpoint: clamps to that endpoint (an infinite line would say 10).
        assert(std::fabs(DistancePointToSegment(Vector2{150.0f, 10.0f}, Vector2{0.0f, 0.0f}, Vector2{100.0f, 0.0f}) - std::sqrt(50.0f * 50.0f + 100.0f)) < 0.001f);
        // Before the near endpoint: clamps to the start.
        assert(std::fabs(DistancePointToSegment(Vector2{-30.0f, 0.0f}, Vector2{0.0f, 0.0f}, Vector2{100.0f, 0.0f}) - 30.0f) < 0.001f);
        // Degenerate segment (start == end): plain point-to-point distance.
        assert(std::fabs(DistancePointToSegment(Vector2{3.0f, 4.0f}, Vector2{0.0f, 0.0f}, Vector2{0.0f, 0.0f}) - 5.0f) < 0.001f);
    }

    // TUNNELING FIX: a dash whose endpoints are BOTH well outside kCatchRadius of the
    // potato, but whose swept segment passes within it, must catch the potato. This is the
    // exact case a plain point-sample (at either endpoint alone) misses. Now that the dash is
    // spread over several ticks (Player::AdvanceDash), the production code sweeps each tick's
    // sub-segment rather than the whole dash in one shot — this test still validates the core
    // geometric claim (DashSegmentCatchesPotato correctly catches a mid-segment pass-through)
    // using the full start->end span, which is what any single sub-segment sweep reduces to
    // when the potato sits within ONE tick's slice of travel; the per-tick integration itself
    // is exercised by SmokeTestDashTunnelingAcrossTicks below.
    {
        Player p(Vector2{400.0f, 300.0f});
        p.facingDirection = Vector2{1.0f, 0.0f};

        HotPotato potato{};
        potato.inFlight = true;
        potato.holderSlot = -1;
        potato.lastThrowerSlot = 1;
        potato.velocity = Vector2{0.0f, 200.0f};
        potato.catchCount = 2;
        // Mid-segment (dash runs +X 150 units from x=400 to x=550), 5px off the dash line:
        // both endpoints are 75+ units away, far beyond kCatchRadius(20).
        potato.position = Vector2{475.0f, 305.0f};
        assert(DistanceBetween2(potato.position, p.position) > kCatchRadius);

        Vector2 preDashPos = p.position;
        p.StartDash(Vector2{ preDashPos.x + kDashDistance, preDashPos.y });
        for (int i = 0; i < 20; i++) p.AdvanceDash(1.0f / 60.0f); // drive to completion
        assert(p.IsDashing() == false);
        assert(DistanceBetween2(potato.position, p.position) > kCatchRadius); // post-dash point-sample also misses
        assert(DashSegmentCatchesPotato(preDashPos, p.position, potato.position) == true);

        int expectedCatchCount = potato.catchCount + 1;
        ResolveCatch(potato, 0, p.position);
        assert(potato.held == true);
        assert(potato.inFlight == false);
        assert(potato.holderSlot == 0);
        assert(potato.velocity.x == 0.0f && potato.velocity.y == 0.0f);
        assert(potato.position.x == p.position.x && potato.position.y == p.position.y);
        assert(potato.catchCount == expectedCatchCount);
        assert(std::fabs(potato.explodeTimer - ComputeExplodeTimerForCatch(expectedCatchCount)) < 0.001f);

        // The tick loop's flight block is gated on inFlight, which ResolveCatch cleared —
        // so a SimulateSessionTick later in the same tick can't double-process the flight.
        assert(potato.inFlight == false);
    }

    // TUNNELING FIX, negative case: a dash segment that stays farther than kCatchRadius
    // from the potato at every point must NOT catch it.
    {
        Player p(Vector2{400.0f, 300.0f});
        p.facingDirection = Vector2{1.0f, 0.0f};
        Vector2 preDashPos = p.position;
        p.StartDash(Vector2{ preDashPos.x + kDashDistance, preDashPos.y });
        for (int i = 0; i < 20; i++) p.AdvanceDash(1.0f / 60.0f);
        assert(p.IsDashing() == false);

        // 40px off the dash line: closest approach is 40 > kCatchRadius(20).
        Vector2 potatoPos{ 475.0f, 340.0f };
        assert(std::fabs(DistancePointToSegment(potatoPos, preDashPos, p.position) - 40.0f) < 0.001f);
        assert(DashSegmentCatchesPotato(preDashPos, p.position, potatoPos) == false);

        // Also negative: potato well past the END of the dash, on the dash line. An
        // infinite-line test would (wrongly) call this a catch; the segment test must not.
        Vector2 beyondEnd{ p.position.x + 100.0f, p.position.y };
        assert(DashSegmentCatchesPotato(preDashPos, p.position, beyondEnd) == false);
    }
}

// Integration test: drives a dash through SimulateSessionTick tick-by-tick (rather than
// calling TryApplyDash/AdvanceDash directly) to confirm the per-tick sub-segment sweep added
// alongside the dash-duration change actually catches a potato positioned mid-dash, exactly
// as the direct-call tunneling test above proves the underlying geometry does.
void SmokeTestDashTunnelingAcrossTicks() {
    Session session;
    session.slots[0].state = SlotState::Connected;
    session.slots[0].player = Player(Vector2{400.0f, 300.0f});
    session.slots[0].player.facingDirection = Vector2{1.0f, 0.0f};
    session.slots[1].state = SlotState::Connected;
    session.slots[1].player = Player(Vector2{900.0f, 500.0f});

    HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
    std::vector<WorldItem> items;
    Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
    float hazardCarry[kMaxPlayersPerSession] = {};
    bool attack[kMaxPlayersPerSession] = {};
    bool interact[kMaxPlayersPerSession] = {};
    bool usePressed[kMaxPlayersPerSession] = {};
    float chargeTimer[kMaxPlayersPerSession] = {};
    InputMsg latestInputs[kMaxPlayersPerSession] = {};
    MatchState match{};
    const float dt = 1.0f / 60.0f;

    HotPotato potato{};
    potato.inFlight = true;
    potato.holderSlot = -1;
    potato.lastThrowerSlot = 1;
    potato.velocity = Vector2{0.0f, 0.0f};
    potato.catchCount = 0;
    // Sits 5px off the straight-line dash path (400,300)->(550,300), well within the swept
    // segment's kCatchRadius(20) at closest approach, but never coincides with either the
    // pre-dash position or any single tick's endpoint closely enough for a point-sample to
    // have caught it by luck.
    potato.position = Vector2{475.0f, 305.0f};

    session.slots[0].player.StartDash(Vector2{550.0f, 300.0f});
    session.slots[0].player.dashCooldownTimer = kDashCooldown;

    bool caught = false;
    for (int tick = 0; tick < 20 && !caught; tick++) { // well past kDashDuration(0.2s) at 60Hz
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
        if (potato.held) caught = true;
    }

    assert(caught == true);
    assert(potato.holderSlot == 0);
    // The catch can land mid-dash (the potato sits close enough to the dash's start that a
    // catch may resolve before the dash's own kDashDuration has fully elapsed) — that's fine;
    // the dash keeps sliding independently afterward and simply finishes on its own schedule.
}

void SmokeTestGameModes() {
    // RevivePotionTest: hazard damage applies, potato logic does not run
    {
        Session session;
        for (int i = 0; i < 2; i++) {
            session.slots[i].state = SlotState::Connected;
            session.slots[i].player = Player(kSpawnPoints[i]);
        }
        session.slots[0].player.position = Vector2{ 50.0f, 50.0f }; // inside a test hazard zone
        HazardZone hazard{ Rectangle{ 0.0f, 0.0f, 100.0f, 100.0f } };
        std::vector<WorldItem> items;
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
    bool usePressed[kMaxPlayersPerSession] = {};
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{}; // never used in this mode, but the signature still requires one

        int startingHp = session.slots[0].player.hp;
        for (int tick = 0; tick < 60; tick++) { // 1 second at 60Hz
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, 1.0f / 60.0f, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::RevivePotionTest);
        }
        assert(session.slots[0].player.hp < startingHp); // hazard damage applied
        assert(potato.held == false && potato.inFlight == false); // potato never activated

        // Confirm charging/releasing input is simply ignored in this mode (no potato exists)
        latestInputs[0].chargingThrow = true;
        latestInputs[0].releaseThrow = true;
        bool active2[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, 1.0f / 60.0f, active2,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::RevivePotionTest);
        assert(potato.held == false && potato.inFlight == false); // still untouched
    }

    // FFA: hazard damage does NOT apply (matches existing FFA behavior, must remain unchanged)
    {
        Session session;
        for (int i = 0; i < 2; i++) {
            session.slots[i].state = SlotState::Connected;
            session.slots[i].player = Player(kSpawnPoints[i]);
        }
        session.slots[0].player.position = Vector2{ 50.0f, 50.0f };
        HazardZone hazard{ Rectangle{ 0.0f, 0.0f, 100.0f, 100.0f } };
        std::vector<WorldItem> items;
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
    bool usePressed[kMaxPlayersPerSession] = {};
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{};
        potato.held = true;
        potato.holderSlot = 0;
        potato.position = session.slots[0].player.position;
        // Must be set explicitly: HotPotato{}'s default explodeTimer is 0.0f, which would
        // otherwise detonate the potato (and ForceDown the holder) on the very first tick,
        // confounding this test's hp comparison with an unrelated Hot-Potato mechanic.
        potato.explodeTimer = ComputeExplodeTimerForCatch(0);

        int startingHp = session.slots[0].player.hp;
        for (int tick = 0; tick < 60; tick++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, 1.0f / 60.0f, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
        }
        assert(session.slots[0].player.hp == startingHp); // hazard does NOT apply in FFA
    }
}

// Finding 1 regression: a 2v2 room with fewer than 4 connected players must never score
// or end a match. Slot assignment hands out the lowest empty slot, so 2 players naturally
// land on slots 0 and 1 -- both Team A -- and every round-end would otherwise credit the
// EMPTY Team B, producing a guaranteed bogus 0-3 "Team B Wins".
void SmokeTestTwoVTwoPopulationGate() {
    // Pure predicate: only 2v2 is gated, and only below 4 players.
    {
        bool active2[kMaxPlayersPerSession] = { true, true, false, false };
        bool active3[kMaxPlayersPerSession] = { true, true, true, false };
        bool active4[kMaxPlayersPerSession] = { true, true, true, true };
        assert(HotPotatoGameplayEnabled(GameMode::TwoVTwo, active2) == false);
        assert(HotPotatoGameplayEnabled(GameMode::TwoVTwo, active3) == false);
        assert(HotPotatoGameplayEnabled(GameMode::TwoVTwo, active4) == true);
        // FFA is untouched by the gate at every population.
        assert(HotPotatoGameplayEnabled(GameMode::FFA, active2) == true);
        assert(HotPotatoGameplayEnabled(GameMode::FFA, active4) == true);
        // RevivePotionTest never runs potato logic, at any population.
        assert(HotPotatoGameplayEnabled(GameMode::RevivePotionTest, active4) == false);
        // Both 2-player slots really are on the same team (the root cause).
        assert(TeamForSlot(0) == TeamForSlot(1));
    }

    Session session;
    for (int i = 0; i < 2; i++) {
        session.slots[i].state = SlotState::Connected;
        session.slots[i].player = Player(kSpawnPoints[i]);
    }
    HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
    std::vector<WorldItem> items;
    Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
    float hazardCarry[kMaxPlayersPerSession] = {};
    bool attack[kMaxPlayersPerSession] = {};
    bool interact[kMaxPlayersPerSession] = {};
    bool usePressed[kMaxPlayersPerSession] = {};
    float chargeTimer[kMaxPlayersPerSession] = {};
    InputMsg latestInputs[kMaxPlayersPerSession] = {};
    MatchState match{};
    const float dt = 1.0f / 60.0f;

    // Seed a live-looking potato held by slot 0 with a short fuse: under the gate, it must
    // be forced inert and its timer must never expire into a scored round-end.
    HotPotato potato{};
    potato.held = true;
    potato.holderSlot = 0;
    potato.position = session.slots[0].player.position;
    potato.explodeTimer = 0.5f;

    // 30 seconds of ticks -- far more than enough for 3 rounds of explosions to resolve a
    // whole match if the gate were absent.
    for (int tick = 0; tick < 60 * 30; tick++) {
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::TwoVTwo);
    }
    assert(potato.held == false && potato.inFlight == false); // forced inert, nothing live on the wire
    assert(potato.holderSlot == -1);
    assert(match.teamScore[0] == 0 && match.teamScore[1] == 0); // never scored
    assert(match.matchOver == false);                            // never froze on a bogus win
    assert(match.winnerSlot == -1);
    assert(match.roundNumber == 1);                              // never advanced a round
    assert(match.inTiebreak == false);
    assert(session.slots[0].player.state == PlayerState::Alive); // no explosion ever downed anyone

    // Charging/releasing is inert too while under-populated.
    latestInputs[0].chargingThrow = true;
    latestInputs[0].releaseThrow = true;
    {
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::TwoVTwo);
    }
    assert(potato.inFlight == false && potato.held == false);
    latestInputs[0] = InputMsg{};

    // A 3rd player still isn't enough (2v2 needs a full room, not merely both teams staffed).
    session.slots[2].state = SlotState::Connected;
    session.slots[2].player = Player(kSpawnPoints[2]);
    for (int tick = 0; tick < 60 * 10; tick++) {
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::TwoVTwo);
    }
    assert(match.teamScore[0] == 0 && match.teamScore[1] == 0);
    assert(match.matchOver == false);
    assert(potato.held == false);

    // 4th player joins: gameplay resumes. The very next tick seeds a fresh live potato.
    session.slots[3].state = SlotState::Connected;
    session.slots[3].player = Player(kSpawnPoints[3]);
    {
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::TwoVTwo);
    }
    assert(potato.held == true);
    assert(potato.holderSlot == 0); // first Alive active slot
    assert(potato.explodeTimer > 0.0f);

    // ...and scoring genuinely works again: run until the fuse burns down on slot 0
    // (Team A), which must credit Team B.
    for (int tick = 0; tick < 60 * 20 && match.teamScore[1] == 0; tick++) {
        bool active[kMaxPlayersPerSession];
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active,
                             potato, chargeTimer, latestInputs, courtBounds, match, GameMode::TwoVTwo);
    }
    assert(match.teamScore[1] == 1); // Team B credited for Team A's explosion
    assert(match.roundNumber == 2);  // round genuinely advanced

    // StartNewMatch respects the gate too: dropping back under 4 players must not hand
    // out a live potato via the debug menu's "New Match" button.
    session.slots[3].state = SlotState::Empty;
    session.slots[2].state = SlotState::Empty;
    StartNewMatch(session, match, potato, chargeTimer, GameMode::TwoVTwo);
    assert(potato.held == false && potato.inFlight == false && potato.holderSlot == -1);
    assert(match.teamScore[0] == 0 && match.teamScore[1] == 0);
}

void SmokeTestRevivalItemSpawnLocations() {
    Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
    Vector2 twoVTwoSpawn1{ -50.0f, 300.0f };
    Vector2 twoVTwoSpawn2{ 1050.0f, 300.0f };
    assert(!PointInRect(twoVTwoSpawn1, courtBounds));
    assert(!PointInRect(twoVTwoSpawn2, courtBounds));

    Vector2 ffaSpawn1{ 300.0f, 500.0f };
    Vector2 ffaSpawn2{ 700.0f, 100.0f };
    assert(PointInRect(ffaSpawn1, courtBounds));
    assert(PointInRect(ffaSpawn2, courtBounds));
}

void SmokeTestHotbarAndSelfHeal() {
    // Inventory: fixed 4-slot behavior — Add claims the first empty slot, Remove clears in
    // place (does not compact/shift), Count and SlotAt agree.
    {
        Inventory inv;
        assert(inv.Add(ItemType::RevivePotion) == true);
        assert(inv.Count(ItemType::RevivePotion) == 1);
        assert(inv.SlotAt(0).count == 1 && inv.SlotAt(0).type == ItemType::RevivePotion);

        assert(inv.Remove(ItemType::RevivePotion, 1) == true);
        assert(inv.Count(ItemType::RevivePotion) == 0);
        assert(inv.SlotAt(0).count == 0); // slot stays in place, empty — not erased/shifted

        // Re-adding after removal claims the same now-empty slot again (not a new one).
        assert(inv.Add(ItemType::RevivePotion) == true);
        assert(inv.SlotAt(0).count == 1);
    }

    // Player::TryHeal: clamps to kMaxHp, no-ops when not Alive.
    {
        Player p(Vector2{0.0f, 0.0f});
        p.hp = 90;
        p.TryHeal(30);
        assert(p.hp == Player::kMaxHp); // clamped, not 120

        p.state = PlayerState::Downed;
        int hpBefore = p.hp;
        p.TryHeal(30);
        assert(p.hp == hpBefore); // no-op while not Alive
    }

    // Self-heal via SimulateSessionTick: selected slot holds RevivePotion, no one revivable
    // nearby, usePressed fires once -> instant heal, one potion consumed, no channel.
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[0].player = Player(kSpawnPoints[0]);
        session.slots[0].player.hp = 50;
        session.slots[0].player.selectedSlot = 0;
        session.slots[0].player.inventory.Add(ItemType::RevivePotion);
        // Slot 1 present but far away and Alive (not revivable), so self-heal is the only
        // applicable path — proves the gate correctly distinguishes "no one to revive".
        session.slots[1].state = SlotState::Connected;
        session.slots[1].player = Player(Vector2{900.0f, 500.0f});

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = { true, false, false, false };
        bool usePressed[kMaxPlayersPerSession] = { true, false, false, false };
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{};
        bool active[kMaxPlayersPerSession];

        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed,
                             1.0f / 60.0f, active, potato, chargeTimer, latestInputs, courtBounds,
                             match, GameMode::RevivePotionTest);

        assert(session.slots[0].player.hp == 80); // 50 + 30, no clamp needed
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 0); // consumed
        assert(session.slots[0].player.channelTimer == 0.0f); // no channel occurred
    }

    // Revive-channel is blocked when a DIFFERENT slot is selected, even with a Downed
    // teammate in range and a potion in inventory (just not in the selected slot).
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[0].player.selectedSlot = 1; // NOT the slot holding the potion
        session.slots[0].player.inventory.Add(ItemType::RevivePotion); // lands in slot 0

        session.slots[1].state = SlotState::Connected;
        session.slots[1].player = Player(Vector2{5.0f, 0.0f}); // within reviveRange
        session.slots[1].player.ForceDown();

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = { true, false, false, false };
        bool usePressed[kMaxPlayersPerSession] = {};
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        // SimulateSessionTick re-derives selectedSlot from latestInputs every tick (clamped),
        // so the intended "wrong slot selected" setup must be expressed here too, not just on
        // the Player object directly — otherwise the first tick would reset it back to 0.
        latestInputs[0].selectedSlot = 1;
        MatchState match{};
        HotPotato potato{};
        bool active[kMaxPlayersPerSession];

        for (int tick = 0; tick < 200; tick++) { // far more than kChannelDuration(2s) at 60Hz
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed,
                                 1.0f / 60.0f, active, potato, chargeTimer, latestInputs, courtBounds,
                                 match, GameMode::RevivePotionTest);
        }

        assert(session.slots[1].player.state == PlayerState::Downed); // never revived
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 1); // never consumed
    }
}

// Regression tests for the two Critical findings in the hotbar/self-heal final review.
void SmokeTestUseFlagLatchingAndReviveOverlap() {
    // (a) Finding 1: the server latches the last-received InputMsg's edge-triggered
    // usePressed flag. Under UDP loss / framerate drift several server ticks can elapse
    // before a fresh packet overwrites that latch. A single physical E press must consume
    // exactly ONE potion, not one per tick the stale `true` is still sitting in the latch.
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[0].player = Player(kSpawnPoints[0]);
        session.slots[0].player.hp = 10;
        session.slots[0].player.selectedSlot = 0;
        for (int n = 0; n < 4; n++) session.slots[0].player.inventory.Add(ItemType::RevivePotion);
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 4);
        // Second player present, Alive and far away: nothing revivable, so self-heal is the
        // only path usePressed can take.
        session.slots[1].state = SlotState::Connected;
        session.slots[1].player = Player(Vector2{900.0f, 500.0f});

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = {};
        // ONE press latched by the server; no further packets arrive for the next 4 ticks.
        bool usePressed[kMaxPlayersPerSession] = { true, false, false, false };
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{};
        bool active[kMaxPlayersPerSession];

        for (int tick = 0; tick < 4; tick++) {
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed,
                                 1.0f / 60.0f, active, potato, chargeTimer, latestInputs, courtBounds,
                                 match, GameMode::RevivePotionTest);
        }

        // Exactly one heal off one press: 4 -> 3 potions, 10 -> 40 hp. Before the fix this
        // drained all 4 potions and healed 4 times.
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 3);
        assert(session.slots[0].player.hp == 40);
        // The latch itself must have been drained so a later tick can't see the stale press.
        assert(usePressed[0] == false);

        // A genuinely fresh packet re-arming the flag heals exactly once more.
        usePressed[0] = true;
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed,
                             1.0f / 60.0f, active, potato, chargeTimer, latestInputs, courtBounds,
                             match, GameMode::RevivePotionTest);
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 2);
        assert(session.slots[0].player.hp == 70);
    }

    // (b) Finding 2: UpdateRevive zeroes channelTimer on BOTH "no progress possible" and
    // "revive just completed", so the self-heal loop's `channelTimer > 0.0f` guard cannot
    // tell them apart. If usePressed happens to be true on the exact tick a revive channel
    // completes, the reviver used to ALSO self-heal — spending a second potion and healing
    // themselves off the single E press that was supposed to only revive the teammate.
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[0].player.hp = 50;
        session.slots[0].player.selectedSlot = 0;
        session.slots[0].player.inventory.Add(ItemType::RevivePotion);
        session.slots[0].player.inventory.Add(ItemType::RevivePotion); // 2 potions

        session.slots[1].state = SlotState::Connected;
        session.slots[1].player = Player(Vector2{5.0f, 0.0f}); // within kReviveRange
        session.slots[1].player.ForceDown();

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = { true, false, false, false }; // E held
        bool usePressed[kMaxPlayersPerSession] = {};
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{};
        bool active[kMaxPlayersPerSession];

        const float dt = 1.0f / 60.0f;
        // Channel until exactly one tick short of completion.
        int guard = 0;
        while (session.slots[0].player.channelTimer + dt < Player::kChannelDuration) {
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed,
                                 dt, active, potato, chargeTimer, latestInputs, courtBounds,
                                 match, GameMode::RevivePotionTest);
            assert(++guard < 1000);
        }
        assert(session.slots[1].player.state == PlayerState::Downed); // not yet revived
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 2);

        // The completing tick, with usePressed ALSO true (a fresh press landing on the very
        // tick the channel finishes).
        usePressed[0] = true;
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed,
                             dt, active, potato, chargeTimer, latestInputs, courtBounds,
                             match, GameMode::RevivePotionTest);

        assert(session.slots[1].player.state != PlayerState::Downed); // revive completed
        // Only the revive's own consumption: 2 -> 1, NOT 2 -> 0.
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 1);
        // The reviver heals nobody but the target: own HP unchanged at 50, not 80.
        assert(session.slots[0].player.hp == 50);
        assert(session.slots[0].player.channelTimer == 0.0f);
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

    SmokeTestTeamScoring();
    std::printf("SmokeTestTeamScoring passed\n");

    SmokeTestMatchOverFreezeAndNewMatch();
    std::printf("SmokeTestMatchOverFreezeAndNewMatch passed\n");

    SmokeTestDash();
    std::printf("SmokeTestDash passed\n");

    SmokeTestDashTunnelingAcrossTicks();
    std::printf("SmokeTestDashTunnelingAcrossTicks passed\n");

    SmokeTestGameModes();
    std::printf("SmokeTestGameModes passed\n");

    SmokeTestTwoVTwoPopulationGate();
    std::printf("SmokeTestTwoVTwoPopulationGate passed\n");

    SmokeTestRevivalItemSpawnLocations();
    std::printf("SmokeTestRevivalItemSpawnLocations passed\n");

    SmokeTestHotbarAndSelfHeal();
    std::printf("SmokeTestHotbarAndSelfHeal passed\n");

    SmokeTestUseFlagLatchingAndReviveOverlap();
    std::printf("SmokeTestUseFlagLatchingAndReviveOverlap passed\n");

    std::printf("All smoke tests passed\n");
}
