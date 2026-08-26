# Hot Potato Phase 4: Dash Ability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a short-burst movement dash: pressing a dash key instantly displaces the player 150px in their current (or last-known) movement direction, on a 2-second cooldown, clamped to stay inside the arena bounds. No invincibility, no interaction with combat/potato mechanics beyond the position change itself.

**Architecture:** `Player` gains two new fields: `dashCooldownTimer` (ticks down every server tick, mirroring the existing `attackCooldownTimer` pattern) and `facingDirection` (a `Vector2`, updated whenever movement input is non-zero, used as the dash direction when the player isn't currently pressing a movement key). `InputMsg` gains one new field, `dashPressed`. The dash itself is applied at the SAME site where movement is already applied directly on packet receipt (not inside `SimulateSessionTick`, which only runs on the fixed 60Hz tick) — this matches how movement itself works today (applied immediately per-input-packet, not tick-batched) and keeps dash feeling equally responsive. `dashCooldownTimer`'s countdown, however, IS ticked inside `SimulateSessionTick` alongside the existing `UpdateAttackCooldown` pattern, since cooldown decay is a time-based process that belongs in the fixed tick, not the input-handling path.

**Tech Stack:** C++17, existing UDP client/server protocol (additive `InputMsg` field), raylib/raylib-cpp (one new key binding, no new rendering required — a dash has no persistent visual state to draw, just an instantaneous position change already reflected by existing player rendering).

**Spec:** [docs/superpowers/specs/2026-08-26-hot-potato-design.md](../specs/2026-08-26-hot-potato-design.md) (Phase 4 section — "Dash Ability")

## Global Constraints

- `kDashDistance = 150.0f` (px, instant displacement), `kDashCooldown = 2.0f` (s).
- Dash requires `dashCooldownTimer <= 0.0f` AND `player.state == PlayerState::Alive`. A dash attempt that fails either check is silently ignored (no partial effect, no cooldown reset).
- Direction: the player's CURRENT movement input direction if non-zero (normalized `moveX`/`moveY`), else their last-known non-zero movement direction (`facingDirection`, updated every time movement input is non-zero, regardless of whether a dash occurs that tick). If `facingDirection` has never been set (a player who has never moved), default to a sensible fixed direction (e.g. `{1.0f, 0.0f}`) rather than a zero vector, so a dash never becomes a no-op displacement of zero distance.
- No collision/wall-clamping beyond the arena `courtBounds` rectangle — a dash whose destination would land outside `courtBounds` is clamped to just inside the boundary (same rectangle already used by Phase 2's potato out-of-bounds check).
- No invincibility frames, no special interaction with the potato's catch radius, revive range, or any other mechanic — dashing is purely a position change that other systems observe normally on their next check.
- Protocol changes additive only (`InputMsg` gains one field appended at the end).
- Builds with `mingw32-make server` / `mingw32-make client` using the existing Makefile.
- Toolchain: MinGW at `C:\msys64\mingw64\bin`, add to PATH if needed (bash: `export PATH="/c/msys64/mingw64/bin:$PATH"`). If a rebuild fails with a linker "Permission denied," a previous `server.exe`/`client.exe` may be running — check `tasklist //FI "IMAGENAME eq server.exe"` / `client.exe` and ASK before killing (don't kill without asking).

---

### Task 1: `Player` gains dash cooldown and facing-direction state, plus pure dash-displacement logic

**Files:**
- Modify: `src/server/Player.h`
- Create: `src/server/Dash.h`

**Interfaces:**
- Produces: `Player::dashCooldownTimer`, `Player::facingDirection` fields; `kDashDistance`, `kDashCooldown` constants (in `Dash.h`); a free function `Vector2 ComputeDashDestination(Vector2 currentPosition, Vector2 dashDirection, Rectangle courtBounds)` that computes the clamped destination, and `Vector2 ResolveDashDirection(Vector2 moveInput, Vector2 facingDirection)` that picks the current-input-or-fallback direction. Consumed by Task 3 (server input-handling integration).

- [ ] **Step 1: Add `dashCooldownTimer` and `facingDirection` to `Player`**

In `src/server/Player.h`, add two new fields to the `Player` class and initialize them in the constructor. Replace:

```cpp
    Vector2 position;
    Vector2 spawnPoint;
    int hp;
    PlayerState state;
    float downedTimer;
    float deathRespawnTimer;
    float attackCooldownTimer;
    float channelTimer;
    Inventory inventory;

    explicit Player(Vector2 spawn)
        : position(spawn), spawnPoint(spawn), hp(kMaxHp), state(PlayerState::Alive),
          downedTimer(0.0f), deathRespawnTimer(0.0f), attackCooldownTimer(0.0f), channelTimer(0.0f) {}
```

with:

```cpp
    Vector2 position;
    Vector2 spawnPoint;
    int hp;
    PlayerState state;
    float downedTimer;
    float deathRespawnTimer;
    float attackCooldownTimer;
    float channelTimer;
    float dashCooldownTimer;
    Vector2 facingDirection; // last non-zero movement direction; used as the dash direction
                             // when the player isn't currently pressing a movement key
    Inventory inventory;

    explicit Player(Vector2 spawn)
        : position(spawn), spawnPoint(spawn), hp(kMaxHp), state(PlayerState::Alive),
          downedTimer(0.0f), deathRespawnTimer(0.0f), attackCooldownTimer(0.0f), channelTimer(0.0f),
          dashCooldownTimer(0.0f), facingDirection(Vector2{1.0f, 0.0f}) {}
```

(`facingDirection` defaults to `{1.0f, 0.0f}` — a sensible non-zero direction — so a player who dashes before ever moving still gets a real, non-zero-distance dash.)

- [ ] **Step 2: Write `Dash.h` with constants and pure direction/destination logic**

```cpp
#pragma once

#include "../shared/Geometry.h"
#include <cmath>

constexpr float kDashDistance = 150.0f;
constexpr float kDashCooldown = 2.0f;

// Picks the dash direction: the player's current movement input if it's non-zero
// (normalized), else their last-known facing direction (already normalized/maintained
// by the caller). This never returns a zero vector as long as `facingDirection` itself
// is never zero (Player's constructor seeds it to {1,0}, and it's only ever overwritten
// with normalized non-zero movement input, so it stays non-zero for the object's lifetime).
inline Vector2 ResolveDashDirection(Vector2 moveInput, Vector2 facingDirection) {
    float len = std::sqrt(moveInput.x * moveInput.x + moveInput.y * moveInput.y);
    if (len > 0.0001f) {
        return Vector2{ moveInput.x / len, moveInput.y / len };
    }
    return facingDirection;
}

// Computes the dash's destination position, clamped to stay inside courtBounds.
inline Vector2 ComputeDashDestination(Vector2 currentPosition, Vector2 dashDirection, Rectangle courtBounds) {
    Vector2 dest{ currentPosition.x + dashDirection.x * kDashDistance, currentPosition.y + dashDirection.y * kDashDistance };
    if (dest.x < courtBounds.x) dest.x = courtBounds.x;
    if (dest.x > courtBounds.x + courtBounds.width) dest.x = courtBounds.x + courtBounds.width;
    if (dest.y < courtBounds.y) dest.y = courtBounds.y;
    if (dest.y > courtBounds.y + courtBounds.height) dest.y = courtBounds.y + courtBounds.height;
    return dest;
}
```

- [ ] **Step 3: Build to confirm no compile error**

Run: nothing includes `Dash.h` yet, and `Player.h`'s new fields are used nowhere yet — but `Player.h` IS already included by `main.cpp`, `Item.h`, `Combat.h`, `Hazard.h`, etc., so this task's `Player.h` change WILL be compiled as part of any existing build. Run `mingw32-make server` to confirm the `Player.h` change alone doesn't break anything (the new fields are simply unused additions, which is not an error in C++).

Expected: builds cleanly (this task doesn't change any BEHAVIOR yet, only adds unused-so-far fields and a new header nothing includes).

- [ ] **Step 4: Commit**

```bash
git add src/server/Player.h src/server/Dash.h
git commit -m "Add Player dash-cooldown/facing-direction fields and pure dash-direction/destination logic"
```

---

### Task 2: Protocol addition — `dashPressed` input field

**Files:**
- Modify: `src/shared/Protocol.h`

**Interfaces:**
- Produces: `InputMsg::dashPressed` (appended at the end). Consumed by Task 3 (server), Task 4 (client).

- [ ] **Step 1: Add `dashPressed` to `InputMsg`**

In `src/shared/Protocol.h`, replace:

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
    bool dashPressed;
};
```

- [ ] **Step 2: Build both targets to confirm no compile breakage**

Run: `mingw32-make server && mingw32-make client`

Expected: this task alone may build completely cleanly (an added trailing struct field with no consuming code yet is not a compile error, similar to prior phases' analogous tasks) — this is fine and not a sign anything was missed.

- [ ] **Step 3: Commit**

```bash
git add src/shared/Protocol.h
git commit -m "Add dashPressed field to InputMsg"
```

---

### Task 3: Server-side dash — apply displacement on input, tick down cooldown, track facing direction

**Files:**
- Modify: `src/server/main.cpp`

**Interfaces:**
- Consumes: `Player::dashCooldownTimer`/`facingDirection` (Task 1), `ResolveDashDirection`/`ComputeDashDestination` (Task 1), `InputMsg::dashPressed` (Task 2).
- Produces: dash displacement applied immediately on packet receipt (mirroring how movement itself is applied); `dashCooldownTimer` ticked down once per `SimulateSessionTick` call (mirroring `UpdateAttackCooldown`'s existing per-tick pattern).

Read the CURRENT state of `src/server/main.cpp` yourself before editing — locate the exact `Input` message-handling block (where `input.moveX`/`input.moveY` are currently applied directly to `slot.player.position`) and the exact per-player cooldown-ticking loop inside `SimulateSessionTick` (where `UpdateAttackCooldown` is called for each active player — NOTE: per Phase 3, melee attack is no longer called from `SimulateSessionTick`, but re-check whether `UpdateAttackCooldown` itself is still invoked anywhere, since Phase 2's removal only removed `TryAttack`'s call site and the `UpdateAttackCooldown` loop specifically — read the actual current file to confirm exactly what per-tick cooldown-style loops currently exist, since dash's cooldown-ticking loop should be added in the same structural spot, whatever that turns out to be after Phase 2/3's changes).

- [ ] **Step 1: Add `#include "Dash.h"`**

Add `#include "Dash.h"` to `src/server/main.cpp`'s includes, alongside `HotPotato.h`/`MatchState.h`.

- [ ] **Step 2: Update `facingDirection` and apply dash on packet receipt**

In the `Input` message-handling block, find:

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

replace with:

```cpp
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
                        }
```

(`std::sqrt` requires `<cmath>` — check if `main.cpp` already includes it; add `#include <cmath>` if not already present.)

Note: `courtBounds` must be in scope at this point in `main()` — verify it's declared before the packet-receive loop (it already is, per Phase 2's Task 3, used for the potato's out-of-bounds check inside the per-session tick loop later in `main()`; confirm it's ALSO accessible at this earlier point in the function, which it should be since it's a single local declared once near the top of `main()`, not scoped inside the tick loop).

- [ ] **Step 3: Tick down `dashCooldownTimer` once per server tick**

Phase 2 fully removed `UpdateAttackCooldown`'s call site from `SimulateSessionTick` (not just `TryAttack`'s) — the only remaining per-active-player per-tick loop outside the `matchOver` guard is the `UpdateTimers` loop. Add the new dash-cooldown loop directly alongside it:

```cpp
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (active[i] && session.slots[i].player.dashCooldownTimer > 0.0f) {
            session.slots[i].player.dashCooldownTimer -= dt;
            if (session.slots[i].player.dashCooldownTimer < 0.0f) session.slots[i].player.dashCooldownTimer = 0.0f;
        }
    }
```

Place this loop OUTSIDE the `if (!match.matchOver) { ... }` guard added in Phase 3 (dash cooldown should keep decaying even if a match has concluded — it's a player-movement mechanic unrelated to potato/match state, not something that needs to freeze). If unsure where exactly to place it relative to the guard, place it directly alongside the `UpdateTimers` loop (`for (int i = 0; i < kMaxPlayersPerSession; i++) { if (active[i]) session.slots[i].player.UpdateTimers(dt); }`), which is also outside that guard and runs unconditionally for every active player.

- [ ] **Step 4: Build**

Run: `mingw32-make server`

Expected: builds cleanly.

- [ ] **Step 5: Add smoke tests for dash logic**

Add a new smoke test function `SmokeTestDash()` in `src/server/main.cpp` (following the existing `SmokeTest*` pattern), covering:

```cpp
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

        // Tick the cooldown down to 0, confirm dash works again
        for (int i = 0; i < 120; i++) { // 2 seconds at 60Hz
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
```

Register `SmokeTestDash()` in `RunAllSmokeTests()` following the existing pattern.

- [ ] **Step 6: Build and run**

Run: `mingw32-make server && ./bin/server.exe --test`

Expected: all smoke tests (existing + new `SmokeTestDash`) pass with pristine output.

- [ ] **Step 7: Commit**

```bash
git add src/server/main.cpp
git commit -m "Apply dash displacement on input, tick down cooldown, track facing direction"
```

---

### Task 4: Client dash input

**Files:**
- Modify: `src/client/NetClient.h`
- Modify: `src/client/NetClient.cpp`
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `InputMsg::dashPressed` (Task 2).
- Produces: `NetClient::SendInput`'s signature grows to accept `dashPressed`; `main.cpp` reads a dash key and passes it through.

- [ ] **Step 1: Expand `NetClient::SendInput`'s signature**

In `src/client/NetClient.h`, change:

```cpp
    void SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed,
                   bool chargingThrow, bool releaseThrow, float aimDirX, float aimDirY);
```

to:

```cpp
    void SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed,
                   bool chargingThrow, bool releaseThrow, float aimDirX, float aimDirY, bool dashPressed);
```

In `src/client/NetClient.cpp`, change:

```cpp
void NetClient::SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed,
                           bool chargingThrow, bool releaseThrow, float aimDirX, float aimDirY) {
    if (!connected) return;

    InputMsg input{ moveX, moveY, interactHeld, attackPressed, chargingThrow, releaseThrow, aimDirX, aimDirY };
```

to:

```cpp
void NetClient::SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed,
                           bool chargingThrow, bool releaseThrow, float aimDirX, float aimDirY, bool dashPressed) {
    if (!connected) return;

    InputMsg input{ moveX, moveY, interactHeld, attackPressed, chargingThrow, releaseThrow, aimDirX, aimDirY, dashPressed };
```

- [ ] **Step 2: Read a dash key in `main.cpp` and update the `SendInput` call site**

In `src/client/main.cpp`, find the input-gathering block (where `chargingThrow`/`releaseThrow` are read, from Phase 2) and add:

```cpp
        bool dashPressed = IsKeyPressed(KEY_LEFT_SHIFT) || IsKeyPressed(KEY_RIGHT_SHIFT);
```

Update the `SendInput` call site to append the new argument:

```cpp
        netClient.SendInput(move.x, move.y, interactHeld, attackPressed, chargingThrow, releaseThrow, aimDirX, aimDirY, dashPressed);
```

Read the CURRENT exact call site (its argument list may not match this snippet verbatim depending on exactly how Phase 2 left it) before making this edit.

- [ ] **Step 3: Build**

Run: `mingw32-make client`

Expected: builds cleanly.

- [ ] **Step 4: Commit**

```bash
git add src/client/NetClient.h src/client/NetClient.cpp src/client/main.cpp
git commit -m "Add Shift-key dash input on the client"
```

---

## Final Verification

After all four tasks:

- [ ] Run `mingw32-make server && mingw32-make client` — both build cleanly.
- [ ] Run `./bin/server.exe --test` — all server smoke tests (including the new `SmokeTestDash`) pass.
- [ ] Run `./bin/client.exe --test` — all client smoke tests still pass (unaffected, but confirm no regression).
- [ ] Live verification with 1+ clients: press Shift while moving — confirm the player instantly displaces ~150px in the movement direction. Press Shift again immediately — confirm nothing happens (on cooldown). Wait 2 seconds, press Shift again — confirm it works again. Stand still (no movement key held) and press Shift — confirm the player dashes in whatever direction they were last moving (or a sensible default direction if they've never moved since connecting).
- [ ] Live verification: dash toward the arena boundary — confirm the player's final position is clamped to stay inside the court, never landing outside it.
- [ ] Confirm dash has no effect while Downed or Dead (attempt to dash in that state — the position should not change, and the cooldown should not be consumed since the check is `state == Alive` gated before any cooldown/position logic runs).
- [ ] Confirm the dash doesn't visually break anything in the existing rendering (juice effects, HP bars, revive ring, potato/HUD) — since dash is purely a `position` change, all of these should simply track the new position on their next snapshot, same as any other movement.
