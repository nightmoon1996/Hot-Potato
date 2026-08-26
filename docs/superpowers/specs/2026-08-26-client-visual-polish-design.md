# Client Visual Polish Design

## Goal

Add "juicy" client-side visual feedback to the existing networked 2-player game: screen shake, hit flash, floating damage numbers, tweened HP bars, particle bursts, and a dot-ring revive indicator. All of this is purely cosmetic, client-side, and derived by observing consecutive server snapshots — no protocol or server changes.

## Non-goals

- No changes to `src/shared/Protocol.h`, `src/server/`, or the wire format.
- No client-side prediction or simulation — the client still only renders what the server's snapshot says; juice is a presentation layer on top, never affects gameplay state.
- No general-purpose particle/animation engine — this is scoped to the specific effects listed below with fixed-capacity pools, not an extensible framework.

## Context

The current client (`src/client/main.cpp`) renders directly from `NetClient::GetLatestSnapshot()` every frame with no per-frame client state: HP bars read `p.hp` directly, the revive indicator is a `DrawCircleSector` pie slice, and there is no shake, flash, particles, or floating text. `SnapshotMsg` (`src/shared/Protocol.h`) has no event log — only current-state fields (`PlayerSnapshot`: posX/posY/hp/state/potionCount/channelTimer; `WorldItemSnapshot`: posX/posY/active). All "events" (a hit landed, an item was picked up, a revive completed) must be inferred by the client by comparing this frame's snapshot to the previous frame's.

## Detecting events via diffing

The client keeps a small cache of the previous frame's relevant snapshot fields per player and per item slot, and compares against the new snapshot each frame:

- **Hit/damage**: player's `hp` this frame < `hp` last frame → damage of `(last - current)` occurred at that player's position.
- **Revive complete**: player's `state` last frame was Downed (1) and this frame is Alive (0) → revive burst at that player's position.
- **Item pickup**: an item slot's `active` was `true` last frame and is `false` this frame → pickup burst at that item's last-known position (not tied to which player picked it up).

Diffing is inherently approximate (e.g. hazard-zone damage-over-time and a melee hit both look like "HP dropped") but that ambiguity is acceptable for this scope — no gameplay logic depends on distinguishing them, only the visual burst.

## Components

### `ClientEffectsState` (new: `src/client/Juice.h`, possibly + `.cpp`)

One instance owned by `main.cpp`, updated once per frame after receiving a snapshot, then consumed only by rendering code. Holds:

- **Per-player (2) diff cache**: previous `hp`, previous `state`, previous `potionCount` (potionCount tracked for completeness/future use, not required by any effect in this scope beyond revive/pickup already covered by state/item diffing).
- **Per-player (2) presentation state**: `displayedHp` (float, eased toward real `hp` via exponential smoothing), `hitFlashTimer` (float, counts down from a fixed constant on a hit).
- **Per-item (2) diff cache**: previous `active`.
- **Global**: `shakeTrauma` (float 0..1, added to on local-player hits, decays every frame).
- **Particle pool**: fixed-size array (128) of `{ Vector2 pos; Vector2 vel; Color color; float age; float lifetime; bool active; }`.
- **Damage number pool**: fixed-size array (32) of `{ Vector2 pos; int value; float age; float lifetime; bool active; }`.

Constants (all in `Juice.h`):
- `kHitFlashDuration = 0.15f`
- `kDamageNumberLifetime = 0.7f`
- `kDamageNumberRiseSpeed = 40.0f` (px/sec upward drift)
- `kHpDisplaySmoothingRate = 8.0f` (exponential smoothing coefficient)
- `kShakeTraumaPerHit = 0.4f`
- `kShakeTraumaDecayPerSecond = 1.0f`
- `kShakeMaxOffsetPixels = 12.0f`
- `kParticleLifetime = 0.5f`
- `kParticlesPerBurst = 10`
- `kReviveRingDotCount = 12`

### `ClientEffectsState::Update(const SnapshotMsg& snap, uint8_t mySlot, float dt)`

Runs once per frame, in this order:
1. For each player slot `i` in {0, 1}:
   - If `snap.players[i].state != 3` (present) and cached-previous state also present and `snap.players[i].hp < prevHp[i]`: spawn a damage number (value = `prevHp[i] - snap.players[i].hp`) and a particle burst at `{posX, posY}`; set `hitFlashTimer[i] = kHitFlashDuration`; if `i == mySlot`, `shakeTrauma = min(1.0, shakeTrauma + kShakeTraumaPerHit)`.
   - If `prevState[i] == 1 (Downed)` and `snap.players[i].state == 0 (Alive)`: spawn a revive-complete particle burst at the player's position.
   - `displayedHp[i] += (snap.players[i].hp - displayedHp[i]) * min(1.0f, kHpDisplaySmoothingRate * dt)`.
   - `hitFlashTimer[i] = max(0.0f, hitFlashTimer[i] - dt)`.
   - Update `prevHp[i]`, `prevState[i]`, `prevPotionCount[i]` from this frame's snapshot (skip updating "previous" for absent slots, so a player reappearing doesn't register as a false hit/revive).
