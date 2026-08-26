# Hot Potato Phase 5: Game Modes (FFA / 2v2 / Revive Test) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce a `GameMode` selected at room-creation time, with three modes: **HotPotatoFFA** (today's default behavior, unchanged), **HotPotato2v2** (fixed team assignment by slot pair, team-based scoring, requires exactly 4 players, revive items spawn outside the arena), and **RevivePotionTest** (a small 2-player-oriented mode with NO potato at all — the hazard zone is re-enabled and deals real HP damage-over-time via the original Classic mechanic, downing a player at 0 HP; revive items spawn at their original in-arena positions; Downed→Dead auto-respawns exactly like the original Classic game, since there's no round/match concept to gate a reset on).

**Architecture:** `GameMode` is a new enum (`FFA`, `TwoVTwo`, `RevivePotionTest`) chosen by the client's `RoomMenu` before creating a room, sent in `ConnectRequestMsg`, stored on the server's per-session state (a new `sessionGameMode` map, parallel to `sessionMatch`/`sessionPotato`), and echoed back in `WelcomeMsg` so the client knows which HUD/behavior to render. `SimulateSessionTick` gains a `GameMode` parameter and branches on it at exactly three points: (1) whether the hazard zone's `ApplyHazardDamage` runs at all (only `RevivePotionTest`), (2) whether the entire Hot-Potato charge/throw/flight/catch/explosion block runs at all (skipped for `RevivePotionTest`, which has no potato), (3) whether Dead-state auto-respawn is allowed (already governed by `Player::UpdateTimers`, which is NOT itself touched — instead, `RevivePotionTest` sessions simply never call anything that would prevent `UpdateTimers`'s existing unconditional Dead→auto-respawn behavior, since that behavior was never disabled in the first place; only `FFA`/`TwoVTwo`'s existing round-reset-based revival stays as-is, untouched by this phase). Scoring branches by mode: `FFA` keeps `MatchState::roundScore[kMaxPlayersPerSession]` as today; `TwoVTwo` uses a new team-scoring path that credits both members of the non-losing team; `RevivePotionTest` does no scoring at all (no match/round concept applies).

**Tech Stack:** C++17, existing UDP client/server protocol (additive fields), raylib/raylib-cpp (a mode-cycling UI control on `RoomMenu`, mode-appropriate HUD rendering).

