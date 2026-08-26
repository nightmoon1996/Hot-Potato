# Hot Potato Phase 1: Networking Foundation (2 -> 4 Player Slots) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand the server/client/protocol from a hardcoded 2-player-per-session model to support up to 4 players per session, with no behavior change to gameplay rules yet (Hot Potato mechanics themselves are later phases) — this phase only makes "up to 4 players can join one room and see each other" work correctly.

**Architecture:** `Session::slots` grows from `PlayerSlot[2]` to `PlayerSlot[4]`. `SnapshotMsg::players` grows from `[2]` to `[4]`. All per-player-pair server logic (attack, revive checks) that today is written as two hardcoded variables (`p0`, `p1`) becomes a nested loop over up to 4 active slots. The per-session parallel maps (`pendingAttack`, `pendingInteract`, `sessionHazardCarry`) grow their inner C-arrays from `[2]` to `[4]`. The client's render loop and `ClientEffectsState`'s per-player arrays grow the same way. Four spawn points (near each corner) replace the current two (left/right).

**Tech Stack:** C++17, existing UDP client/server protocol (Protocol.h field arrays resized, not restructured), raylib/raylib-cpp (client rendering only).

**Spec:** [docs/superpowers/specs/2026-08-26-hot-potato-design.md](../specs/2026-08-26-hot-potato-design.md) (Phase 1 section)

## Global Constraints