2. For each item slot `j` in {0, 1}: if `prevActive[j] && !snap.items[j].active`, spawn a pickup particle burst at the item's cached previous position; update `prevActive[j]`/cached position.
3. `shakeTrauma = max(0.0f, shakeTrauma - kShakeTraumaDecayPerSecond * dt)`.
4. Advance all active particles (`pos += vel * dt`, `age += dt`, deactivate when `age >= lifetime`) and all active damage numbers (`pos.y -= kDamageNumberRiseSpeed * dt`, `age += dt`, deactivate when `age >= lifetime`).

Helper methods:
- `SpawnBurst(Vector2 pos, Color color)` — activates up to `kParticlesPerBurst` free slots in the particle pool with randomized velocity direction/magnitude around `pos`. Silently does nothing if the pool has no free slots (fixed capacity, no overflow handling needed at this scale).
- `SpawnDamageNumber(Vector2 pos, int value)` — activates one free slot in the damage-number pool.
- `float GetShakeOffsetX/Y()` — returns a random jitter scaled by `shakeTrauma * shakeTrauma * kShakeMaxOffsetPixels` (squared trauma is the standard "trauma" shake curve — small hits barely shake, big/repeated hits shake hard).

### Rendering changes (`src/client/main.cpp`)

- Construct a `Camera2D` with `offset = { screenWidth/2 + shakeOffsetX, screenHeight/2 + shakeOffsetY }`, `target = { screenWidth/2, screenHeight/2 }`, zoom 1 — recomputed every frame from `effects.GetShakeOffsetX/Y()`. Wrap existing world-space drawing (hazard, items, players) in `BeginMode2D(camera)` / `EndMode2D()`. UI text (slot label, F1 hint, debug menu) stays outside the camera block, unaffected by shake.
- In `drawPlayer`: blend `drawColor` toward `WHITE` proportional to `hitFlashTimer[i] / kHitFlashDuration` (e.g. `ColorLerp(drawColor, WHITE, flashRatio)`).
- HP bar width computed from `effects.displayedHp[i]` instead of `p.hp`.
- Revive indicator: replace the `DrawCircleSector` call with a loop over `kReviveRingDotCount` dots positioned evenly around a circle of the same radius (24px) centered on the player; dot `k` is drawn filled (`SKYBLUE`) if `k < kReviveRingDotCount * (channelTimer / channelDuration)`, else drawn as a hollow/dim outline.
- After drawing players/world, loop over active particles and draw each as a small filled circle fading out via `Fade(color, 1.0f - age/lifetime)`.
- Loop over active damage numbers and draw each as red `DrawText("-N", ...)` at its current position, alpha fading via `age/lifetime` (raylib text doesn't support alpha directly on `DrawText`, so use `Fade` with `DrawTextEx` or draw color scaled manually — implementation detail for the plan).

## Testing

This is presentation-only code; there's no gameplay behavior to unit test end-to-end, but the diffing/decay/smoothing math is plain arithmetic and can be smoke-tested the same way the server does (`RunAllSmokeTests()`-style, gated behind a `--test` arg or a small standalone test entry point), covering:
- HP-drop diff triggers a damage number + burst; HP-rise does not.
- Downed→Alive triggers a revive burst; Alive→Downed does not.
- Item active→inactive triggers a pickup burst; inactive→active does not.
- `displayedHp` converges toward real hp over repeated `Update` calls.
- `shakeTrauma` decays to 0 over time with no further hits.
- Particle/damage-number pool `Spawn*` calls don't exceed pool capacity and correctly reuse expired slots.

Beyond that, verification is running two live clients, triggering hits (PvP attack, hazard zone), pickups, and a full revive channel, and visually confirming each effect fires correctly and doesn't interfere with existing debug menu / snapshot rendering.

## Open items carried into the plan

None — this spec is fully scoped. The implementation plan should sequence work as: (1) `Juice.h` core state + diffing/update logic + smoke tests, (2) camera/shake integration into `main.cpp`, (3) hit-flash + tweened HP bars, (4) particle bursts, (5) damage numbers, (6) dot-ring revive indicator — each an independently testable/reviewable increment.