**Spec:** [docs/superpowers/specs/2026-08-26-hot-potato-design.md](../specs/2026-08-26-hot-potato-design.md) (Phase 5 section — "2v2 Mode"), plus the following scope amendments agreed for this plan specifically (not in the original spec text, decided during this plan's brainstorming):
- The original spec's third "Classic" mode (full melee/PvP/original hazard-HP game, fully backward compatible with pre-Hot-Potato behavior) is OUT OF SCOPE for this plan — every session built since Phase 2 is already unconditionally Hot Potato, and restoring true Classic-mode parity would be a substantially larger, separate effort. Instead, a NEW, smaller **RevivePotionTest** mode is added: a 2-player-oriented, potato-free mode using the ORIGINAL hazard-zone HP-damage mechanic (not Hot-Potato's instant-Down) purely to let a player exercise the Downed→revive→Alive flow with the hazard zone as the down-trigger, and ORIGINAL Classic-style Dead auto-respawn (not Hot Potato's round-scoped, no-auto-respawn behavior) since this mode has no round/match concept to gate a reset on.
- `RevivePotionTest`'s revive items spawn at their ORIGINAL in-arena positions (`{300,500}`, `{700,100}` — the same coordinates already in the code today), not the outside-the-court positions Hot Potato 2v2 uses.
- 2v2's HUD shows TEAM scores only (`Team A: N` / `Team B: N`), not per-player scores.
- Mode selection UI: the `RoomMenu`'s single "Create Room" button is kept; a small mode-cycling control above it lets the player cycle through `FFA` / `2v2` / `Revive Test` before clicking Create. Joining an existing room inherits whatever mode its creator picked (no mode choice on Join).

## Global Constraints

- `GameMode::FFA` must produce IDENTICAL behavior to what exists today (Phases 1-4's already-shipped Hot Potato FFA) — this is a regression-sensitive constraint; every existing FFA-path smoke test must continue passing unchanged.
- `GameMode::TwoVTwo` requires exactly 4 CONNECTED players to be considered "started" for gameplay purposes — but per this project's existing session-fill pattern (rooms already accept 1-4 players and simulate whoever is active), 2v2 does NOT need a new "waiting room" gate; it simply behaves like FFA with team-based scoring once 4 are present, and if fewer than 4 are connected, existing `active[]`-gating already means only present players are simulated (an inactive slot's team is simply never a factor). This is a deliberate simplification over the spec's "room stays in a waiting state" language — no new pre-match lobby/ready-check UI is being added this phase.
- Team assignment for `TwoVTwo`: slots 0+1 = Team A, slots 2+3 = Team B, fixed by slot index, no picker UI.
- `GameMode::RevivePotionTest` disables the ENTIRE Hot-Potato charge/throw/flight/catch/explosion block (no potato exists in this mode at all — the client must not render one either) and enables the hazard zone (`ApplyHazardDamage`, using the existing, unmodified `Hazard.h`/`kHazardDamagePerSecond`/`HazardZone` — none of that code changes, it's simply CALLED again for this mode).
- No changes to `Player.h`'s `UpdateTimers` — its Dead-state auto-respawn behavior is untouched and always runs for every player in every mode; `FFA`/`TwoVTwo` already rely on round-resets happening far more often in practice (via potato round-ends) so auto-respawn rarely if ever fires there in normal play, while `RevivePotionTest` has no round concept and therefore is the mode where auto-respawn is actually the PRIMARY way a Dead player returns.
- Protocol changes additive only (`ConnectRequestMsg` gains one new field, `WelcomeMsg` gains one new field, both appended at the end).
- Builds with `mingw32-make server` / `mingw32-make client` using the existing Makefile.
- Toolchain: MinGW at `C:\msys64\mingw64\bin`, add to PATH if needed (bash: `export PATH="/c/msys64/mingw64/bin:$PATH"`). If a rebuild fails with a linker "Permission denied," a previous `server.exe`/`client.exe` may be running — check `tasklist //FI "IMAGENAME eq server.exe"` / `client.exe` and ASK before killing (don't kill without asking; this has happened in prior phases of this same project and the correct behavior is to stop and ask, not force-kill unilaterally).

---

### Task 1: `GameMode` enum, protocol plumbing, and per-session mode storage

**Files:**
- Modify: `src/shared/Protocol.h`
- Modify: `src/server/main.cpp`

**Interfaces:**
- Produces: `enum class GameMode : uint8_t { FFA, TwoVTwo, RevivePotionTest }` (in `Protocol.h`, shared by client and server); `ConnectRequestMsg::requestedMode` (new field, only meaningful on a CREATE request — i.e. empty `sessionName`; ignored on a JOIN request, since the room's mode was already fixed at creation); `WelcomeMsg::gameMode` (echoes back whichever mode the session actually is, whether the caller created it or joined an existing one); a new per-session `std::map<std::string, GameMode> sessionGameMode` in `main()`. Consumed by Task 2 (tick branching), Task 3 (team logic), Task 4 (item spawn locations), Task 5 (client mode selection + HUD).

- [ ] **Step 1: Add `GameMode` enum and the two new protocol fields**

In `src/shared/Protocol.h`, add the enum near the other enums (e.g. after `RejectReason`):

```cpp
enum class GameMode : uint8_t {
    FFA,
    TwoVTwo,
    RevivePotionTest
};
```

Modify `ConnectRequestMsg` — replace:

```cpp
struct ConnectRequestMsg {
    char sessionName[32];
    uint32_t reconnectToken; // 0 = no token
};
```

with:

```cpp
struct ConnectRequestMsg {
    char sessionName[32];
    uint32_t reconnectToken; // 0 = no token
    GameMode requestedMode; // only meaningful when sessionName is empty (a create request);
                            // ignored when joining an existing room, since the room's mode
                            // was already fixed by whoever created it
};
```

Modify `WelcomeMsg` — find its closing field (`char roomCode[7];`) and append:

```cpp
struct WelcomeMsg {
    uint8_t playerSlot;
    uint32_t sessionToken;
    int maxHp;
    float downedDuration;
    float deathRespawnDelay;
    int reviveHp;
    int respawnHp;
    float attackCooldown;
    float attackRange;
    int attackDamage;
    float channelDuration;
    float hazardDamagePerSecond;
    int inventoryCapacity;
    char roomCode[7]; // 6 digits + null terminator
    GameMode gameMode; // the ACTUAL mode of the room the caller ended up in (create or join)
};
```

- [ ] **Step 2: Add per-session `GameMode` storage in `main()`**

In `src/server/main.cpp`, add a new map alongside the existing `sessionMatch`/`sessionPotato` declarations:

```cpp
    std::map<std::string, GameMode> sessionGameMode;
```

In the session-CREATION block (the `if (sessionWorldItems.find(outcome.roomCode) == sessionWorldItems.end()) { ... }` block that runs only the first time a room code is seen), store the requested mode:

```cpp
                                sessionGameMode[outcome.roomCode] = msg.requestedMode;
```

Place this assignment inside that same creation-only block (read the CURRENT code carefully to find the exact right spot — it should be set once, at creation, alongside `sessionMatch[outcome.roomCode] = MatchState{};`, and NEVER overwritten on a later join, since `msg.requestedMode` is meaningless/undefined on a join request per this task's own field-comment).

- [ ] **Step 3: Populate `WelcomeMsg::gameMode` in the response**

Find the `WelcomeMsg welcome{ ... };` aggregate-init in the `ConnectRequest` handling branch and add the new trailing field:

```cpp
                            WelcomeMsg welcome{
                                (uint8_t)outcome.slotIndex, outcome.sessionToken,
                                Player::kMaxHp, Player::kDownedDuration, Player::kDeathRespawnDelay,
                                Player::kReviveHp, Player::kRespawnHp, Player::kAttackCooldown,
                                Player::kAttackRange, Player::kAttackDamage, Player::kChannelDuration,
                                kHazardDamagePerSecond, Inventory::kCapacity, {},
                                sessionGameMode[outcome.roomCode]
                            };
```

(Reading `sessionGameMode[outcome.roomCode]` works correctly for BOTH a fresh create — Step 2 just set it — and a join to an existing room — it was set once at that room's creation and never touched since.)

- [ ] **Step 4: Build**

Run: `mingw32-make server && mingw32-make client`

Expected: builds cleanly — this task is purely additive plumbing with no behavior branching yet (every session still behaves exactly like `FFA` today, since nothing reads `sessionGameMode` for branching purposes until Task 2).

- [ ] **Step 5: Commit**

```bash
git add src/shared/Protocol.h src/server/main.cpp
git commit -m "Add GameMode enum and per-session mode storage/plumbing"
```

---

### Task 2: Server tick branching — hazard on/off, potato on/off by mode

**Files:**
- Modify: `src/server/main.cpp`

**Interfaces:**
- Consumes: `GameMode` (Task 1).
- Produces: `SimulateSessionTick` gains a `GameMode mode` parameter; the hazard-damage call and the entire Hot-Potato block are both gated on `mode`.

Read the CURRENT state of `src/server/main.cpp`'s `SimulateSessionTick` yourself before editing — it has evolved significantly across Phases 2-4 (the Hot-Potato block is now large, spanning charge/release/flight/catch/bounds/explosion/dash-adjacent reclaim logic, all inside the existing `if (match.matchOver) { ... } else { ... }` guard from Phase 3).

- [ ] **Step 1: Grow `SimulateSessionTick`'s signature**

Change the forward declaration and definition from:

```cpp
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds, MatchState& match);
```

to:

```cpp
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds, MatchState& match,
                                GameMode mode);
```

- [ ] **Step 2: Re-enable hazard damage, gated on `mode == GameMode::RevivePotionTest`**

Find where `pickup` logic runs inside `SimulateSessionTick` (this is unconditional today, for every mode — leave it unconditional, revive-item pickup makes sense in all three modes). Immediately after the pickup block and BEFORE the "Hot Potato" comment/block begins, add:

```cpp
    // Hazard zone: only active in RevivePotionTest, which uses the ORIGINAL Classic HP-loss
    // mechanic (not Hot Potato's instant-Down) as its down-trigger, since it has no potato.
    if (mode == GameMode::RevivePotionTest) {
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (active[i]) ApplyHazardDamage(hazard, session.slots[i].player, dt, hazardCarry[i]);
        }
    }
```

(`ApplyHazardDamage` is already declared via `#include "Hazard.h"`, already included by `main.cpp` — confirm this include is still present; it should be, since `Hazard.h` was never removed, only its call site was.)

- [ ] **Step 3: Gate the entire Hot-Potato block on `mode != GameMode::RevivePotionTest`**

Find the existing Hot-Potato block, which currently starts with:

```cpp
    // --- Hot Potato: charge tracking, throw, flight, catch, explosion ---
    bool soloMode = false;
    {
        int activeCount = 0;
        for (int i = 0; i < kMaxPlayersPerSession; i++) if (active[i]) activeCount++;
        soloMode = (activeCount == 1);
    }

    if (match.matchOver) {
        ...
    } else {
        ...
    }

    // Holder vanished (disconnected, downed, or dead): reclaim the potato immediately.
    ...

    // Explosion: timer expires while held.
    ...

    // Held potato tracks its holder's position each tick.
    ...
```

Wrap this ENTIRE block (from the `// --- Hot Potato ---` comment through the end of the "Held potato tracks its holder's position" block, i.e. everything up to but NOT including the revive-loop comment that follows) in:

```cpp
    if (mode != GameMode::RevivePotionTest) {
        // --- Hot Potato: charge tracking, throw, flight, catch, explosion --- (existing content, now indented one level)
        ...
    }
```

Read the actual current code precisely to find the correct start/end boundaries — do not guess based on this plan's abbreviated sketch. The revive loop, the `UpdateTimers` loop, and the (Phase 4) dash-cooldown-decay loop that follow the Hot-Potato block must remain OUTSIDE this new `if` — they apply to every mode (revive-channeling and player-state timers are relevant in `RevivePotionTest` too; dash's cooldown decay is harmless to leave running even in a mode with no meaningful use for dash — Global Constraints don't require removing dash from `RevivePotionTest`, and there's no reason to special-case it out, so leave it fully functional in all three modes for simplicity, matching the spec's silence on dash-per-mode).

- [ ] **Step 4: Update the tick-loop call site**

Find:

```cpp
                SimulateSessionTick(*session, items, hazard, hazardCarry, attack, interact, dt, active, potato, chargeTimer, latestInputs, courtBounds, match);
```

replace with:

```cpp
                GameMode mode = sessionGameMode.count(sessionEntry) ? sessionGameMode[sessionEntry] : GameMode::FFA;
                SimulateSessionTick(*session, items, hazard, hazardCarry, attack, interact, dt, active, potato, chargeTimer, latestInputs, courtBounds, match, mode);
```

(The `.count(...) ? ... : GameMode::FFA` fallback guards against a theoretical missing-entry case the same defensive way other session-scoped lookups in this file already do — read nearby code for the established idiom and match it.)

- [ ] **Step 5: Update ALL pre-existing test call sites of `SimulateSessionTick`**

Every existing smoke test that calls `SimulateSessionTick` directly (from Phases 2-4: `SmokeTestSimulationTick`, `SmokeTestHotPotato`'s various blocks, `SmokeTestMatchOverFreezeAndNewMatch` if that's a separate function, etc. — read the actual current file to find all call sites) needs a `GameMode` argument added. For all EXISTING tests (which test Hot-Potato behavior), pass `GameMode::FFA` to preserve their exact current behavior/assertions unchanged.

- [ ] **Step 6: Add smoke tests for mode-gated behavior**

Add a new smoke test function `SmokeTestGameModes()` covering:

```cpp
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
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{}; // never used in this mode, but the signature still requires one

        int startingHp = session.slots[0].player.hp;
        for (int tick = 0; tick < 60; tick++) { // 1 second at 60Hz
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, 1.0f / 60.0f, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::RevivePotionTest);
        }
        assert(session.slots[0].player.hp < startingHp); // hazard damage applied
        assert(potato.held == false && potato.inFlight == false); // potato never activated

        // Confirm charging/releasing input is simply ignored in this mode (no potato exists)
        latestInputs[0].chargingThrow = true;
        latestInputs[0].releaseThrow = true;
        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, 1.0f / 60.0f, nullptr,
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
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{};
        potato.held = true;
        potato.holderSlot = 0;
        potato.position = session.slots[0].player.position;

        int startingHp = session.slots[0].player.hp;
        for (int tick = 0; tick < 60; tick++) {
            bool active[kMaxPlayersPerSession];
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, 1.0f / 60.0f, active,
                                 potato, chargeTimer, latestInputs, courtBounds, match, GameMode::FFA);
        }
        assert(session.slots[0].player.hp == startingHp); // hazard does NOT apply in FFA
    }
}
```

Register `SmokeTestGameModes()` in `RunAllSmokeTests()`.

- [ ] **Step 7: Build and run**

Run: `mingw32-make server && ./bin/server.exe --test`

Expected: ALL smoke tests pass, including every pre-existing Hot-Potato test (now passing `GameMode::FFA` explicitly) and the new `SmokeTestGameModes`.

- [ ] **Step 8: Commit**

```bash
git add src/server/main.cpp
git commit -m "Gate hazard damage and the Hot Potato block by GameMode in the simulation tick"
```

---

### Task 3: Team assignment and team-based scoring for 2v2

**Files:**
- Modify: `src/server/MatchState.h`
- Modify: `src/server/main.cpp`

**Interfaces:**
- Produces: `MatchState` gains `int teamScore[2]` (parallel to the existing `roundScore[kMaxPlayersPerSession]`, used only when the session's mode is `TwoVTwo`); a new free function `int TeamForSlot(int slot)` (returns 0 for slots 0-1, 1 for slots 2-3); a new `ScoreRoundEndTeam(MatchState& match, const bool* active, int excludedSlot)` mirroring `ScoreRoundEnd`'s existing exclusion/tiebreak-eligibility logic but crediting `teamScore[]` by team membership instead of `roundScore[]` per-slot; the two scoring call sites in `SimulateSessionTick` (explosion, out-of-bounds) branch on `mode == GameMode::TwoVTwo` to call the team-scoring path instead of the existing per-slot path.

Read the CURRENT state of `src/server/MatchState.h` and the two scoring call sites in `src/server/main.cpp`'s `SimulateSessionTick` (from Phase 3) before editing.

- [ ] **Step 1: Add `teamScore` and `TeamForSlot` to `MatchState.h`**

In `src/server/MatchState.h`, add near the top (after existing includes/before the struct, or as a free function near `ScoreRoundEnd` — your judgment on the cleanest spot, matching the file's existing style):

```cpp
// Fixed team assignment for 2v2: slots 0+1 = Team A (0), slots 2+3 = Team B (1).
inline int TeamForSlot(int slot) {
    return (slot < 2) ? 0 : 1;
}
```

Add `int teamScore[2] = {0, 0};` to the `MatchState` struct, alongside the existing `roundScore[kMaxPlayersPerSession]` field.

- [ ] **Step 2: Add `ScoreRoundEndTeam`**

Append to `MatchState.h`, mirroring `ScoreRoundEnd`'s existing structure:

```cpp
// 2v2 variant of ScoreRoundEnd: credits the NON-losing team's teamScore, not individual
// players. `excludedSlot` is the round's cause (exploded holder / out-of-bounds thrower);
// the team OPPOSITE that player's team gets +1. Tiebreak eligibility (when inTiebreak) is
// tracked per-team here rather than per-slot — reuses the same tiebreakEligible[4] array,
// but only ever needs indices 0/1 meaningfully populated for a 2-team tiebreak (2v2 never
// has more than 2 contending parties, so the richer per-slot narrowing FFA's tiebreak needs
// is unnecessary complexity here; a direct 2-team comparison suffices).
inline void ScoreRoundEndTeam(MatchState& match, const bool* active, int excludedSlot) {
    if (excludedSlot < 0 || excludedSlot >= kMaxPlayersPerSession) return;
    int losingTeam = TeamForSlot(excludedSlot);
    int winningTeam = 1 - losingTeam;
    match.teamScore[winningTeam] += 1;
}
```

- [ ] **Step 3: Add a team-based round-advance/match-resolution path**

Append to `MatchState.h`:

```cpp
// 2v2 variant of AdvanceRoundOrEndMatch: exactly 2 teams, so a tie is always a straight
// 2-way tie — no eligibility-narrowing tiebreak loop is needed (that complexity in the FFA
// path exists to handle 3+ tied parties, which cannot happen with exactly 2 teams). The
// very next round-end's scoring (which team gets credited) IS the tiebreak resolution.
inline void AdvanceRoundOrEndMatchTeam(MatchState& match) {
    if (match.matchOver) return;

    if (match.inTiebreak) {
        // Any single round-end during a tiebreak immediately decides it: whichever team
        // just scored (i.e. now leads) wins outright, since with 2 teams "still tied" after
        // a scoring round-end is impossible (only one team's score can change per round-end).
        if (match.teamScore[0] != match.teamScore[1]) {
            match.matchOver = true;
            match.winnerSlot = (match.teamScore[0] > match.teamScore[1]) ? 0 : 1; // stores the WINNING TEAM index, not a player slot, for 2v2's HUD to interpret
        }
        return;
    }

    match.roundNumber += 1;
    if (match.roundNumber > kRoundsPerMatch) {
        if (match.teamScore[0] != match.teamScore[1]) {
            match.matchOver = true;
            match.winnerSlot = (match.teamScore[0] > match.teamScore[1]) ? 0 : 1;
        } else {
            match.inTiebreak = true;
        }
    }
}
```

(Note: for `TwoVTwo`, `match.winnerSlot` is repurposed to mean "winning TEAM index" (0 or 1), not a player slot — this is a deliberate, documented reuse of the existing field rather than adding a new one, since the two concepts (FFA's per-slot winner, 2v2's per-team winner) are never both meaningful for the same `MatchState` instance at once, given a session's mode never changes mid-match. Task 5's client HUD code must interpret `winnerSlot` differently based on the session's `GameMode`.)

- [ ] **Step 4: Branch the two scoring call sites in `SimulateSessionTick` on mode**

Find the explosion round-end site (from Phase 3):

```cpp
        if (potato.explodeTimer <= 0.0f) {
            int exploderSlot = potato.holderSlot;
            session.slots[exploderSlot].player.ForceDown();
            ScoreRoundEnd(match, active, exploderSlot);
            AdvanceRoundOrEndMatch(match, active);
            ResetPotatoForNewRound(potato, session, active, chargeTimer);
        }
```

replace with:

```cpp
        if (potato.explodeTimer <= 0.0f) {
            int exploderSlot = potato.holderSlot;
            session.slots[exploderSlot].player.ForceDown();
            if (mode == GameMode::TwoVTwo) {
                ScoreRoundEndTeam(match, active, exploderSlot);
                AdvanceRoundOrEndMatchTeam(match);
            } else {
                ScoreRoundEnd(match, active, exploderSlot);
                AdvanceRoundOrEndMatch(match, active);
            }
            ResetPotatoForNewRound(potato, session, active, chargeTimer);
        }
```

Find the out-of-bounds round-end site (from Phase 3):

```cpp
                } else if (potato.lastThrowerSlot != -1 && active[potato.lastThrowerSlot]) {
                    session.slots[potato.lastThrowerSlot].player.ForceDown();
                    ScoreRoundEnd(match, active, potato.lastThrowerSlot);
                    AdvanceRoundOrEndMatch(match, active);
                    ResetPotatoForNewRound(potato, session, active, chargeTimer);
                }
```

replace with:

```cpp
                } else if (potato.lastThrowerSlot != -1 && active[potato.lastThrowerSlot]) {
                    session.slots[potato.lastThrowerSlot].player.ForceDown();
                    if (mode == GameMode::TwoVTwo) {
                        ScoreRoundEndTeam(match, active, potato.lastThrowerSlot);
                        AdvanceRoundOrEndMatchTeam(match);
                    } else {
                        ScoreRoundEnd(match, active, potato.lastThrowerSlot);
                        AdvanceRoundOrEndMatch(match, active);
                    }
                    ResetPotatoForNewRound(potato, session, active, chargeTimer);
                }
```

Read the ACTUAL current code at both sites carefully — the exact surrounding lines may include Phase 4's dash-tunneling-fix additions (a segment-catch-check call site exists near here too, from Phase 4's final fix wave) — do not disturb anything else in this vicinity, only the two `if (mode == ...)` branches described above.

- [ ] **Step 5: Populate `snap.match` with team score data**

Find the `MatchSnapshot matchSnap{}; ... snap.match = matchSnap;` block (from Phase 3) and add the two team-score fields — but `MatchSnapshot` doesn't currently have a `teamScore` field. Add it: in `src/shared/Protocol.h`, find `struct MatchSnapshot` and append `int teamScore[2];` as its last field. Then in `main.cpp`'s snapshot-building code, add:

```cpp
                matchSnap.teamScore[0] = match.teamScore[0];
                matchSnap.teamScore[1] = match.teamScore[1];
```

- [ ] **Step 6: Build**

Run: `mingw32-make server && mingw32-make client`

Expected: builds cleanly.

- [ ] **Step 7: Add smoke tests for team scoring**

Add a new smoke test `SmokeTestTeamScoring()` covering:

```cpp
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

    // Tie after 3 rounds triggers a tiebreak; the NEXT round-end resolves it immediately
    {
        MatchState match{};
        bool active[kMaxPlayersPerSession] = { true, true, true, true };
        // Alternate which team is the cause so both end up tied
        ScoreRoundEndTeam(match, active, 2); AdvanceRoundOrEndMatchTeam(match); // A: 1, B: 0
        ScoreRoundEndTeam(match, active, 0); AdvanceRoundOrEndMatchTeam(match); // A: 1, B: 1
        ScoreRoundEndTeam(match, active, 2); AdvanceRoundOrEndMatchTeam(match); // A: 2, B: 1
        ScoreRoundEndTeam(match, active, 0); AdvanceRoundOrEndMatchTeam(match); // A: 2, B: 2 -- wait, this is round 4, past kRoundsPerMatch(3)
        // Recompute: after exactly 3 ScoreRoundEndTeam+Advance calls, roundNumber becomes 4 (>3),
        // so the 3rd call's AdvanceRoundOrEndMatchTeam is what evaluates the post-round-3 state.
        // Re-derive precisely: round 1 (A:1,B:0), round 2 (A:1,B:1), round 3 (A:2,B:1) -- NOT tied,
        // so this specific sequence doesn't actually produce a tie. Fix: use a sequence that DOES
        // tie after exactly 3 rounds, e.g. cause alternates A,B,A giving A:1,B:0 then A:1,B:1 then
        // A:2,B:1 -- still not tied. To tie after 3 (odd) rounds each awarding exactly 1 point to
        // one side, an exact tie is impossible with 3 total points split between 2 teams (someone
        // has at least 2). A genuine tie after 3 rounds requires the SAME team to not always be
        // credited a full point per round in this simple model -- since every round-end here always
        // credits exactly one team +1, three rounds always produce an odd total (e.g. 2-1 or 3-0),
        // never a 1.5-1.5 tie. CORRECTION: this specific test scenario (tie after 3 rounds in 2v2)
        // is IMPOSSIBLE under this scoring model with an odd number of rounds each awarding exactly
        // one point -- the implementer must recognize this during implementation and either (a) test
        // the tiebreak path using a DIFFERENT mechanism (e.g. directly constructing a MatchState with
        // match.teamScore = {1,1} and match.roundNumber already at kRoundsPerMatch, then calling
        // AdvanceRoundOrEndMatchTeam once to observe inTiebreak become true), rather than trying to
        // reach a tie through 3 rounds of ScoreRoundEndTeam calls, which cannot produce one.
    }
}
```

**IMPORTANT NOTE for the implementer**: the test sketch above contains a DELIBERATE self-correction demonstrating a real mathematical fact you must account for: with exactly `kRoundsPerMatch = 3` rounds and each round-end always crediting exactly one team, the two teams' scores after 3 rounds are ALWAYS `{3,0}`, `{2,1}`, `{1,2}`, or `{0,3}` — never tied (3 is odd, can't split evenly between 2). This means the "tie after 3 normal rounds" path can genuinely never be reached in 2v2 through normal round-by-round play with this scoring model. Do NOT try to force the test sketch above to work as literally written — instead, write a smoke test that DIRECTLY constructs a `MatchState` with `teamScore = {1, 1}` and `roundNumber` already at `kRoundsPerMatch`, then calls `AdvanceRoundOrEndMatchTeam` once and asserts `inTiebreak` becomes `true` (this exercises the tiebreak-ENTRY logic in isolation, which is the part that's actually reachable and testable, even though the specific "3 rounds of alternating team credit" path that would naturally lead there doesn't exist for odd round counts). Then, continuing from that artificially-constructed tied state, call `ScoreRoundEndTeam` once more (crediting either team) and `AdvanceRoundOrEndMatchTeam` again, and assert `matchOver` becomes `true` with `winnerSlot` correctly reflecting whichever team just scored (this exercises the tiebreak-RESOLUTION logic). This is a case where recognizing a plan's own test sketch doesn't actually work, and replacing it with a correct one that tests the same underlying logic via direct state construction, is exactly the kind of judgment call expected — do not silently work around it by e.g. changing `kRoundsPerMatch` or forcing a scoring path that doesn't exist.

Register `SmokeTestTeamScoring()` in `RunAllSmokeTests()`.

- [ ] **Step 8: Build and run**

Run: `mingw32-make server && ./bin/server.exe --test`

Expected: all smoke tests pass, including the corrected `SmokeTestTeamScoring`.

- [ ] **Step 9: Commit**

```bash
git add src/server/MatchState.h src/server/main.cpp src/shared/Protocol.h
git commit -m "Add team assignment and team-based scoring for 2v2 mode"
```

---

### Task 4: Revive-item spawn locations by mode

**Files:**
- Modify: `src/server/main.cpp`

**Interfaces:**
- Consumes: `GameMode` (Task 1), `courtBounds` (existing local).
- Produces: session-creation item-spawn logic branches by mode: `RevivePotionTest` and `FFA` keep the existing in-arena positions; `TwoVTwo` uses 4 new positions just outside `courtBounds`.

- [ ] **Step 1: Branch the item-spawn positions at session creation**

Find the session-creation block:

```cpp
                            if (sessionWorldItems.find(outcome.roomCode) == sessionWorldItems.end()) {
                                sessionWorldItems[outcome.roomCode] = {
                                    WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
                                    WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
                                };
```

replace with:

```cpp
                            if (sessionWorldItems.find(outcome.roomCode) == sessionWorldItems.end()) {
                                if (msg.requestedMode == GameMode::TwoVTwo) {
                                    // 2v2: revive items spawn OUTSIDE the court, so fetching one
                                    // costs a teammate real time away from the potato action.
                                    sessionWorldItems[outcome.roomCode] = {
                                        WorldItem{ Vector2{-50.0f, 300.0f}, ItemType::RevivePotion, true },  // just left of the court
                                        WorldItem{ Vector2{1050.0f, 300.0f}, ItemType::RevivePotion, true }, // just right of the court
                                    };
                                } else {
                                    // FFA and RevivePotionTest: original in-arena positions.
                                    sessionWorldItems[outcome.roomCode] = {
                                        WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
                                        WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
                                    };
                                }
```

(`courtBounds` is `{0, 0, 1000, 600}` per Phase 2 — `-50` and `1050` are 50px outside the left/right edges respectively, comfortably clear of the `1000`-wide court while still being reasonably close for a player to reach. Only 2 spawn points are used here, matching the existing 2-item pattern — the spec mentions "4 fixed spots, one per edge" as an option but this plan deliberately keeps the existing 2-item count for minimal change, placing both new points outside the court rather than expanding to 4; this is a scope-reduction decision consistent with "smaller, working increment over spec's full ambition," matching this project's established pattern of pragmatic scope choices during implementation.)

- [ ] **Step 2: Build**

Run: `mingw32-make server`

Expected: builds cleanly.

- [ ] **Step 3: Add a smoke test confirming 2v2's items spawn outside `courtBounds`**

This requires a way to trigger session creation with a specific mode from a test context. Since `sessionWorldItems`/`msg.requestedMode` branching lives inline in `main()`'s packet-handling code (not a standalone testable function), add a FOCUSED smoke test that exercises the underlying position logic directly rather than the full packet path — e.g., a small test asserting the literal coordinates `{-50, 300}` and `{1050, 300}` are indeed outside `courtBounds{0,0,1000,600}` (a trivial geometric assertion, but it pins the constants so a future edit can't accidentally move them back inside without a test noticing):

```cpp
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
```

(`PointInRect` is already defined in `src/shared/Geometry.h`, already included by `main.cpp`.)

Register in `RunAllSmokeTests()`.

- [ ] **Step 4: Build and run**

Run: `mingw32-make server && ./bin/server.exe --test`

Expected: all smoke tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/server/main.cpp
git commit -m "Spawn revive items outside the court for 2v2, keep in-arena positions for FFA/RevivePotionTest"
```

---

### Task 5: Client — mode-cycling create UI, per-mode HUD

**Files:**
- Modify: `src/client/RoomMenu.h`
- Modify: `src/client/NetClient.h`
- Modify: `src/client/NetClient.cpp`
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `GameMode` (Task 1), `WelcomeMsg::gameMode`, `MatchSnapshot::teamScore` (Task 3).
- Produces: `RoomMenu` gains a mode-cycling control; `NetClient::CreateRoom` threads the selected mode through; the client's HUD renders team scores for `TwoVTwo`, per-player scores for `FFA`, and no round/score HUD at all for `RevivePotionTest` (since it has no match/round concept); the potato and its countdown are not drawn in `RevivePotionTest`.

- [ ] **Step 1: Add mode-cycling state and UI to `RoomMenu`**

In `src/client/RoomMenu.h`, add a private member and cycling logic:

```cpp
private:
    // ... existing members ...
    GameMode selectedMode = GameMode::FFA;

    static const char* ModeLabel(GameMode mode) {
        switch (mode) {
            case GameMode::FFA: return "Mode: FFA (click to cycle)";
            case GameMode::TwoVTwo: return "Mode: 2v2 (click to cycle)";
            case GameMode::RevivePotionTest: return "Mode: Revive Test (click to cycle)";
            default: return "Mode: ?";
        }
    }

    static GameMode NextMode(GameMode mode) {
        switch (mode) {
            case GameMode::FFA: return GameMode::TwoVTwo;
            case GameMode::TwoVTwo: return GameMode::RevivePotionTest;
            case GameMode::RevivePotionTest: return GameMode::FFA;
            default: return GameMode::FFA;
        }
    }
```

Add a mode-cycling button/rectangle (e.g. above the existing `createButton`) — in `Draw()`:

```cpp
        Rectangle modeButton{ 400.0f, 140.0f, 200.0f, 30.0f };
        DrawRectangleRec(modeButton, LIGHTGRAY);
        DrawRectangleLinesEx(modeButton, 1, DARKGRAY);
        DrawText(ModeLabel(selectedMode), (int)modeButton.x + 5, (int)modeButton.y + 8, 12, BLACK);
```

(Adjust the existing `createButton`/`codeBox`/`joinButton` y-coordinates downward if this new button's placement at y=140 would overlap the "MAXION TEST" title at y=100 with its 32px font — read the CURRENT exact layout before finalizing; shift subsequent elements down by ~40-50px if needed to keep clear spacing.)

In `HandleInput`, add click handling for the mode button (store it as a member `Rectangle modeButton` rather than a local, so both `Draw()` and `HandleInput()` reference the same rectangle — read the current class structure to see whether `createButton`/`codeBox`/`joinButton` are members or locals and match that pattern):

```cpp
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), modeButton)) {
            selectedMode = NextMode(selectedMode);
        }
```

- [ ] **Step 2: Thread `selectedMode` through `CreateRoom`**

`NetClient::CreateRoom`'s current signature is `bool CreateRoom(const std::string& serverIp, uint16_t serverPort);`. Expand it:

In `src/client/NetClient.h`:

```cpp
    bool CreateRoom(const std::string& serverIp, uint16_t serverPort, GameMode mode);
```

In `src/client/NetClient.cpp`, find `CreateRoom`'s implementation (it constructs a `ConnectRequestMsg` with an empty `sessionName`) and add the mode field to that construction — read the current exact code (it likely builds the message inline or delegates to a shared connect-attempt helper; find where `ConnectRequestMsg`'s fields are actually set for a create request and add `req.requestedMode = mode;` there). `JoinRoom` does NOT need a mode parameter — per Global Constraints, mode is meaningless on a join.

In `RoomMenu::HandleInput`, update the create-button click handler to pass `selectedMode`:

```cpp
        if (createClicked) {
            errorMessage.clear();
            if (netClient.CreateRoom(serverIp, serverPort, selectedMode)) {
```

(Read the exact current `createClicked` handling block to integrate this correctly — don't guess at surrounding code.)

- [ ] **Step 3: Store and expose the session's actual `GameMode` on `NetClient`**

Add a getter and backing field to `NetClient` (mirroring how `gameConstants`/`GetGameConstants()` already work):

```cpp
    GameMode GetGameMode() const { return gameConstants.gameMode; }
```

(This works for free once `WelcomeMsg::gameMode` — Task 1 — is populated and `gameConstants` is assigned from the received `WelcomeMsg`, which `NetClient` already does for every other field.)

- [ ] **Step 4: Branch the client's HUD/rendering by mode**

In `src/client/main.cpp`, find the potato-drawing block (from Phase 2) and the match/score HUD block (from Phase 3). Wrap BOTH in a mode check:

```cpp
        GameMode myMode = netClient.GetGameMode();

        if (myMode != GameMode::RevivePotionTest) {
            // ... existing potato-drawing block, unchanged ...
        }
```

For the HUD, replace the existing per-player score loop's logic with a mode branch:

```cpp
        if (netClient.HasReceivedSnapshot()) {
            if (myMode == GameMode::RevivePotionTest) {
                // No match/round/score concept in this mode — nothing to show here.
            } else if (myMode == GameMode::TwoVTwo) {
                const MatchSnapshot& matchSnap = snap.match;
                if (matchSnap.matchOver) {
                    const char* winnerText = (matchSnap.winnerSlot == 0) ? "Match Over! Team A Wins!" :
                                             (matchSnap.winnerSlot == 1) ? "Match Over! Team B Wins!" : "Match Over! (no winner)";
                    DrawText(winnerText, 10, 70, 20, RED);
                } else {
                    const char* roundText = matchSnap.inTiebreak ? "Round: Tiebreak" : TextFormat("Round: %d / %d", matchSnap.roundNumber, kRoundsPerMatch);
                    DrawText(roundText, 10, 70, 16, BLACK);
                }
                DrawText(TextFormat("Team A: %d", matchSnap.teamScore[0]), 10, 90, 14, BLUE);
                DrawText(TextFormat("Team B: %d", matchSnap.teamScore[1]), 10, 108, 14, MAROON);
            } else {
                // FFA: existing per-player score HUD, unchanged from Phase 3.
                ... (existing FFA HUD code stays exactly as-is here) ...
            }
        }
```

Read the CURRENT exact HUD code (from Phase 3, Task 4) before restructuring it into this mode branch — preserve the FFA rendering exactly as it exists today inside the `else` branch, don't rewrite it.

- [ ] **Step 5: Build**

Run: `mingw32-make client`

Expected: builds cleanly.

- [ ] **Step 6: Add a client smoke test for the mode-cycling logic**

Extend `src/client/RoomMenu.h`'s existing testable-logic pattern (mirroring `AppendDigitInput`'s extraction for testability) — `NextMode`/`ModeLabel` are already `static` pure functions per Step 1, so they're directly testable. Add to the existing client test file (find whichever file currently holds `RunRoomMenuSmokeTests` — likely `RoomMenuTests.h`) a few assertions:

```cpp
    // Mode cycling: FFA -> 2v2 -> RevivePotionTest -> FFA (wraps around)
    assert(RoomMenu::NextMode(GameMode::FFA) == GameMode::TwoVTwo);
    assert(RoomMenu::NextMode(GameMode::TwoVTwo) == GameMode::RevivePotionTest);
    assert(RoomMenu::NextMode(GameMode::RevivePotionTest) == GameMode::FFA);
```

(Add this inside the existing `RunRoomMenuSmokeTests()` function, or as a new small function registered alongside it in `main.cpp`'s `--test` branch — your judgment on the cleanest fit given the current file structure.)

- [ ] **Step 7: Build and run**

Run: `mingw32-make client && ./bin/client.exe --test`

Expected: all client smoke tests pass, including the new mode-cycling assertions.

- [ ] **Step 8: Commit**

```bash
git add src/client/RoomMenu.h src/client/NetClient.h src/client/NetClient.cpp src/client/main.cpp src/client/RoomMenuTests.h
git commit -m "Add mode-cycling create UI and per-mode HUD rendering on the client"
```

---

## Final Verification

After all five tasks:

- [ ] Run `mingw32-make server && mingw32-make client` — both build cleanly.
- [ ] Run `./bin/server.exe --test` — all server smoke tests pass (existing Hot-Potato tests unaffected, plus the new `SmokeTestGameModes`, `SmokeTestTeamScoring`, `SmokeTestRevivalItemSpawnLocations`).
- [ ] Run `./bin/client.exe --test` — all client smoke tests pass, including the mode-cycling assertions.
- [ ] Live verification, FFA regression check: create an FFA room exactly as before, confirm the potato, charge/throw/catch, explosion, scoring, and match/tiebreak all behave IDENTICALLY to how they did before this phase (no potato-visual changes, no HUD changes for FFA specifically).
- [ ] Live verification, 2v2: create a 2v2 room, connect 4 clients. Confirm slots 0+1 are visually/functionally one team and slots 2+3 the other (no explicit color-coding requirement was specced, but confirm the HUD's "Team A"/"Team B" scores update correctly — whichever team ISN'T the round's cause should gain a point). Confirm revive items spawn visibly outside the arena rectangle. Play to a team win and confirm the "Team A/B Wins!" announcement appears.
- [ ] Live verification, Revive Test: create a Revive Test room, connect 2 clients. Confirm NO potato appears anywhere. Confirm standing in the hazard zone visibly damages HP over time (existing HP bar UI, if any, should tick down) and eventually Downs a player at 0 HP. Confirm a teammate can channel-revive them using the existing E-key/potion mechanic. Confirm that if the Downed timer expires with no revive, the player becomes Dead and then AUTO-RESPAWNS after the existing `kDeathRespawnDelay` — with NO manual "New Match"/reset action needed (unlike FFA/2v2).
- [ ] Confirm the mode-cycling button in `RoomMenu` visibly cycles through all 3 labels and that whichever mode is showing when "Create Room" is clicked is the mode the resulting session actually runs in (cross-check against server console output or by observing gameplay behavior once connected).
