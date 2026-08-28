# Hot Potato Phase 3: FFA Scoring, Best-of-3, Tiebreak Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn Phase 2's round-end mechanism (explosion/out-of-bounds downs a player) into an actual scored match: every active player except the one attributed as the round's cause scores +1; exactly 3 rounds are played; the player(s) with the highest score after 3 rounds win; if 2+ players tie for the highest score, a sudden-death tiebreak round (or rounds, repeated until broken) resolves it. Adds HUD elements: current round number, per-player scores, and a match-winner announcement.

**Architecture:** Phase 2's `ResetPotatoForNewRound` already fires at exactly the two real round-end moments (explosion-while-held, out-of-bounds-in-multiplayer) plus one non-round-end moment (holder-vanished reclaim, which must NOT score). This phase distinguishes those call sites by having each round-ending call site score BEFORE calling the reset helper (using the `session`/`active` state that's still valid pre-reset), while the reclaim call site scores nothing. A new per-session `MatchState` struct (round number, per-slot round score, match-over flag, winner slot(s)) is added alongside the existing `HotPotato`/charge-timer maps. `SnapshotMsg` gains match-state fields so the client can render them.

**Tech Stack:** C++17, existing UDP client/server protocol (additive fields), raylib/raylib-cpp (HUD text rendering only — no new input this phase).

**Spec:** [docs/superpowers/specs/2026-08-26-hot-potato-design.md](../specs/2026-08-26-hot-potato-design.md) (Phase 3 section — "FFA Scoring, Best-of-3, Tiebreak")

## Global Constraints

