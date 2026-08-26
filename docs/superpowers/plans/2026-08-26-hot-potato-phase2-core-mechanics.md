# Hot Potato Phase 2: Core Mechanics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the core Hot Potato gameplay loop: a server-authoritative potato object that players charge-and-release throw with mouse-aimed direction, that flies, decelerates, and is auto-caught by proximity; an escalating explosion timer that shrinks on each catch; arena out-of-bounds detection (multiplayer: downs the last thrower; solo: bounces off walls); and round-end handling (instant-Down on explosion/out-of-bounds, brief pause, next-round potato respawn). This phase does NOT include scoring/match structure (Phase 3), dash (Phase 4), or 2v2/revive rework (Phase 5) — those build on top of what this phase delivers.

**Architecture:** A new `HotPotato` struct (server-only, one per session) holds flight/charge/timer state, updated each tick by a new `UpdateHotPotato` function called from `SimulateSessionTick`. `InputMsg` gains charge/release/aim fields; the client sends these based on mouse hold-and-release plus cursor direction. A new `PotatoSnapshot` struct is added to `SnapshotMsg` so the client can render the potato (position, held/in-flight state, explosion timer for the HUD-over-head display). This phase is gated behind existing session state — for now, EVERY session runs Hot Potato rules (no mode selection yet; Phase 5's spec-level "GameMode" flag and mode selection UI is out of this phase's scope, matching the design doc's phased build order). The existing melee-attack (`Q`/`attackPressed`) and hazard-zone logic are DISABLED for this phase's sessions per the design spec (Hot Potato explicitly removes them), but the underlying `Combat.h`/`Hazard.h` code is not deleted — it's simply not invoked from the Hot Potato tick path, since Phase 5 (2v2) still needs revive/PvP-adjacent mechanics layered back in later and Classic-mode compatibility isn't a concern once every session is Hot Potato (per this phase's scope, this fully replaces the prior gameplay — see Global Constraints for the precise boundary).

**Tech Stack:** C++17, existing UDP client/server protocol (additive fields), raylib/raylib-cpp (mouse input, cursor position, basic 2D drawing for the potato and its timer).

**Spec:** [docs/superpowers/specs/2026-08-26-hot-potato-design.md](../specs/2026-08-26-hot-potato-design.md) (Phase 2 section — "Core Hot Potato Mechanics")

## Global Constraints

- This phase REPLACES the existing melee-attack and hazard-zone gameplay for every session (no mode flag yet — that's Phase 5's concern per the design doc's stated build order). Concretely: `SimulateSessionTick` stops calling the attack loop and `ApplyHazardDamage`; the server stops constructing/broadcasting a `HazardZone`. The existing `Combat.h` (`TryAttack`, `UpdateAttackCooldown`), `Hazard.h`, `DebugActions.h`'s attack-adjacent actions, and all their smoke tests remain in the codebase UNCHANGED and UNDELETED — they're simply not called from the Hot Potato tick. Do not delete these files or their tests.
- The existing revive/Downed/Dead state machine (`Player.h`) is REUSED as-is for this phase's "explosion downs the holder" mechanic (via `Player::ForceDown()`, already existing, already correctly guarding `state != Alive`). Full revive-item mechanics (item spawn locations, 2v2 teammate-revive) are Phase 5's concern; this phase only needs `ForceDown()` to work, which it already does.
- No scoring, no match structure, no round-win/loss attribution UI yet — this phase only needs a round-END EVENT (something happened: explosion or out-of-bounds) to trigger `ForceDown()` on the right player and reset the potato for the next round. Phase 3 consumes round-end events for scoring; this phase just needs the mechanism to fire correctly and be inspectable (e.g. via the potato's own state), not to actually score anything yet.
- Arena bounds: a server-side `Rectangle courtBounds` matching the client's visible arena (`{0, 0, 1000, 600}` per the client's window size, with a small margin — exact values in Task 2).
- Solo mode (exactly 1 active player in a session) uses wall-bounce reflection instead of the out-of-bounds/down rule — determined each tick by counting active slots.
- No true 3D/Z-axis — the potato's "arc" is entirely 2D: charge duration maps to launch speed, drag decelerates it, catching is proximity-based. No height/shadow rendering required this phase (a flat 2D dot is sufficient; visual polish for the potato itself is not in scope here).
- Protocol changes are additive only (new `InputMsg` fields appended at the end, new `PotatoSnapshot` struct added, `SnapshotMsg` gains one new field appended at the end) — consistent with the project's established wire-compatibility discipline.
- Builds with `mingw32-make server` / `mingw32-make client` using the existing Makefile.
- Toolchain: MinGW at `C:\msys64\mingw64\bin`, add to PATH if needed (bash: `export PATH="/c/msys64/mingw64/bin:$PATH"`). If a rebuild fails with a linker "Permission denied," a previous `server.exe`/`client.exe` may be running — check `tasklist //FI "IMAGENAME eq server.exe"` / `client.exe` and ASK before killing (don't kill without asking).

---

### Task 1: `HotPotato` struct and pure charge/force/catch/timer logic

**Files:**
- Create: `src/server/HotPotato.h`

**Interfaces:**
- Produces: `struct HotPotato` (fields: `inFlight`, `held`, `holderSlot`, `lastThrowerSlot`, `position`, `velocity`, `explodeTimer`, `catchCount`), constants (`kPotatoStartTimer = 10.0f`, `kPotatoTimerShrink = 2.0f`, `kPotatoTimerFloor = 3.0f`, `kMaxChargeDuration = 1.5f`, `kMinThrowForce = 150.0f`, `kMaxThrowForce = 500.0f`, `kCatchRadius = 20.0f`, `kPotatoDragPerSecond = 0.6f`, `kLandingSpeedThreshold = 15.0f`), and free functions: `float ComputeThrowForce(float chargeDuration)`, `void ApplyPotatoDrag(HotPotato& potato, float dt)`, `bool TryCatchPotato(HotPotato& potato, const Vector2* activePositions, const bool* active, int throwerSlotToExclude, float dt)` (or similar — exact signature refined below), `float ComputeExplodeTimerForCatch(int catchCount)`. Consumed by Task 2 (`UpdateHotPotato`, the tick-integration function).

This task is pure logic with no dependency on `Session`/networking — it's the mathematically-testable core, mirroring how `Combat.h`/`Hazard.h` are structured (free functions operating on plain data, easily smoke-tested).

- [ ] **Step 1: Write `HotPotato.h` with the struct, constants, and pure helper functions**

```cpp
#pragma once

#include "../shared/Geometry.h"
#include <cmath>

constexpr float kPotatoStartTimer = 10.0f;
constexpr float kPotatoTimerShrink = 2.0f;
constexpr float kPotatoTimerFloor = 3.0f;
constexpr float kMaxChargeDuration = 1.5f;
constexpr float kMinThrowForce = 150.0f;
constexpr float kMaxThrowForce = 500.0f;
constexpr float kCatchRadius = 20.0f;
constexpr float kPotatoDragPerSecond = 0.6f; // fraction of speed removed per second (exponential decay)
constexpr float kLandingSpeedThreshold = 15.0f; // below this speed, treat the potato as "landed" for catch purposes (still catchable, just not visibly flying)

struct HotPotato {
    bool inFlight = false;
    bool held = false;
    int holderSlot = -1;
    int lastThrowerSlot = -1;
    Vector2 position{0.0f, 0.0f};
    Vector2 velocity{0.0f, 0.0f};
    float explodeTimer = 0.0f;
    int catchCount = 0;
};

// Force scales linearly from kMinThrowForce (no charge) to kMaxThrowForce (full charge),
// clamped so overcharging (chargeDuration > kMaxChargeDuration) doesn't exceed max force.
inline float ComputeThrowForce(float chargeDuration) {
    float clamped = chargeDuration;
    if (clamped < 0.0f) clamped = 0.0f;
    if (clamped > kMaxChargeDuration) clamped = kMaxChargeDuration;
    float t = clamped / kMaxChargeDuration;
    return kMinThrowForce + t * (kMaxThrowForce - kMinThrowForce);
}

// Exponential drag: velocity shrinks by a fixed fraction per second, framerate-independent
// via the standard 1 - exp(-k*dt) formulation.
inline void ApplyPotatoDrag(HotPotato& potato, float dt) {
    float decay = 1.0f - std::exp(-kPotatoDragPerSecond * dt);
    potato.velocity.x -= potato.velocity.x * decay;
    potato.velocity.y -= potato.velocity.y * decay;
}

// The explosion timer for a given catch count: starts at kPotatoStartTimer, shrinks by
// kPotatoTimerShrink per catch, floored at kPotatoTimerFloor. catchCount=0 is the very
// first hold of a fresh round (full duration); each successful catch after that shrinks it.
inline float ComputeExplodeTimerForCatch(int catchCount) {
    float timer = kPotatoStartTimer - (float)catchCount * kPotatoTimerShrink;
    if (timer < kPotatoTimerFloor) timer = kPotatoTimerFloor;
    return timer;
}
```

- [ ] **Step 2: Add a smoke-testable free function for catch detection**

Append to `HotPotato.h`:

```cpp
inline float DistanceBetween2(Vector2 a, Vector2 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Returns the slot index of the first active, non-excluded player within kCatchRadius of
// the potato's current position, or -1 if none. `excludeSlot` prevents an instantaneous
// self-catch check on the exact tick of release (pass the thrower's slot on the release
// tick only; pass -1 on subsequent ticks so the thrower CAN catch their own throw later,
// e.g. after it bounces back in solo mode).
inline int FindCatchTarget(Vector2 potatoPos, const Vector2* positions, const bool* active, int playerCount, int excludeSlot) {
    for (int i = 0; i < playerCount; i++) {
        if (!active[i] || i == excludeSlot) continue;
        if (DistanceBetween2(potatoPos, positions[i]) <= kCatchRadius) {
            return i;
        }
    }
    return -1;
}
```

- [ ] **Step 3: Build to confirm no compile error**

Run: `mingw32-make bin/server/main.o` is not yet applicable since nothing includes this header yet — instead, do a standalone syntax check: create no test file yet (Task 2 will integrate and test this). Skip building for this task; Task 2's build step will catch any syntax error in this header when it's first included. (This deviates from the usual "build after every task" pattern because this header has zero external dependents until Task 2 — that's fine, not a shortcut, since a header-only file with no includers can't meaningfully "build" on its own beyond a syntax parse, which the next task's build will perform.)

- [ ] **Step 4: Commit**

```bash
git add src/server/HotPotato.h
git commit -m "Add HotPotato struct and pure charge/force/drag/catch/timer logic"
```

---

### Task 2: Protocol additions — charge/aim input fields and potato snapshot

**Files:**
- Modify: `src/shared/Protocol.h`

**Interfaces:**
- Produces: `InputMsg` gains `bool chargingThrow`, `bool releaseThrow`, `float aimDirX`, `float aimDirY` (appended at the end). New `struct PotatoSnapshot` with `float posX, posY; bool held; bool inFlight; int holderSlot; float explodeTimer;`. `SnapshotMsg` gains `PotatoSnapshot potato;` (appended at the end). Consumed by Task 3 (server tick integration), Task 5 (client input), Task 6 (client rendering).

- [ ] **Step 1: Add charge/aim fields to `InputMsg`**

In `src/shared/Protocol.h`, replace:

```cpp
struct InputMsg {
    float moveX;
    float moveY;
    bool interactHeld;
    bool attackPressed;
};
```

with:

```cpp
struct InputMsg {
    float moveX;
    float moveY;
    bool interactHeld;
    bool attackPressed;
    bool chargingThrow;
    bool releaseThrow;
    float aimDirX;
    float aimDirY;
};
```

(`attackPressed` is intentionally left in place, unused by Hot Potato's tick per the Global Constraints, but not removed — it's still valid wire-protocol history and removing a field would be a non-additive, disruptive change for no benefit this phase.)

- [ ] **Step 2: Add `PotatoSnapshot` and append it to `SnapshotMsg`**

In `src/shared/Protocol.h`, add this new struct just before `SnapshotMsg`:

```cpp
struct PotatoSnapshot {
    float posX;
    float posY;
    bool held;
    bool inFlight;
    int holderSlot; // -1 if unheld/mid-flight
    float explodeTimer;
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
};
```

(The `hazardX/Y/W/H` fields stay in the struct even though Hot Potato disables the hazard zone this phase — per Global Constraints, `Hazard.h` isn't deleted, and removing these fields would be non-additive. The server will simply populate them with the zone's last/default position or zeros; exact behavior specified in Task 3.)

- [ ] **Step 3: Build both targets to confirm no compile breakage**

Run: `mingw32-make server && mingw32-make client`

Expected: this task alone will likely fail to fully build, since `src/server/main.cpp`'s existing `InputMsg`/`SnapshotMsg` construction sites (aggregate initializers) don't yet account for the new trailing fields — a build error here referencing `main.cpp`'s aggregate-init lines (not `Protocol.h` itself) is EXPECTED, since Task 3 fixes the server-side construction and Task 5/6 fix the client-side. If the error is inside `Protocol.h` itself, that's this task's problem.

- [ ] **Step 4: Commit**

```bash
git add src/shared/Protocol.h
git commit -m "Add charge/aim input fields and PotatoSnapshot to the wire protocol"
```

---

### Task 3: Server-side potato simulation — charge tracking, throw, flight, catch, explosion, round reset

**Files:**
- Modify: `src/server/main.cpp`

**Interfaces:**
- Consumes: `HotPotato`, `ComputeThrowForce`, `ApplyPotatoDrag`, `ComputeExplodeTimerForCatch`, `FindCatchTarget` (Task 1); `InputMsg`'s new fields, `PotatoSnapshot` (Task 2).
- Produces: `UpdateHotPotato(...)` function integrated into `SimulateSessionTick`; per-session `HotPotato` state tracked alongside the existing `sessionWorldItems`/`sessionHazardCarry` maps; server disables melee-attack and hazard-zone simulation for this phase.

This is the largest task in this phase — read the CURRENT state of `src/server/main.cpp` yourself before editing (it has evolved significantly since Phase 1; in particular `SimulateSessionTick` is now a standalone function, not inline in `main()`).

- [ ] **Step 1: Add per-session `HotPotato` and per-player charge-timer tracking**

In `src/server/main.cpp`, add `#include "HotPotato.h"` alongside the other includes at the top.

Add a new per-session map alongside the existing ones (find the block declaring `sessionWorldItems`/`sessionHazardCarry`/etc. inside `main()`, and also mirror this in the smoke-test harness if `SimulateSessionTick`'s signature needs a `HotPotato&` parameter — see Step 2):

```cpp
    std::map<std::string, HotPotato> sessionPotato;
    std::map<std::string, float[kMaxPlayersPerSession]> sessionChargeTimer;
```

(`sessionChargeTimer` tracks each player's current charge-hold duration, server-authoritative per the design spec — the client only reports "charging: yes/no" and a release event, never a force value directly.)

- [ ] **Step 2: Add `UpdateHotPotato` and integrate it into `SimulateSessionTick`**

`SimulateSessionTick`'s signature must grow to accept the potato state, charge timers, per-player latest input (for the charge/release/aim fields), and the arena bounds. Change its signature (both the forward declaration near the top of the file and the definition) from:

```cpp
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, float dt,
                                bool* activeOut);
```

to:

```cpp
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds);
```

(`latestInputs` is an `InputMsg*` array of size `kMaxPlayersPerSession` — the most recently received input per slot, needed to read `chargingThrow`/`releaseThrow`/`aimDirX`/`aimDirY`. The server main loop already stores per-player pending state in maps like `pendingAttack`/`pendingInteract`; this task adds a parallel `sessionLatestInput` map of full `InputMsg` structs — see Step 3 — rather than threading four more raw arrays through the signature, since the aim/charge/release fields are naturally grouped as one struct.)

Inside `SimulateSessionTick`'s body (read the current file to find exactly where the existing pickup/attack/revive/hazard/timer blocks are), make these changes:

1. **Disable the attack loop and hazard damage per Global Constraints** — wrap the existing attack-handling block (`if (attack) { ... }` and the `UpdateAttackCooldown` loop) and the `ApplyHazardDamage` loop in `if (false)` is NOT the right approach (dead code is bad practice) — instead, simply DELETE the call sites for this phase's tick (leave `Combat.h`/`Hazard.h` themselves untouched, just don't call `TryAttack`/`UpdateAttackCooldown`/`ApplyHazardDamage` from this function anymore). Remove:
   - The `if (attack) { for (...) { ... TryAttack ... } }` block.
   - The `for (int i = 0; i < kMaxPlayersPerSession; i++) { if (active[i]) UpdateAttackCooldown(...); }` loop.
   - The `for (int i = 0; i < kMaxPlayersPerSession; i++) { if (active[i]) ApplyHazardDamage(...); }` loop.

   Leave the pickup loop, revive loop, and `UpdateTimers` loop exactly as they are (pickup/revive/timers are still needed — pickup for potential future potion use in later phases, revive for the eventual 2v2 rework, timers for the Downed→Dead state machine that Hot Potato's explosion-down mechanic relies on).

2. **Add potato simulation**, inserted after the existing pickup loop (order doesn't matter much here since pickup and potato-throwing don't interact this phase, but keep it readable — insert after pickup, before revive):

```cpp
    // --- Hot Potato: charge tracking, throw, flight, catch, explosion ---
    bool soloMode = false;
    {
        int activeCount = 0;
        for (int i = 0; i < kMaxPlayersPerSession; i++) if (active[i]) activeCount++;
        soloMode = (activeCount == 1);
    }

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
        chargeTimer[holder] = 0.0f;
    }

    // Flight: integrate position, apply drag, check catch, check bounds.
    if (potato.inFlight) {
        potato.position.x += potato.velocity.x * dt;
        potato.position.y += potato.velocity.y * dt;
        ApplyPotatoDrag(potato, dt);

        Vector2 positions[kMaxPlayersPerSession];
        for (int i = 0; i < kMaxPlayersPerSession; i++) positions[i] = session.slots[i].player.position;

        int excludeSlot = -1; // thrower can be caught by their own throw once it's moving; no same-tick self-catch issue since position integrates before the check
        int catcher = FindCatchTarget(potato.position, positions, active, kMaxPlayersPerSession, excludeSlot);
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
                    // Multiplayer: leaving the court downs the last thrower and starts a new round.
                    session.slots[potato.lastThrowerSlot].player.ForceDown();
                    potato = HotPotato{};
                    // Respawn held by a random active player (see Step 3 for the round-reset helper used at session creation; here, pick any active slot).
                    for (int i = 0; i < kMaxPlayersPerSession; i++) {
                        if (active[i]) { potato.held = true; potato.holderSlot = i; potato.position = session.slots[i].player.position; potato.explodeTimer = ComputeExplodeTimerForCatch(0); break; }
                    }
                }
            }
        }
    }

    // Explosion: timer expires while held.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot]) {
        potato.explodeTimer -= dt;
        if (potato.explodeTimer <= 0.0f) {
            session.slots[potato.holderSlot].player.ForceDown();
            HotPotato reset{};
            for (int i = 0; i < kMaxPlayersPerSession; i++) {
                if (active[i]) { reset.held = true; reset.holderSlot = i; reset.position = session.slots[i].player.position; reset.explodeTimer = ComputeExplodeTimerForCatch(0); break; }
            }
            potato = reset;
        }
    }

    // Held potato tracks its holder's position each tick.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot]) {
        potato.position = session.slots[potato.holderSlot].player.position;
    }
```

Note on `excludeSlot = -1` above: the plan's design doc mentions preventing "instant same-tick self-catch on release" — in this implementation, release and the flight/catch check happen in the SAME tick call but as sequential code (release moves the potato out of `held` state and sets velocity; the catch check runs afterward in the same function call, against the ALREADY-INTEGRATED new position after one tick of movement at the new velocity). Since position is integrated before the catch check runs, the potato has already moved away from the thrower's exact position by the time catching is checked, so a same-tick self-catch is naturally avoided without needing an explicit exclude — VERIFY this holds in Task 3's testing (Step 4) rather than assuming it; if a same-tick self-catch DOES occur in testing (e.g. because the thrower doesn't move and the integrated distance is still within `kCatchRadius` for a low-force throw), add a one-tick "grace period" flag to `HotPotato` (e.g. `justThrown` bool cleared after one tick) and exclude `lastThrowerSlot` from `FindCatchTarget` only while `justThrown` is true. Implement this fallback ONLY if the smoke test in Step 4 demonstrates the problem — don't add unneeded state pre-emptively.

- [ ] **Step 3: Update `main()` to construct/track `HotPotato` per session and disable the hazard zone**

In `src/server/main.cpp`'s `main()` function:

1. Change the arena/hazard setup. Find `HazardZone hazard{ Rectangle{450.0f, 200.0f, 100.0f, 200.0f} };` and add the court-bounds rectangle alongside it:

```cpp
    HazardZone hazard{ Rectangle{450.0f, 200.0f, 100.0f, 200.0f} }; // retained per Global Constraints; not applied to Hot Potato sessions this phase
    Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
```

2. Add the new per-session state maps (alongside `sessionWorldItems` etc., per Step 1):

```cpp
    std::map<std::string, HotPotato> sessionPotato;
    std::map<std::string, float[kMaxPlayersPerSession]> sessionChargeTimer;
    std::map<std::string, InputMsg[kMaxPlayersPerSession]> sessionLatestInput;
```

3. In the session-creation block (find `if (sessionWorldItems.find(outcome.roomCode) == sessionWorldItems.end()) { ... }`), initialize the new per-session state:

```cpp
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
                            }
```

(Starting the potato held by slot 0 rather than a truly random active player is a deliberate simplification for this phase — at session-creation time, only slot 0 is guaranteed connected yet, since the session was JUST created by this very connect request. Randomizing the initial holder among "eventually connected" players doesn't make sense before other players have joined. This is fine and matches the spec's intent loosely enough for Phase 2; a more considered "random holder among active players at ROUND start" applies to Phase 3's round-reset logic, not initial session creation.)

4. Find the `Input` message-handling block (`if (header.channel == 0 && type == MessageType::Input) { ... }`) and store the full input alongside the existing `pendingAttack`/`pendingInteract` writes:

```cpp
                        if (header.channel == 0 && type == MessageType::Input) {
                            InputMsg input{};
                            if (DeserializeStruct(body, bodyLen, input)) {
                                if (slot.player.state == PlayerState::Alive) {
                                    slot.player.position.x += input.moveX * kMoveSpeed * (float)tickInterval;
                                    slot.player.position.y += input.moveY * kMoveSpeed * (float)tickInterval;
                                }
                                pendingAttack[loc.sessionName][loc.slotIndex] = input.attackPressed;
                                pendingInteract[loc.sessionName][loc.slotIndex] = input.interactHeld;
                                sessionLatestInput[loc.sessionName][loc.slotIndex] = input;
                            }
                        }
```

5. In the per-session tick loop (find `SimulateSessionTick(*session, items, hazard, hazardCarry, attack, interact, dt, active);`), update the call site and pass the new state:

```cpp
                bool active[kMaxPlayersPerSession];
                HotPotato& potato = sessionPotato[sessionEntry];
                float* chargeTimer = sessionChargeTimer[sessionEntry];
                InputMsg* latestInputs = sessionLatestInput.count(sessionEntry) ? sessionLatestInput[sessionEntry] : nullptr;
                if (!latestInputs) continue; // session exists but no input map yet (shouldn't happen once creation-time init runs, but guards a null deref)
                SimulateSessionTick(*session, items, hazard, hazardCarry, attack, interact, dt, active, potato, chargeTimer, latestInputs, courtBounds);
```

6. In the snapshot-building block, populate `snap.potato`:

```cpp
                snap.hazardX = hazard.bounds.x;
                snap.hazardY = hazard.bounds.y;
                snap.hazardW = hazard.bounds.width;
                snap.hazardH = hazard.bounds.height;
                snap.potato = PotatoSnapshot{ potato.position.x, potato.position.y, potato.held, potato.inFlight, potato.holderSlot, potato.explodeTimer };
```

- [ ] **Step 4: Add smoke tests for the new potato logic**

Add a new smoke test function `SmokeTestHotPotato()` in `src/server/main.cpp` (following the existing `SmokeTest*` pattern), covering:

```cpp
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
    assert(foundExcluded == -1); // slot 0 excluded, slot 2 (5,0) is outside kCatchRadius(20)? -- actually within 20, check math
    // Note: slot 2 at (5,0) is within kCatchRadius(20) of (0,0), so excluding slot 0 should still find slot 2.
    // Re-derive expected value from the actual constant rather than assuming — see Step 4 note below.
}
```

**IMPORTANT NOTE for the implementer**: the last two assertions in the sketch above have a note-to-self acknowledging the math needs verifying against the ACTUAL `kCatchRadius` value (20.0f, per Task 1). Don't copy that test body verbatim without first computing `DistanceBetween2({0,0}, {5,0}) = 5.0f`, which IS within `kCatchRadius = 20.0f` — so `foundExcluded` should actually be `2` (slot 2, the next-nearest in-range active candidate), not `-1`. Fix this assertion to `assert(foundExcluded == 2);` before finalizing — the plan's own sketch contains this error and you must correct it, not propagate it. This is exactly the kind of self-check the writing-plans process expects an implementer to catch.

Also add a full-integration test exercising `SimulateSessionTick`'s potato logic end-to-end (construct a 2-player `Session`, a `HotPotato` held by slot 0, simulate a charge+release+flight+catch sequence over several ticks, assert the potato ends up held by slot 1 with a shrunk `explodeTimer` and `catchCount == 1`), and a separate test for the explosion-downs-holder path (let `explodeTimer` run out while held, assert the holder becomes `Downed` and the potato resets to a fresh round state), and a solo-mode wall-bounce test (1 active player, throw the potato toward a boundary, assert it reflects rather than downing anyone).

Register `SmokeTestHotPotato()` in `RunAllSmokeTests()` following the existing pattern.

- [ ] **Step 5: Build and run**

Run: `mingw32-make server && ./bin/server.exe --test`

Expected: all smoke tests (existing + new `SmokeTestHotPotato`) pass with pristine output.

- [ ] **Step 6: Commit**

```bash
git add src/server/main.cpp
git commit -m "Implement server-side Hot Potato simulation: charge, throw, flight, catch, explosion, round reset"
```

---

### Task 4: Client input — mouse charge/release/aim

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `InputMsg`'s new fields (Task 2).
- Produces: `NetClient::SendInput`'s signature grows to accept the new fields (this task also touches `NetClient.h`/`.cpp` minimally to thread them through).

- [ ] **Step 1: Expand `NetClient::SendInput`'s signature**

In `src/client/NetClient.h`, change:

```cpp
    void SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed);
```

to:

```cpp
    void SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed,
                   bool chargingThrow, bool releaseThrow, float aimDirX, float aimDirY);
```

In `src/client/NetClient.cpp`, change:

```cpp
void NetClient::SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed) {
    if (!connected) return;

    InputMsg input{ moveX, moveY, interactHeld, attackPressed };
```

to:

```cpp
void NetClient::SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed,
                           bool chargingThrow, bool releaseThrow, float aimDirX, float aimDirY) {
    if (!connected) return;

    InputMsg input{ moveX, moveY, interactHeld, attackPressed, chargingThrow, releaseThrow, aimDirX, aimDirY };
```

- [ ] **Step 2: Add mouse-based charge/release/aim input to `main.cpp`'s game loop**

In `src/client/main.cpp`, find the input-gathering block:

```cpp
        Vector2 move{0, 0};
        if (IsKeyDown(KEY_W)) move.y -= 1;
        if (IsKeyDown(KEY_S)) move.y += 1;
        if (IsKeyDown(KEY_A)) move.x -= 1;
        if (IsKeyDown(KEY_D)) move.x += 1;
        bool interactHeld = IsKeyDown(KEY_E);
        bool attackPressed = IsKeyPressed(KEY_Q);

        netClient.SendInput(move.x, move.y, interactHeld, attackPressed);
```

replace with:

```cpp
        Vector2 move{0, 0};
        if (IsKeyDown(KEY_W)) move.y -= 1;
        if (IsKeyDown(KEY_S)) move.y += 1;
        if (IsKeyDown(KEY_A)) move.x -= 1;
        if (IsKeyDown(KEY_D)) move.x += 1;
        bool interactHeld = IsKeyDown(KEY_E);
        bool attackPressed = IsKeyPressed(KEY_Q);

        bool chargingThrow = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        bool releaseThrow = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

        // Aim direction: from my own player's last-known snapshot position toward the
        // cursor, in world space. The camera offset (shake) doesn't affect world-space
        // cursor mapping since GetMousePosition() is already screen-space and our camera
        // only ever translates (never zooms/rotates), so screen-space delta direction
        // equals world-space delta direction for aiming purposes.
        float aimDirX = 1.0f, aimDirY = 0.0f;
        {
            const SnapshotMsg& latestSnap = netClient.GetLatestSnapshot();
            if (mySlot < kMaxPlayersPerSession) {
                Vector2 myPos{ latestSnap.players[mySlot].posX, latestSnap.players[mySlot].posY };
                Vector2 mouseScreen = GetMousePosition();
                // Screen space has origin top-left matching our world coordinates directly
                // (no separate world/screen offset beyond the shake-only camera), so this
                // subtraction is valid without an explicit screen-to-world unproject call.
                Vector2 toMouse{ mouseScreen.x - myPos.x, mouseScreen.y - myPos.y };
                float len = std::sqrt(toMouse.x * toMouse.x + toMouse.y * toMouse.y);
                if (len > 0.0001f) {
                    aimDirX = toMouse.x / len;
                    aimDirY = toMouse.y / len;
                }
            }
        }

        netClient.SendInput(move.x, move.y, interactHeld, attackPressed, chargingThrow, releaseThrow, aimDirX, aimDirY);
```

**IMPORTANT CAVEAT the implementer must verify**: the comment above claims screen-space mouse position can be subtracted directly from world-space player position because "the camera only ever translates." Verify this against the ACTUAL camera setup in the current file (`Camera2D camera{}; camera.offset = ...; camera.target = ...;`) — raylib's `Camera2D` with a non-zero `offset`/`target` pair DOES perform a translation between screen and world space that is NOT simply "screen position equals world position." If the existing shake-camera offset is non-zero at the moment of aiming (which it usually is near-zero when not shaking, but is NEVER exactly guaranteed zero), a naive subtraction introduces a small aiming error proportional to the current shake magnitude. Given shake is typically small (max 12px per the existing `kShakeMaxOffsetPixels`) and aiming precision at that scale is not gameplay-critical for this phase, using raylib's `GetScreenToWorld2D(mouseScreen, camera)` instead of a raw subtraction is the CORRECT and only slightly more code fix — use it:

```cpp
                Vector2 mouseWorld = GetScreenToWorld2D(mouseScreen, camera);
                Vector2 toMouse{ mouseWorld.x - myPos.x, mouseWorld.y - myPos.y };
```

This requires the `camera` variable to already be constructed before this point in the frame — check the current file's ordering; if `camera` is constructed AFTER the input-gathering block (likely, since camera setup currently happens right before `BeginMode2D`), you must either move the camera construction earlier in the frame (before input gathering) or duplicate the camera-offset computation inline. The cleanest fix: move the `Camera2D camera{...}` construction to occur BEFORE the input-gathering block (it only depends on `effects.GetShakeOffsetX/Y()`, which are already computed from the PREVIOUS frame's snapshot at this point in the loop — using last-frame's shake value for this frame's aim computation is a one-frame lag, imperceptible and harmless). Verify this reordering doesn't break anything else that currently depends on `camera` being constructed where it is (the `BeginMode2D(camera)` call immediately after still works identically once `camera` exists earlier).

- [ ] **Step 3: Build**

Run: `mingw32-make client`

Expected: builds cleanly.

- [ ] **Step 4: Commit**

```bash
git add src/client/NetClient.h src/client/NetClient.cpp src/client/main.cpp
git commit -m "Add mouse-based charge/release/aim input for Hot Potato throwing"
```

---

### Task 5: Client rendering — draw the potato, its explosion timer over the holder's head

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `SnapshotMsg::potato` (Task 2), server-computed potato state (Task 3).

- [ ] **Step 1: Add potato rendering inside the existing `BeginMode2D`/`EndMode2D` block**

In `src/client/main.cpp`, after the existing item-drawing loop (`for (int i = 0; i < 2; i++) { if (snap.items[i].active) { ... } }`) and before the `drawPlayer` lambda definition (or after the player-drawing loop — either position works since the potato doesn't depend on player draw state; insert it after the player loop for simplicity, so it renders on top):

```cpp
        // Draw the potato itself (a simple colored circle; no arc/height rendering this phase).
        {
            const PotatoSnapshot& potato = snap.potato;
            Color potatoColor = BROWN;
            DrawCircleV(Vector2{ potato.posX, potato.posY }, 10.0f, potatoColor);

            // Explosion countdown, shown above whoever's currently holding it.
            if (potato.held && potato.holderSlot >= 0 && potato.holderSlot < kMaxPlayersPerSession) {
                const PlayerSnapshot& holderSnap = snap.players[potato.holderSlot];
                Vector2 textPos{ holderSnap.posX, holderSnap.posY - 50.0f };
                Color timerColor = potato.explodeTimer <= 2.0f ? RED : BLACK; // urgency cue in the final 2 seconds
                DrawText(TextFormat("%.1f", potato.explodeTimer), (int)textPos.x - 12, (int)textPos.y, 16, timerColor);
            }
        }
```

- [ ] **Step 2: Build**

Run: `mingw32-make client`

Expected: builds cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/client/main.cpp
git commit -m "Render the Hot Potato and its explosion countdown over the holder's head"
```

---

### Task 6: Client smoke test for aim-direction math (pure logic, no raylib window needed)

**Files:**
- Create: `src/client/AimDirection.h`
- Create: `src/client/AimDirectionTests.h`
- Modify: `src/client/main.cpp` (use the extracted function, wire tests into `--test`)

**Interfaces:**
- Consumes: nothing new — this is a standalone test of the aim-direction normalization math extracted as a small pure function, mirroring the project's existing pattern of extracting testable logic (e.g. `RoomMenu::AppendDigitInput`) out of raylib-dependent code.

- [ ] **Step 1: Extract the aim-direction computation into a small testable free function**

In `src/client/main.cpp`, the aim-direction block built in Task 4 contains normalization logic that's awkward to unit-test inline (it reads `GetMousePosition()`/`GetScreenToWorld2D()` directly). Extract just the pure vector-math part into a free function in a new header, `src/client/AimDirection.h`:
```cpp
#pragma once

#include "raylib-cpp.hpp"
#include <cmath>

// Normalizes the direction from `from` to `to`; returns {1,0} if the points coincide
// (degenerate case — avoids a zero-length aim vector reaching the server).
inline Vector2 ComputeAimDirection(Vector2 from, Vector2 to) {
    Vector2 delta{ to.x - from.x, to.y - from.y };
    float len = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (len < 0.0001f) {
        return Vector2{1.0f, 0.0f};
    }
    return Vector2{ delta.x / len, delta.y / len };
}
```

Update Task 4's inline aim-computation in `main.cpp` to call this function instead of duplicating the math:

```cpp
                Vector2 aimDir = ComputeAimDirection(myPos, mouseWorld);
                aimDirX = aimDir.x;
                aimDirY = aimDir.y;
```

(Add `#include "AimDirection.h"` to `main.cpp`'s includes.)

- [ ] **Step 2: Write the smoke test**

Create `src/client/AimDirectionTests.h`:

```cpp
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "AimDirection.h"

inline void RunAimDirectionSmokeTests() {
    // Straight right
    {
        Vector2 dir = ComputeAimDirection(Vector2{0.0f, 0.0f}, Vector2{10.0f, 0.0f});
        if (std::fabs(dir.x - 1.0f) > 0.001f || std::fabs(dir.y - 0.0f) > 0.001f) {
            printf("FAIL: expected direction (1,0), got (%f,%f)\n", dir.x, dir.y);
            exit(1);
        }
        printf("PASS: aim direction straight right normalizes correctly\n");
    }

    // Diagonal
    {
        Vector2 dir = ComputeAimDirection(Vector2{0.0f, 0.0f}, Vector2{3.0f, 4.0f});
        float expectedX = 3.0f / 5.0f, expectedY = 4.0f / 5.0f; // 3-4-5 triangle
        if (std::fabs(dir.x - expectedX) > 0.001f || std::fabs(dir.y - expectedY) > 0.001f) {
            printf("FAIL: expected direction (%f,%f), got (%f,%f)\n", expectedX, expectedY, dir.x, dir.y);
            exit(1);
        }
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (std::fabs(len - 1.0f) > 0.001f) {
            printf("FAIL: expected unit length, got %f\n", len);
            exit(1);
        }
        printf("PASS: diagonal aim direction normalizes to correct unit vector\n");
    }

    // Degenerate: same point returns a default direction, not NaN/zero
    {
        Vector2 dir = ComputeAimDirection(Vector2{50.0f, 50.0f}, Vector2{50.0f, 50.0f});
        if (std::fabs(dir.x - 1.0f) > 0.001f || std::fabs(dir.y - 0.0f) > 0.001f) {
            printf("FAIL: expected degenerate default (1,0), got (%f,%f)\n", dir.x, dir.y);
            exit(1);
        }
        printf("PASS: degenerate same-point aim returns default direction, not NaN\n");
    }

    printf("All AimDirection smoke tests passed.\n");
}
```

- [ ] **Step 3: Wire into `main.cpp`'s `--test` branch**

Add `#include "AimDirectionTests.h"` and a call to `RunAimDirectionSmokeTests();` alongside the existing `RunJuiceSmokeTests();`/`RunRoomMenuSmokeTests();` calls.

- [ ] **Step 4: Build and run**

Run: `mingw32-make client && ./bin/client.exe --test`

Expected: all existing smoke tests plus the 3 new AimDirection tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/client/AimDirection.h src/client/AimDirectionTests.h src/client/main.cpp
git commit -m "Extract and test aim-direction normalization logic"
```

---

## Final Verification

After all six tasks:

- [ ] Run `mingw32-make server && mingw32-make client` — both build cleanly.
- [ ] Run `./bin/server.exe --test` — all server smoke tests (including the new `SmokeTestHotPotato`) pass.
- [ ] Run `./bin/client.exe --test` — all client smoke tests (including the new AimDirection tests) pass.
- [ ] Live verification with 2+ clients: connect two `bin/client.exe` instances to one room. Confirm: the potato appears (a brown circle) held by whichever player's slot is the initial holder; holding left mouse charges a throw (no visible charge indicator required this phase — verify via server behavior, i.e. a longer hold visibly throws farther); releasing throws it toward the mouse cursor; the potato flies, decelerates, and is auto-caught by the other player when they get close; the explosion countdown number appears above the current holder's head and shrinks faster after each catch; if the timer hits 0 while held, that player becomes Downed (existing Downed visual — gray circle — should appear) and a new round starts with the potato spawning held by an active player.
- [ ] Live verification, solo mode: connect exactly ONE client. Confirm: throwing the potato toward a wall causes it to bounce back into play rather than downing anyone or ending anything.
- [ ] Live verification, out-of-bounds (2+ clients): deliberately throw the potato out of the arena. Confirm the LAST THROWER becomes Downed, not the original holder.
- [ ] Confirm melee attack (`Q`) and the hazard zone visibly no longer affect gameplay (Q does nothing now; standing in the old hazard-zone rectangle area, if still drawn, deals no damage) — consistent with this phase's Global Constraints.