- This phase does NOT change any gameplay rule (attack damage, revive range, hazard damage, pickup radius) — it only widens the player-count ceiling from 2 to 4. The existing "Classic" game (pickup/inventory/revive/hazard/PvP) must work identically for 2-player sessions after this phase as it did before.
- All pairwise interactions (attack-can-hit, revive-can-target) that were previously two hardcoded checks (`p0` vs `p1`, `p1` vs `p0`) become an `O(n^2)` nested loop over active slots (n <= 4, so this is at most 16 iterations per tick per session — trivially cheap, no optimization needed).
- `SnapshotMsg`/`PlayerSnapshot` array size change from `[2]` to `[4]` is NOT wire-compatible with the previous protocol version — this is fine since client and server are always rebuilt together from the same source tree, but do not attempt to preserve old-client compatibility.
- Spawn points: 4 fixed positions, one per corner of the arena (exact coordinates specified in Task 1).
- `WorldItemSnapshot`/world-item arrays stay at `[2]` in this phase (not tied to player count; Phase 5's revive-item rework is a separate later phase).
- Build with `mingw32-make server` / `mingw32-make client` using the existing Makefile.
- Toolchain: MinGW at `C:\msys64\mingw64\bin`, add to PATH if needed (bash: `export PATH="/c/msys64/mingw64/bin:$PATH"`). If a rebuild fails with a linker "Permission denied," a previous `server.exe`/`client.exe` may be running — check `tasklist //FI "IMAGENAME eq server.exe"` / `client.exe` and ASK before killing (don't kill without asking).

---

### Task 1: Expand `Session`/`PlayerSlot` to 4 slots with 4 spawn points

**Files:**
- Modify: `src/server/Session.h`

**Interfaces:**
- Produces: `PlayerSlot slots[4]`, `kSlot0Spawn` through `kSlot3Spawn` constants — consumed by Task 2 (SessionManager), Task 3 (server main.cpp).

- [ ] **Step 1: Add 4 spawn points and expand the slots array**

In `src/server/Session.h`, replace:

```cpp
static constexpr Vector2 kSlot0Spawn{ 150.0f, 300.0f };
static constexpr Vector2 kSlot1Spawn{ 850.0f, 300.0f };
```

with:

```cpp
static constexpr Vector2 kSlot0Spawn{ 100.0f, 100.0f };   // top-left
static constexpr Vector2 kSlot1Spawn{ 900.0f, 100.0f };   // top-right
static constexpr Vector2 kSlot2Spawn{ 100.0f, 500.0f };   // bottom-left
static constexpr Vector2 kSlot3Spawn{ 900.0f, 500.0f };   // bottom-right

static constexpr Vector2 kSpawnPoints[4] = { kSlot0Spawn, kSlot1Spawn, kSlot2Spawn, kSlot3Spawn };
```

(The `kSpawnPoints` array lets callers do `kSpawnPoints[slotIndex]` instead of a 4-way if/else chain — used by Task 2 and Task 3.)

Replace:

```cpp
class Session {
public:
    PlayerSlot slots[2];

    int FindEmptySlot() const {
        for (int i = 0; i < 2; i++) {
            if (slots[i].state == SlotState::Empty) {
                return i;
            }
        }
        return -1;
    }

    int FindDisconnectedSlotByToken(uint32_t token) const {
        if (token == 0) {
            return -1;
        }
        for (int i = 0; i < 2; i++) {
            if (slots[i].state == SlotState::DisconnectedPending && slots[i].sessionToken == token) {
                return i;
            }
        }
        return -1;
    }

    void CheckTimeouts(double nowSeconds) {
        for (int i = 0; i < 2; i++) {
            if (slots[i].state == SlotState::Connected &&
                nowSeconds - slots[i].lastPacketAtSeconds >= 60.0) {
                slots[i].state = SlotState::DisconnectedPending;
            } else if (slots[i].state == SlotState::DisconnectedPending &&
                       nowSeconds - slots[i].lastPacketAtSeconds >= 120.0) {
                Vector2 spawn = (i == 0) ? kSlot0Spawn : kSlot1Spawn;
                slots[i] = PlayerSlot{};
                slots[i].player = Player(spawn);
            }
        }
    }
};
```

with:

```cpp
class Session {
public:
    PlayerSlot slots[4];

    int FindEmptySlot() const {
        for (int i = 0; i < 4; i++) {
            if (slots[i].state == SlotState::Empty) {
                return i;
            }
        }
        return -1;
    }

    int FindDisconnectedSlotByToken(uint32_t token) const {
        if (token == 0) {
            return -1;
        }
        for (int i = 0; i < 4; i++) {
            if (slots[i].state == SlotState::DisconnectedPending && slots[i].sessionToken == token) {
                return i;
            }
        }
        return -1;
    }

    void CheckTimeouts(double nowSeconds) {
        for (int i = 0; i < 4; i++) {
            if (slots[i].state == SlotState::Connected &&
                nowSeconds - slots[i].lastPacketAtSeconds >= 60.0) {
                slots[i].state = SlotState::DisconnectedPending;
            } else if (slots[i].state == SlotState::DisconnectedPending &&
                       nowSeconds - slots[i].lastPacketAtSeconds >= 120.0) {
                Vector2 spawn = kSpawnPoints[i];
                slots[i] = PlayerSlot{};
                slots[i].player = Player(spawn);
            }
        }
    }
};
```

- [ ] **Step 2: Build to confirm no compile breakage in this file alone**

Run: `mingw32-make bin/server/main.o` (this will fail to LINK the full server since `SessionManager.h`/`main.cpp` still reference the old 2-slot assumptions — that's expected and fixed in Tasks 2-3; this step just confirms `Session.h` itself has no syntax errors by way of compiling any translation unit that includes it).

Expected: compiles without error (link failure, if any, is fine/expected at this point — only a COMPILE error in this step is a problem).

- [ ] **Step 3: Commit**

```bash
git add src/server/Session.h
git commit -m "Expand Session to 4 player slots with 4 corner spawn points"
```

---

### Task 2: Expand `SessionManager` slot-assignment logic to 4 slots

**Files:**
- Modify: `src/server/SessionManager.h`

**Interfaces:**
- Consumes: `Session::slots[4]`, `kSpawnPoints[4]` (Task 1).
- Produces: `SessionManager::HandleConnect` now assigns any of 4 slots — consumed by Task 3 (server main.cpp) and Task 5 (smoke tests).

- [ ] **Step 1: Update slot-assignment call sites to use `kSpawnPoints[slotIndex]`**

In `src/server/SessionManager.h`, in the create path, replace:

```cpp
        // Create path: empty session name means "create a new room, generate a code"
        if (sessionName.empty()) {
            std::string code = GenerateUniqueRoomCode();
            Session newSession;
            uint32_t token = GenerateToken();
            newSession.slots[0].state = SlotState::Connected;
            newSession.slots[0].sessionToken = token;
            newSession.slots[0].clientIp = clientIp;
            newSession.slots[0].clientPort = clientPort;
            newSession.slots[0].lastPacketAtSeconds = nowSeconds;
            newSession.slots[0].player = Player(kSlot0Spawn);
            newSession.slots[1].player = Player(kSlot1Spawn);
            sessions[code] = newSession;
            return { ConnectResult::Created, 0, token, RejectReason::SessionFull, code };
        }
```

with:

```cpp
        // Create path: empty session name means "create a new room, generate a code"
        if (sessionName.empty()) {
            std::string code = GenerateUniqueRoomCode();
            Session newSession;
            uint32_t token = GenerateToken();
            newSession.slots[0].state = SlotState::Connected;
            newSession.slots[0].sessionToken = token;
            newSession.slots[0].clientIp = clientIp;
            newSession.slots[0].clientPort = clientPort;
            newSession.slots[0].lastPacketAtSeconds = nowSeconds;
            for (int i = 0; i < 4; i++) {
                newSession.slots[i].player = Player(kSpawnPoints[i]);
            }
            sessions[code] = newSession;
            return { ConnectResult::Created, 0, token, RejectReason::SessionFull, code };
        }
```

(Every slot's `Player` is constructed at its own spawn point up front, matching the existing pattern where `slots[1].player` was pre-constructed at `kSlot1Spawn` even before slot 1 has a connected client — this is harmless since an `Empty` slot's `Player` is inert until a client actually joins that slot.)

In the join path, replace:

```cpp
        Session& session = it->second;
        int emptySlot = session.FindEmptySlot();
        if (emptySlot != -1) {
            uint32_t token = GenerateToken();
            session.slots[emptySlot].state = SlotState::Connected;
            session.slots[emptySlot].sessionToken = token;
            session.slots[emptySlot].clientIp = clientIp;
            session.slots[emptySlot].clientPort = clientPort;
            session.slots[emptySlot].lastPacketAtSeconds = nowSeconds;
            session.slots[emptySlot].player = Player(emptySlot == 0 ? kSlot0Spawn : kSlot1Spawn);
            return { ConnectResult::Joined, emptySlot, token, RejectReason::SessionFull, sessionName };
        }
```

with:

```cpp
        Session& session = it->second;
        int emptySlot = session.FindEmptySlot();
        if (emptySlot != -1) {
            uint32_t token = GenerateToken();
            session.slots[emptySlot].state = SlotState::Connected;
            session.slots[emptySlot].sessionToken = token;
            session.slots[emptySlot].clientIp = clientIp;
            session.slots[emptySlot].clientPort = clientPort;
            session.slots[emptySlot].lastPacketAtSeconds = nowSeconds;
            session.slots[emptySlot].player = Player(kSpawnPoints[emptySlot]);
            return { ConnectResult::Joined, emptySlot, token, RejectReason::SessionFull, sessionName };
        }
```

- [ ] **Step 2: Build to confirm no compile breakage**

Run: `mingw32-make bin/server/main.o`

Expected: compiles without error.

- [ ] **Step 3: Commit**

```bash
git add src/server/SessionManager.h
git commit -m "Use 4-slot spawn point array in SessionManager create/join paths"
```

---

### Task 3: Expand server `main.cpp` runtime dispatch and simulation tick to 4 players

**Files:**
- Modify: `src/server/main.cpp`

**Interfaces:**
- Consumes: `Session::slots[4]` (Task 1), `SnapshotMsg::players[4]` (Task 4, dispatched in parallel-safe order — see note below).
- Produces: server correctly simulates and broadcasts state for up to 4 active players per session.

NOTE ON ORDERING: this task's code REFERENCES `SnapshotMsg::players[4]`, which Task 4 defines in `Protocol.h`. Since `main.cpp` already includes `Protocol.h`, and Task 4 is a small isolated protocol-only change, this task's implementer should coordinate: if Task 4 hasn't landed yet, this task's changes referencing `snap.players[2]`/`snap.players[3]` will fail to compile. The controller dispatches Task 4 BEFORE Task 3 to avoid this — verify Task 4 is already committed before starting this task (check `git log` for a commit modifying `src/shared/Protocol.h`'s `SnapshotMsg`/`PlayerSnapshot` array size).

- [ ] **Step 1: Expand `FindClientByAddress`'s slot-scan loop**

In `src/server/main.cpp`, replace:

```cpp
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
```

with:

```cpp
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
```

- [ ] **Step 2: Expand the per-session parallel maps from `[2]` to `[4]`**

Replace:

```cpp
    std::map<std::string, std::vector<WorldItem>> sessionWorldItems;
    std::map<std::string, float[2]> sessionHazardCarry;
    std::map<std::string, uint32_t> sessionSnapshotSeq;
    std::map<std::string, bool[2]> pendingAttack;
    std::map<std::string, bool[2]> pendingInteract;
```

with:

```cpp
    std::map<std::string, std::vector<WorldItem>> sessionWorldItems;
    std::map<std::string, float[4]> sessionHazardCarry;
    std::map<std::string, uint32_t> sessionSnapshotSeq;
    std::map<std::string, bool[4]> pendingAttack;
    std::map<std::string, bool[4]> pendingInteract;
```

- [ ] **Step 3: Expand hazard-carry initialization on session creation**

Replace:

```cpp
                            Session* session = sessionManager.GetSession(outcome.roomCode);
                            if (sessionWorldItems.find(outcome.roomCode) == sessionWorldItems.end()) {
                                sessionWorldItems[outcome.roomCode] = {
                                    WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
                                    WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
                                };
                                sessionHazardCarry[outcome.roomCode][0] = 0.0f;
                                sessionHazardCarry[outcome.roomCode][1] = 0.0f;
                                sessionSnapshotSeq[outcome.roomCode] = 1;
                            }
```

with:

```cpp
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
```

- [ ] **Step 4: Expand the debug-action target-slot bound check**

Replace:

```cpp
                                        if (deliveredReq.targetSlot < 2) {
                                            ApplyDebugAction(session->slots[deliveredReq.targetSlot].player, deliveredReq.action);
                                        }
```

with:

```cpp
                                        if (deliveredReq.targetSlot < 4) {
                                            ApplyDebugAction(session->slots[deliveredReq.targetSlot].player, deliveredReq.action);
                                        }
```

- [ ] **Step 5: Rewrite the per-session simulation tick body to loop over 4 players**

This is the main rewrite. Replace the ENTIRE body of the `for (auto& sessionEntry : sessionManager.GetSessionNames())` loop — from `Session* session = sessionManager.GetSession(sessionEntry);` through the closing brace of that same `for` loop (i.e. everything up to but not including the "Retransmit any unacked reliable messages" block's closing, which stays as a separate per-slot loop already written correctly for a runtime slot count — verify it already loops `for (int i = 0; i < 2; i++)` and change that bound to 4 as well, see Step 6) — with:

```cpp
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
```

**Important semantic note (a deliberate, minor gameplay-affecting change over the original 2-player logic, worth calling out in the commit message):** the original 2-player attack logic was `if (p0Active && attack[0]) { if (p1Active) TryAttack(p0, p1); attack[0] = false; }` — a player only tries to attack if THEY are active, but doesn't check whether the opponent's state (Downed/Dead) blocks the hit; `TryAttack` itself presumably has its own internal range check but may not check the target's state. This plan's 4-player version preserves that exact same delegation to `TryAttack`'s internal logic — it does NOT add a "target must be Alive" gate that wasn't in the original code, since Combat.h's `TryAttack` is out of this plan's scope to modify. If `TryAttack` already only succeeds against an Alive target internally, this is a non-issue; if it doesn't, that's pre-existing behavior unchanged by this plan.

- [ ] **Step 6: Expand the retransmit loop's bound**

Find the existing retransmit loop (immediately after the snapshot-broadcast loop, already present in the file, unchanged in structure) and change its bound from 2 to 4:

Replace:

```cpp
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
```

with:

```cpp
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
```

- [ ] **Step 7: Build**

Run: `mingw32-make server`

Expected: builds cleanly (this requires Task 4's `SnapshotMsg`/`PlayerSnapshot` array-size change to already be committed — verify via `git log --oneline -- src/shared/Protocol.h` before running this step; if Task 4 hasn't landed, STOP and report BLOCKED/NEEDS_CONTEXT rather than guessing at Protocol.h's shape).

- [ ] **Step 8: Run existing smoke tests to confirm no regression**

Run: `./bin/server.exe --test`

Expected: all existing smoke test suites still pass (this task doesn't change `SmokeTestSessionManager`'s test data, which uses exactly 2 players — those tests must still pass unmodified against the new 4-slot code, since 2-player behavior must be identical to before).

- [ ] **Step 9: Commit**

```bash
git add src/server/main.cpp
git commit -m "Expand server runtime dispatch and simulation tick to support 4 player slots"
```

---

### Task 4: Expand `SnapshotMsg`/`PlayerSnapshot` array size to 4 (protocol change)

**Files:**
- Modify: `src/shared/Protocol.h`

**Interfaces:**
- Produces: `SnapshotMsg::players[4]` — consumed by Task 3 (server) and Task 6 (client rendering).

**IMPORTANT — DISPATCH THIS TASK BEFORE TASK 3.** Task 3's code directly references a 4-element `players` array; if Task 3 is implemented against the current 2-element `Protocol.h`, it will not compile. The controller must dispatch and commit this task first, then dispatch Task 3.

- [ ] **Step 1: Expand the `players` array in `SnapshotMsg`**

In `src/shared/Protocol.h`, replace:

```cpp
struct SnapshotMsg {
    PlayerSnapshot players[2];
    WorldItemSnapshot items[2];
    float hazardX;
    float hazardY;
    float hazardW;
    float hazardH;
};
```

with:

```cpp
struct SnapshotMsg {
    PlayerSnapshot players[4];
    WorldItemSnapshot items[2];
    float hazardX;
    float hazardY;
    float hazardW;
    float hazardH;
};
```

(`WorldItemSnapshot items[2]` intentionally stays at 2 — world items are not tied to player count in this phase, per the Global Constraints.)

- [ ] **Step 2: Build both targets to confirm no compile breakage**

Run: `mingw32-make server && mingw32-make client`

Expected: this task alone, in isolation, will likely FAIL to fully build/link both targets, since `src/server/main.cpp` (not yet updated — that's Task 3) and `src/client/main.cpp`/`Juice.h`/`.cpp` (not yet updated — that's Task 6) still assume a 2-element array in places that construct/iterate `SnapshotMsg` by index. A build failure here referencing array-bounds or index usage in OTHER files (not `Protocol.h` itself) is EXPECTED and not a defect in this task — this task's own file (`Protocol.h`) has no logic to break, only a struct definition. Confirm via reading any compiler error that it points to `main.cpp`/`Juice.cpp`, not `Protocol.h`, before treating this as acceptable-and-expected. If the compiler error is somehow IN `Protocol.h` itself, that IS this task's problem — investigate and fix.

- [ ] **Step 3: Commit**

```bash
git add src/shared/Protocol.h
git commit -m "Expand SnapshotMsg::players array to 4 slots"
```

---

### Task 5: Extend server smoke tests for 4-player session fill/reject

**Files:**
- Modify: `src/server/main.cpp` (smoke tests only)

**Interfaces:**
- Consumes: `SessionManager::HandleConnect` with 4-slot support (Tasks 1, 2).

- [ ] **Step 1: Add a 4-player fill/reject test case to `SmokeTestSessionManager`**

Read the CURRENT state of `SmokeTestSessionManager()` in `src/server/main.cpp` yourself (Tasks 1-4 do not modify this function's existing test bodies, only the underlying slot-count support, so the existing tests should be unaffected — but confirm the function's exact current end before appending). Append this new test block at the end of the function, before its closing brace:

```cpp
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
```

- [ ] **Step 2: Build and run**

Run: `mingw32-make server && ./bin/server.exe --test`

Expected: all smoke tests (including the new 4-player fill case) pass with pristine output.

- [ ] **Step 3: Commit**

```bash
git add src/server/main.cpp
git commit -m "Add smoke test for 4-player session fill and 5th-joiner rejection"
```

---

### Task 6: Expand client rendering (`ClientEffectsState`, `main.cpp`, `DebugMenu`) to 4 players

**Files:**
- Modify: `src/client/Juice.h`
- Modify: `src/client/Juice.cpp`
- Modify: `src/client/main.cpp`
- Modify: `src/client/DebugMenu.h`

**Interfaces:**
- Consumes: `SnapshotMsg::players[4]` (Task 4).

- [ ] **Step 1: Expand `ClientEffectsState`'s per-player arrays in `Juice.h`**

In `src/client/Juice.h`, replace:

```cpp
private:
    // Diff caches (previous frame's values)
    int prevHp[2] = {0, 0};
    uint8_t prevState[2] = {kSnapshotStateAbsent, kSnapshotStateAbsent};
    int prevPotionCount[2] = {0, 0};
    bool prevItemActive[2] = {false, false};
    Vector2 prevItemPos[2]{};
    bool hasPrevFrame = false;

    // Presentation state
    float displayedHp[2] = {0.0f, 0.0f};
    float hitFlashTimer[2] = {0.0f, 0.0f};
    float shakeTrauma = 0.0f;
```

with:

```cpp
private:
    // Diff caches (previous frame's values)
    int prevHp[4] = {0, 0, 0, 0};
    uint8_t prevState[4] = {kSnapshotStateAbsent, kSnapshotStateAbsent, kSnapshotStateAbsent, kSnapshotStateAbsent};
    int prevPotionCount[4] = {0, 0, 0, 0};
    bool prevItemActive[2] = {false, false};
    Vector2 prevItemPos[2]{};
    bool hasPrevFrame = false;

    // Presentation state
    float displayedHp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float hitFlashTimer[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float shakeTrauma = 0.0f;
```

(`prevItemActive`/`prevItemPos` stay at `[2]` — world items are not tied to player count, matching Task 4's `WorldItemSnapshot items[2]` staying at 2.)

- [ ] **Step 2: Expand the player-diffing loop bound in `Juice.cpp`**

In `src/client/Juice.cpp`, in `ClientEffectsState::Update`, replace:

```cpp
    for (int i = 0; i < 2; i++) {
        const PlayerSnapshot& p = snap.players[i];
```

with:

```cpp
    for (int i = 0; i < 4; i++) {
        const PlayerSnapshot& p = snap.players[i];
```

(The rest of that loop's body is unchanged — it already correctly indexes `prevHp[i]`, `displayedHp[i]`, etc., which are now sized `[4]` per Step 1, so no other change is needed inside the loop body.)

The item-diffing loop (`for (int j = 0; j < 2; j++)`, operating on `snap.items`) is UNCHANGED — world items stay at 2 regardless of player count.

- [ ] **Step 3: Expand `main.cpp`'s player-drawing loop**

In `src/client/main.cpp`, replace:

```cpp
        drawPlayer(0, snap.players[0], BLUE, "P1");
        drawPlayer(1, snap.players[1], MAROON, "P2");
```

with:

```cpp
        static const Color kPlayerColors[4] = { BLUE, MAROON, GREEN, PURPLE };
        static const char* kPlayerLabels[4] = { "P1", "P2", "P3", "P4" };
        for (int i = 0; i < 4; i++) {
            drawPlayer(i, snap.players[i], kPlayerColors[i], kPlayerLabels[i]);
        }
```

- [ ] **Step 4: Expand `DebugMenu`'s column layout to 4 players**

In `src/client/DebugMenu.h`, replace:

```cpp
    void DrawAndHandle(NetClient& netClient) {
        if (!visible) {
            return;
        }

        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.5f));
        DrawRectangle(300, 150, 400, 260, RAYWHITE);
        DrawText("DEBUG MENU (F1 to close)", 320, 160, 16, BLACK);

        DrawText("P1 (slot 0)", 340, 190, 16, BLUE);
        DrawText("P2 (slot 1)", 540, 190, 16, MAROON);

        HandleColumn(netClient, 0, 320, 210);
        HandleColumn(netClient, 1, 520, 210);
    }
```

with:

```cpp
    void DrawAndHandle(NetClient& netClient) {
        if (!visible) {
            return;
        }

        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.5f));
        DrawRectangle(150, 150, 700, 260, RAYWHITE);
        DrawText("DEBUG MENU (F1 to close)", 170, 160, 16, BLACK);

        const char* labels[4] = { "P1 (slot 0)", "P2 (slot 1)", "P3 (slot 2)", "P4 (slot 3)" };
        Color colors[4] = { BLUE, MAROON, GREEN, PURPLE };
        for (int i = 0; i < 4; i++) {
            int x = 170 + i * 170;
            DrawText(labels[i], x, 190, 14, colors[i]);
            HandleColumn(netClient, (uint8_t)i, x, 210);
        }
    }
```

(The panel widens from 400px to 700px, and column labels shrink from 16px to 14px font to fit 4 columns at 170px spacing instead of 2 columns at 200px spacing — adjust spacing/width further only if a live check shows visual overlap; these are reasonable starting values.)

- [ ] **Step 5: Build**

Run: `mingw32-make client`

Expected: builds cleanly.

- [ ] **Step 6: Run client smoke tests**

Run: `./bin/client.exe --test`

Expected: all existing client smoke tests (Juice + RoomMenu) still pass — this task doesn't change the diffing/decay LOGIC, only array sizes, so behavior for slots 0-1 must be identical to before.

- [ ] **Step 7: Commit**

```bash
git add src/client/Juice.h src/client/Juice.cpp src/client/main.cpp src/client/DebugMenu.h
git commit -m "Expand client rendering and debug menu to support 4 player slots"
```

---

## Final Verification

After all six tasks:

- [ ] Run `mingw32-make server && mingw32-make client` — both build cleanly.
- [ ] Run `./bin/server.exe --test` — all server smoke tests (including the new 4-player fill test) pass.
- [ ] Run `./bin/client.exe --test` — all client smoke tests pass.
- [ ] Live verification: start `bin/server.exe`, connect 4 `bin/client.exe` instances to the same room code, confirm all 4 players are visible to each other, can move independently, and the existing Classic-mode mechanics (pickup, revive, hazard, PvP attack, debug menu with 4 columns) work correctly with 3-4 players active (not just 2). Also confirm a 2-player session still behaves exactly as before (no regression for the existing player count).
- [ ] Confirm a 5th connection attempt to a full 4-player room is rejected with `SessionFull` (live, via a 5th client instance or by inspecting server logs/behavior).