- Scoring rule: on a real round-end (explosion-while-held OR out-of-bounds-in-multiplayer), every ACTIVE player except the one attributed as the cause (the exploded-on holder, or the out-of-bounds last-thrower) gets `+1` to their round score. The holder-vanished reclaim (Phase 2's disconnect/downed-holder recovery mechanism) is NOT a round-end and must NEVER score.
- Match structure: exactly 3 rounds are always played (no early-exit at 2/3 like a traditional best-of-3 win-by-majority — the design explicitly calls for "exactly 3 rounds, highest score wins," per the spec). After round 3, compare scores: if exactly one player has the strict maximum, they win outright. If 2+ players are tied for the maximum, trigger sudden-death: play one more round using the exact same round-end/scoring rules, but only track/compare scores among the still-tied players (others continue to physically participate in the tiebreak round's simulation — thrown to, can catch, etc. — but their score changes are ignored for tiebreak-resolution purposes). Repeat additional tiebreak rounds until exactly one player among the originally-tied set has struck ahead of the others in that tiebreak's own score delta.
- "Round" boundary: a round begins when the potato (re)spawns held by a fresh holder (either at session creation, or via `ResetPotatoForNewRound` following a scored round-end) and ends at the next scored round-end.
- No player-count-based scoring exceptions — this applies uniformly for 1 (there's no one else to score, a round-end simply credits nobody and moves to the next round; solo mode's wall-bounce means round-ends mostly don't happen at all in true 1-player sessions, but the scoring path must not crash if it's ever reached with 0 other active players), 2, 3, or 4 active players.
- Protocol changes are additive only (new fields appended at the end of `SnapshotMsg`, consistent with the project's established discipline).
- No new client input this phase — purely HUD rendering additions.
- Builds with `mingw32-make server` / `mingw32-make client` using the existing Makefile.
- Toolchain: MinGW at `C:\msys64\mingw64\bin`, add to PATH if needed (bash: `export PATH="/c/msys64/mingw64/bin:$PATH"`). If a rebuild fails with a linker "Permission denied," a previous `server.exe`/`client.exe` may be running — check `tasklist //FI "IMAGENAME eq server.exe"` / `client.exe` and ASK before killing (don't kill without asking).

---

### Task 1: `MatchState` struct and pure scoring/tiebreak logic

**Files:**
- Create: `src/server/MatchState.h`

**Interfaces:**
- Produces: `struct MatchState` (fields: `int roundNumber` starting at 1, `int roundScore[kMaxPlayersPerSession]` all zero-initialized, `bool matchOver`, `int winnerSlot` (-1 if no single winner yet / tie unresolved, or a specific slot once decided), `bool inTiebreak`, `bool tiebreakEligible[kMaxPlayersPerSession]` (which slots are still contending in an active tiebreak)); free functions `void ScoreRoundEnd(MatchState& match, const bool* active, int excludedSlot)` (adds +1 to every active slot except `excludedSlot`, respecting `tiebreakEligible` gating when `inTiebreak` is true — see Step 2), `void AdvanceRoundOrEndMatch(MatchState& match, const bool* active)` (called after a round-end's scoring, before the next round starts: increments `roundNumber`; if `roundNumber` exceeds 3 and not already resolved, evaluates for a winner or tiebreak). Consumed by Task 3 (server tick integration).

This task is pure logic with no dependency on `Session`/networking, mirroring `HotPotato.h`'s structure.

- [ ] **Step 1: Write `MatchState.h` with the struct and constants**

```cpp
#pragma once

#include "../shared/Protocol.h"

struct MatchState {
    int roundNumber = 1; // 1-indexed; 1..kRoundsPerMatch during normal play, stays at kRoundsPerMatch+something conceptually once in tiebreak (tiebreak rounds don't increment past the cap the same way — see AdvanceRoundOrEndMatch)
    int roundScore[kMaxPlayersPerSession] = {0, 0, 0, 0};
    bool matchOver = false;
    int winnerSlot = -1; // -1 until matchOver is true and exactly one winner is determined
    bool inTiebreak = false;
    bool tiebreakEligible[kMaxPlayersPerSession] = {false, false, false, false}; // which slots are still contending in the current tiebreak; irrelevant when inTiebreak is false
};
```

- [ ] **Step 2: Write `ScoreRoundEnd`**

Append to `MatchState.h`:

```cpp
// Credits every active player except `excludedSlot` (the round-end's cause: the exploded
// holder, or the out-of-bounds last-thrower) with +1 round score. If a tiebreak is active,
// only slots marked `tiebreakEligible` are credited or excluded from — non-eligible slots
// (already-eliminated from contention, or never-tied players) never have their score
// touched during a tiebreak, since tiebreak scoring only needs to resolve who among the
// originally-tied set pulls ahead first.
inline void ScoreRoundEnd(MatchState& match, const bool* active, int excludedSlot) {
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (!active[i] || i == excludedSlot) continue;
        if (match.inTiebreak && !match.tiebreakEligible[i]) continue;
        match.roundScore[i] += 1;
    }
}
```

- [ ] **Step 3: Write `AdvanceRoundOrEndMatch`**

Append to `MatchState.h`:

```cpp
// Called once per round-end, AFTER ScoreRoundEnd, BEFORE the next round's potato spawns.
// Advances the round counter and, once kRoundsPerMatch normal rounds are complete (or a
// tiebreak round has just resolved), determines whether the match is over.
inline void AdvanceRoundOrEndMatch(MatchState& match, const bool* active) {
    if (match.matchOver) return; // no-op once decided; guards against a stray extra call

    if (match.inTiebreak) {
        // Resolve the tiebreak: among tiebreakEligible slots, find the strict max of
        // roundScore. If exactly one holds it, the match ends. If 2+ still tie, stay in
        // tiebreak, narrow tiebreakEligible to just the still-tied slots, and play another
        // tiebreak round (roundNumber is not meaningfully incremented further during
        // tiebreak rounds; kept fixed for HUD display purposes at kRoundsPerMatch + 1).
        int maxScore = -1;
        int maxCount = 0;
        int maxSlot = -1;
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i] || !match.tiebreakEligible[i]) continue;
            if (match.roundScore[i] > maxScore) {
                maxScore = match.roundScore[i];
                maxCount = 1;
                maxSlot = i;
            } else if (match.roundScore[i] == maxScore) {
                maxCount++;
            }
        }
        if (maxCount == 1) {
            match.matchOver = true;
            match.winnerSlot = maxSlot;
        } else {
            // Still tied among 2+: narrow eligibility to just the tied slots and continue.
            for (int i = 0; i < kMaxPlayersPerSession; i++) {
                match.tiebreakEligible[i] = active[i] && match.roundScore[i] == maxScore;
            }
        }
        return;
    }

    match.roundNumber += 1;
    if (match.roundNumber > kRoundsPerMatch) {
        // Normal rounds complete: find the strict max among all active players.
        int maxScore = -1;
        int maxCount = 0;
        int maxSlot = -1;
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i]) continue;
            if (match.roundScore[i] > maxScore) {
                maxScore = match.roundScore[i];
                maxCount = 1;
                maxSlot = i;
            } else if (match.roundScore[i] == maxScore) {
                maxCount++;
            }
        }
        if (maxCount == 1) {
            match.matchOver = true;
            match.winnerSlot = maxSlot;
        } else if (maxCount >= 2) {
            match.inTiebreak = true;
            for (int i = 0; i < kMaxPlayersPerSession; i++) {
                match.tiebreakEligible[i] = active[i] && match.roundScore[i] == maxScore;
            }
        }
        // maxCount == 0 (no active players at all): match simply doesn't resolve; leave
        // matchOver false. This is an edge case with no active players to declare a winner
        // among — not expected in practice but must not crash.
    }
}
```

- [ ] **Step 4: Build to confirm no compile error**

Run: nothing includes this header yet (same situation as `HotPotato.h` in Phase 2's Task 1) — skip building; Task 3's build step will catch any syntax error when this header is first included.

- [ ] **Step 5: Commit**

```bash
git add src/server/MatchState.h
git commit -m "Add MatchState struct and pure scoring/round-advance/tiebreak logic"
```

---

### Task 2: Protocol additions — match state fields in the snapshot

**Files:**
- Modify: `src/shared/Protocol.h`

**Interfaces:**
- Produces: new `struct MatchSnapshot` with `int roundNumber; int roundScore[kMaxPlayersPerSession]; bool matchOver; int winnerSlot; bool inTiebreak;`. `SnapshotMsg` gains `MatchSnapshot match;` appended at the end. Consumed by Task 3 (server population), Task 4 (client rendering).

- [ ] **Step 1: Add `kRoundsPerMatch`, `MatchSnapshot`, and append it to `SnapshotMsg`**

In `src/shared/Protocol.h`, add a new shared constant alongside `kMaxPlayersPerSession` (both client and server need to agree on match length — the client for HUD display, the server for `AdvanceRoundOrEndMatch`'s logic):

```cpp
// Number of rounds in a normal match before tiebreak resolution kicks in (Phase 3).
constexpr int kRoundsPerMatch = 3;
```

Then add this new struct just before `SnapshotMsg`:

```cpp
struct MatchSnapshot {
    int roundNumber;
    int roundScore[kMaxPlayersPerSession];
    bool matchOver;
    int winnerSlot; // -1 if undecided
    bool inTiebreak;
};
```

Then modify `SnapshotMsg` — replace:

```cpp
struct SnapshotMsg {
    PlayerSnapshot players[kMaxPlayersPerSession];
    // Intentionally 2, not kMaxPlayersPerSession: the world-item count is unrelated to
    // player count (a later phase reworks item spawning). Do not "fix" this to match above.
    WorldItemSnapshot items[2];
    float hazardX;
    float hazardY;
    float hazardW;
    float hazardH;
    PotatoSnapshot potato;
};
```

with:

```cpp
struct SnapshotMsg {
    PlayerSnapshot players[kMaxPlayersPerSession];
    // Intentionally 2, not kMaxPlayersPerSession: the world-item count is unrelated to
    // player count (a later phase reworks item spawning). Do not "fix" this to match above.
    WorldItemSnapshot items[2];
    float hazardX;
    float hazardY;
    float hazardW;
    float hazardH;
    PotatoSnapshot potato;
    MatchSnapshot match;
};
```

- [ ] **Step 2: Build both targets to confirm no compile breakage**

Run: `mingw32-make server && mingw32-make client`

Expected: this task alone will likely fail to fully build, since `src/server/main.cpp`'s existing `SnapshotMsg` construction site doesn't yet populate `snap.match` — a build error here is only a problem if it's INSIDE `Protocol.h` itself; a missing-field-initializer situation in an aggregate-init elsewhere is not expected to be a hard compile error in C++ (unset trailing struct members simply zero-initialize), so this task may in fact build completely cleanly on its own, similar to Phase 2 Task 2's outcome. If it does build cleanly, that's fine — just note it in your report; it's not a sign you missed something.

- [ ] **Step 3: Commit**

```bash
git add src/shared/Protocol.h
git commit -m "Add MatchSnapshot fields to the wire protocol"
```

---

### Task 3: Server-side scoring integration — score on round-end, advance rounds, resolve matches

**Files:**
- Modify: `src/server/main.cpp`

**Interfaces:**
- Consumes: `MatchState`, `ScoreRoundEnd`, `AdvanceRoundOrEndMatch` (Task 1); `MatchSnapshot` (Task 2).
- Produces: per-session `MatchState` tracked alongside `HotPotato`; `SimulateSessionTick`'s signature grows to accept `MatchState&`; the two REAL round-end call sites (explosion-while-held, out-of-bounds-multiplayer) call `ScoreRoundEnd` then `AdvanceRoundOrEndMatch` BEFORE calling `ResetPotatoForNewRound`; the holder-vanished reclaim call site is NOT touched (no scoring). Once `match.matchOver` is true, the potato simulation effectively pauses (round-ends stop resetting the potato into a new round) until a NEW MATCH is started (see Step 4 for how a new match begins).

Read the CURRENT state of `src/server/main.cpp` yourself before editing — locate the exact two scoring call sites (they call `ResetPotatoForNewRound` immediately after `ForceDown()`, one in the out-of-bounds-multiplayer branch, one in the explosion branch) and the ONE non-scoring call site (the holder-vanished reclaim, which calls `ResetPotatoForNewRound` directly with no preceding `ForceDown()` in that exact spot — it's syntactically distinguishable from the other two by NOT following a `ForceDown()` call in the immediately preceding line).

- [ ] **Step 1: Add per-session `MatchState` tracking**

In `src/server/main.cpp`, add `#include "MatchState.h"` alongside the other includes.

In `main()`, add a new per-session map alongside `sessionPotato`/`sessionChargeTimer`:

```cpp
    std::map<std::string, MatchState> sessionMatch;
```

In the session-creation block (where `sessionPotato[outcome.roomCode] = freshPotato;` is set), initialize a fresh `MatchState`:

```cpp
                                sessionPotato[outcome.roomCode] = freshPotato;
                                sessionMatch[outcome.roomCode] = MatchState{};
```

- [ ] **Step 2: Grow `SimulateSessionTick`'s signature to accept `MatchState&`**

Change the forward declaration and definition of `SimulateSessionTick` from:

```cpp
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds);
```

to:

```cpp
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds, MatchState& match);
```

- [ ] **Step 3: Wire scoring into the two real round-end call sites, and gate the potato simulation once the match is over**

At the START of the potato-simulation section inside `SimulateSessionTick` (right after `soloMode` is computed, before the charge-tracking loop), add a match-over guard so no further potato activity (charging, throwing, catching, exploding) happens once the match has concluded:

```cpp
    if (match.matchOver) {
        // Match decided: freeze the potato in whatever state it's in (typically unheld,
        // since the winning round-end already reset it) — no further charge/throw/catch/
        // explosion simulation runs. A new match starts only via an explicit reset (Task 4
        // adds a debug/UI trigger for this in a later step; for this task, matches simply
        // end and stay ended).
    } else {
        // ... all existing potato-simulation logic (charge tracking through explosion) goes here, indented one level deeper ...
    }
```

Read the current code carefully: this requires wrapping the ENTIRE existing potato-simulation block (from the charge-tracking loop through the explosion-timer block, i.e. everything between where `soloMode` is computed and the "Held potato tracks its holder's position" block) in an `if (!match.matchOver) { ... }` guard — do NOT wrap the "Held potato tracks its holder's position" tracking block itself, since that's a harmless position-sync that should keep running even if the match just ended (so the client sees the potato at a sensible last position, not frozen at (0,0) or similar).

Within that guarded block, find the out-of-bounds-multiplayer round-end site:

```cpp
                } else if (potato.lastThrowerSlot != -1 && active[potato.lastThrowerSlot]) {
                    // Multiplayer: leaving the court downs the last thrower and starts a new round.
                    session.slots[potato.lastThrowerSlot].player.ForceDown();
                    // Respawn held by the first Alive active player (deterministic slot
                    // order), via the shared ResetPotatoForNewRound helper.
                    ResetPotatoForNewRound(potato, session, active, chargeTimer);
                }
```

replace with:

```cpp
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
```

Find the explosion round-end site:

```cpp
    // Explosion: timer expires while held.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot]) {
        potato.explodeTimer -= dt;
        if (potato.explodeTimer <= 0.0f) {
            session.slots[potato.holderSlot].player.ForceDown();
            ResetPotatoForNewRound(potato, session, active, chargeTimer);
        }
    }
```

replace with:

```cpp
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
```

(Note: `exploderSlot` is captured into a local BEFORE `ForceDown()`/`ScoreRoundEnd`/`ResetPotatoForNewRound` run, since `ResetPotatoForNewRound` mutates `potato.holderSlot` — using `potato.holderSlot` directly after that call would read the NEW holder, not the exploded one. Verify this ordering concern doesn't ALSO apply to the out-of-bounds site: there, `potato.lastThrowerSlot` is read for `ScoreRoundEnd`'s `excludedSlot` argument BEFORE `ResetPotatoForNewRound` runs in the code above, which is correct since the statements execute in the written order — but confirm `ResetPotatoForNewRound` doesn't ALSO clear `lastThrowerSlot` in a way that would matter if the ordering were ever reversed. It's fine as written above since `ScoreRoundEnd`/`AdvanceRoundOrEndMatch` both run before `ResetPotatoForNewRound` in both replacement blocks — just be precise about this ordering when implementing, don't reorder these three calls.)

Do NOT modify the holder-vanished reclaim call site (`if (potato.held && (potato.holderSlot < 0 || ...)) { ResetPotatoForNewRound(...); }`) — leave it exactly as-is, with no `ScoreRoundEnd`/`AdvanceRoundOrEndMatch` calls, since a disconnect/downed-holder reclaim is not a scored round-end.

- [ ] **Step 4: Update `main()`'s tick-loop call site and populate `snap.match`**

Find the tick-loop call site:

```cpp
                bool active[kMaxPlayersPerSession];
                HotPotato& potato = sessionPotato[sessionEntry];
                float* chargeTimer = sessionChargeTimer[sessionEntry];
                InputMsg* latestInputs = sessionLatestInput.count(sessionEntry) ? sessionLatestInput[sessionEntry] : nullptr;
                if (!latestInputs) continue;
                SimulateSessionTick(*session, items, hazard, hazardCarry, attack, interact, dt, active, potato, chargeTimer, latestInputs, courtBounds);
```

replace with:

```cpp
                bool active[kMaxPlayersPerSession];
                HotPotato& potato = sessionPotato[sessionEntry];
                float* chargeTimer = sessionChargeTimer[sessionEntry];
                InputMsg* latestInputs = sessionLatestInput.count(sessionEntry) ? sessionLatestInput[sessionEntry] : nullptr;
                if (!latestInputs) continue;
                MatchState& match = sessionMatch[sessionEntry];
                SimulateSessionTick(*session, items, hazard, hazardCarry, attack, interact, dt, active, potato, chargeTimer, latestInputs, courtBounds, match);
```

Find the snapshot-building block (`snap.potato = PotatoSnapshot{ ... };`) and add immediately after it:

```cpp
                MatchSnapshot matchSnap{};
                matchSnap.roundNumber = match.roundNumber;
                for (int i = 0; i < kMaxPlayersPerSession; i++) matchSnap.roundScore[i] = match.roundScore[i];
                matchSnap.matchOver = match.matchOver;
                matchSnap.winnerSlot = match.winnerSlot;
                matchSnap.inTiebreak = match.inTiebreak;
                snap.match = matchSnap;
```

- [ ] **Step 5: Build**

Run: `mingw32-make server`

Expected: builds cleanly.

- [ ] **Step 6: Add smoke tests for scoring/round-advance/tiebreak logic**

Add a new smoke test function `SmokeTestMatchState()` in `src/server/main.cpp` (following the existing `SmokeTest*` pattern), covering, at minimum:

```cpp
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
```

Register `SmokeTestMatchState()` in `RunAllSmokeTests()` following the existing pattern.

Also extend `SmokeTestHotPotato()`'s existing multi-round integration test (from Phase 2) OR add a new focused integration test verifying that a real explosion round-end via `SimulateSessionTick` actually increments `match.roundScore` for the non-exploded active players and calls `AdvanceRoundOrEndMatch` correctly (i.e., confirm the wiring in Step 3 is actually invoked end-to-end, not just that the pure `MatchState.h` functions work in isolation). Use your judgment on whether to extend the existing test or add a new one — a new one focused specifically on "one explosion round-end via SimulateSessionTick scores correctly" is likely cleaner than entangling it with Phase 2's existing tests.

- [ ] **Step 7: Build and run**

Run: `mingw32-make server && ./bin/server.exe --test`

Expected: all smoke tests (existing + new `SmokeTestMatchState` + the new integration test) pass with pristine output.

- [ ] **Step 8: Commit**

```bash
git add src/server/main.cpp
git commit -m "Wire round-end scoring, round advancement, and tiebreak resolution into the server tick"
```

---

### Task 4: Client HUD — round number, per-player scores, match-winner announcement

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `SnapshotMsg::match` (Task 2), server-computed `MatchState` data (Task 3).

- [ ] **Step 1: Add match-state HUD rendering**

In `src/client/main.cpp`, find the existing HUD text block (outside `BeginMode2D`/`EndMode2D`, where `"Room: %s | Connected"` and the slot/controls text are drawn) and add, after the existing lines:

```cpp
        {
            const MatchSnapshot& matchSnap = snap.match;
            if (netClient.HasReceivedSnapshot()) {
                if (matchSnap.matchOver) {
                    const char* winnerText = (matchSnap.winnerSlot >= 0 && matchSnap.winnerSlot < kMaxPlayersPerSession)
                        ? TextFormat("Match Over! Winner: P%d", matchSnap.winnerSlot + 1)
                        : "Match Over! (no winner)";
                    DrawText(winnerText, 10, 70, 20, RED);
                } else {
                    const char* roundText = matchSnap.inTiebreak
                        ? "Round: Tiebreak"
                        : TextFormat("Round: %d / %d", matchSnap.roundNumber, kRoundsPerMatch);
                    DrawText(roundText, 10, 70, 16, BLACK);
                }

                int scoreY = 90;
                for (int i = 0; i < kMaxPlayersPerSession; i++) {
                    if (snap.players[i].state == 3) continue; // absent slot, skip
                    DrawText(TextFormat("P%d: %d", i + 1, matchSnap.roundScore[i]), 10, scoreY, 14, kPlayerColors[i]);
                    scoreY += 18;
                }
            }
        }
```

`kRoundsPerMatch` is already defined in `src/shared/Protocol.h` (Task 2), which `main.cpp` already includes transitively via `NetClient.h` — no new include needed, and no server-only header is pulled into the client.

- [ ] **Step 2: Build**

Run: `mingw32-make client`

Expected: builds cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/client/main.cpp
git commit -m "Add client HUD for round number, per-player scores, and match-winner announcement"
```

---

## Final Verification

After all four tasks:

- [ ] Run `mingw32-make server && mingw32-make client` — both build cleanly.
- [ ] Run `./bin/server.exe --test` — all server smoke tests (including `SmokeTestMatchState` and the new scoring-integration test) pass.
- [ ] Run `./bin/client.exe --test` — all client smoke tests still pass (unaffected by this phase's changes, but confirm no regression).
- [ ] Live verification with 2 clients: connect two players, play through explosions/out-of-bounds events, confirm the HUD's round number increments, per-player scores update correctly (everyone but the round's cause gains a point), and after exactly 3 rounds the match ends with a "Match Over! Winner: P_" announcement matching whoever has the higher score.
- [ ] Live verification, tiebreak: deliberately engineer a tied outcome across 3 rounds (e.g. alternate which player is the round's cause so both end up with equal scores) and confirm a tiebreak round triggers (HUD shows "Round: Tiebreak"), and that the FIRST subsequent round-end among the tied players correctly ends the match with a single winner.
- [ ] Live verification with 3-4 clients: confirm scoring correctly credits ALL non-cause active players (not just one), and that an inactive/disconnected slot is correctly skipped both in scoring and in the HUD's score list.
- [ ] Confirm the holder-vanished reclaim path (a player disconnecting mid-hold, from Phase 2) still does NOT score a round — verify no score changes when this specific recovery path fires, distinguishing it from a genuine round-end.
