# Client Visual Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add juicy client-side visual feedback (screen shake, hit flash, floating damage numbers, tweened HP bars, particle bursts, dot-ring revive indicator) to the existing networked client, purely as a presentation layer derived from diffing consecutive server snapshots.

**Architecture:** A new `ClientEffectsState` class (`src/client/Juice.h` + `src/client/Juice.cpp`) owns all ephemeral visual state (diff caches, tweened HP, shake trauma, particle pool, damage-number pool) and is updated once per frame from the latest `SnapshotMsg`. `src/client/main.cpp` is modified to construct one instance, call `Update()` each frame, and render its state (camera shake, flash color blend, dot ring, particles, damage numbers) alongside the existing snapshot-driven drawing. No server or protocol changes.

**Tech Stack:** C++17, raylib/raylib-cpp (`Camera2D`, `DrawCircleV`, `DrawText`, `ColorLerp`, `Fade`), existing UDP client/server (untouched).

**Spec:** [docs/superpowers/specs/2026-08-26-client-visual-polish-design.md](../specs/2026-08-26-client-visual-polish-design.md)

## Global Constraints

- No changes to `src/shared/Protocol.h`, any file under `src/server/`, or the wire format.
- No client-side prediction/simulation — juice is presentation only, never feeds back into input or affects what's sent to the server.
- All new effect state lives in `src/client/Juice.h`/`Juice.cpp`; `main.cpp` only calls into it and reads its public getters.
- Particle pool capacity: 128. Damage-number pool capacity: 32. Both fixed-size arrays, no dynamic allocation, silently drop a spawn if the pool is full.
- Constants (exact names/values, all in `Juice.h`):
  - `kHitFlashDuration = 0.15f`
  - `kDamageNumberLifetime = 0.7f`
  - `kDamageNumberRiseSpeed = 40.0f`
  - `kHpDisplaySmoothingRate = 8.0f`
  - `kShakeTraumaPerHit = 0.4f`
  - `kShakeTraumaDecayPerSecond = 1.0f`
  - `kShakeMaxOffsetPixels = 12.0f`
  - `kParticleLifetime = 0.5f`
  - `kParticlesPerBurst = 10`
  - `kReviveRingDotCount = 12`
  - `kMaxParticles = 128`
  - `kMaxDamageNumbers = 32`
- `PlayerSnapshot::state` values: `0 = Alive`, `1 = Downed`, `2 = Dead`, `3 = Absent` (slot not occupied). Never treat an absent slot's stale hp/state as a real diff source.
- Build with `mingw32-make client` (Windows) using the existing Makefile; no Makefile changes needed since `Juice.cpp` under `src/client/` is already picked up by `rwildcard`.

---

### Task 1: Core effects state — diff caches, HP smoothing, shake trauma decay

**Files:**
- Create: `src/client/Juice.h`
- Create: `src/client/Juice.cpp`
- Test: smoke tests added to a new `--test` entry point (see Step 6 below)

**Interfaces:**
- Consumes: `SnapshotMsg`, `PlayerSnapshot`, `WorldItemSnapshot` from `src/shared/Protocol.h` (already built); `Vector2`, `Color` from raylib (via `raylib-cpp.hpp`, already linked into the client target).
- Produces (used by Task 2 and later tasks):
  - `class ClientEffectsState` with public method `void Update(const SnapshotMsg& snap, uint8_t mySlot, float dt);`
  - `float ClientEffectsState::GetDisplayedHp(int slot) const;` (slot is 0 or 1)
  - `float ClientEffectsState::GetHitFlashRatio(int slot) const;` (returns `hitFlashTimer[slot] / kHitFlashDuration`, clamped 0..1)
  - `float ClientEffectsState::GetShakeOffsetX() const;` / `float ClientEffectsState::GetShakeOffsetY() const;`
  - All constants listed in Global Constraints, defined in `Juice.h` at namespace scope (not inside the class) as `constexpr float`/`constexpr int`.

This task builds the skeleton and the three simplest pieces of state (HP smoothing, hit-flash timer decay, shake trauma decay) without particles or damage numbers yet — those come in Tasks 3 and 4. Hit detection (the diff that triggers flash/shake) is wired in this task since flash/shake need it, but particle/damage-number spawning calls are added later (Task 1's `Update` calls two no-op stub methods `SpawnHitEffects(Vector2 pos)` and marks `shakeTrauma`/`hitFlashTimer` directly; Task 3 fills in `SpawnHitEffects` to actually spawn particles+damage number).

- [ ] **Step 1: Write `Juice.h` with class skeleton and constants**

```cpp
#pragma once

#include <cstdint>
#include "raylib-cpp.hpp"
#include "../shared/Protocol.h"

constexpr float kHitFlashDuration = 0.15f;
constexpr float kDamageNumberLifetime = 0.7f;
constexpr float kDamageNumberRiseSpeed = 40.0f;
constexpr float kHpDisplaySmoothingRate = 8.0f;
constexpr float kShakeTraumaPerHit = 0.4f;
constexpr float kShakeTraumaDecayPerSecond = 1.0f;
constexpr float kShakeMaxOffsetPixels = 12.0f;
constexpr float kParticleLifetime = 0.5f;
constexpr int kParticlesPerBurst = 10;
constexpr int kReviveRingDotCount = 12;
constexpr int kMaxParticles = 128;
constexpr int kMaxDamageNumbers = 32;

constexpr uint8_t kSnapshotStateAlive = 0;
constexpr uint8_t kSnapshotStateDowned = 1;
constexpr uint8_t kSnapshotStateDead = 2;
constexpr uint8_t kSnapshotStateAbsent = 3;

struct Particle {
    Vector2 pos{0, 0};
    Vector2 vel{0, 0};
    Color color{255, 255, 255, 255};
    float age = 0.0f;
    float lifetime = 0.0f;
    bool active = false;
};

struct DamageNumber {
    Vector2 pos{0, 0};
    int value = 0;
    float age = 0.0f;
    float lifetime = 0.0f;
    bool active = false;
};

class ClientEffectsState {
public:
    void Update(const SnapshotMsg& snap, uint8_t mySlot, float dt);

    float GetDisplayedHp(int slot) const { return displayedHp[slot]; }
    float GetHitFlashRatio(int slot) const;
    float GetShakeOffsetX() const;
    float GetShakeOffsetY() const;

    const Particle* GetParticles() const { return particles; }
    int GetParticleCount() const { return kMaxParticles; }
    const DamageNumber* GetDamageNumbers() const { return damageNumbers; }
    int GetDamageNumberCount() const { return kMaxDamageNumbers; }

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

    Particle particles[kMaxParticles]{};
    DamageNumber damageNumbers[kMaxDamageNumbers]{};

    void SpawnBurst(Vector2 pos, Color color);
    void SpawnDamageNumber(Vector2 pos, int value);
    void SpawnHitEffects(Vector2 pos, int damage);
};
```

- [ ] **Step 2: Write `Juice.cpp` with `Update`, HP smoothing, hit-flash + shake decay, and diff-driven hit detection**

```cpp
#include "Juice.h"
#include <cstdlib>
#include <cmath>

static float Clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

void ClientEffectsState::Update(const SnapshotMsg& snap, uint8_t mySlot, float dt) {
    for (int i = 0; i < 2; i++) {
        const PlayerSnapshot& p = snap.players[i];
        bool presentNow = (p.state != kSnapshotStateAbsent);
        bool presentBefore = hasPrevFrame && (prevState[i] != kSnapshotStateAbsent);

        if (presentNow && presentBefore) {
            if (p.hp < prevHp[i]) {
                int damage = prevHp[i] - p.hp;
                SpawnHitEffects(Vector2{ p.posX, p.posY }, damage);
                hitFlashTimer[i] = kHitFlashDuration;
                if (i == mySlot) {
                    shakeTrauma = Clamp01(shakeTrauma + kShakeTraumaPerHit);
                }
            }
            if (prevState[i] == kSnapshotStateDowned && p.state == kSnapshotStateAlive) {
                SpawnBurst(Vector2{ p.posX, p.posY }, SKYBLUE);
            }
        }

        if (presentNow) {
            if (!presentBefore) {
                displayedHp[i] = (float)p.hp;
            } else {
                displayedHp[i] += ((float)p.hp - displayedHp[i]) * (dt * kHpDisplaySmoothingRate < 1.0f ? dt * kHpDisplaySmoothingRate : 1.0f);
            }
        }

        hitFlashTimer[i] = hitFlashTimer[i] - dt > 0.0f ? hitFlashTimer[i] - dt : 0.0f;

        prevHp[i] = p.hp;
        prevState[i] = p.state;
        prevPotionCount[i] = p.potionCount;
    }

    for (int j = 0; j < 2; j++) {
        const WorldItemSnapshot& item = snap.items[j];
        bool wasActiveBefore = hasPrevFrame && prevItemActive[j];
        if (wasActiveBefore && !item.active) {
            SpawnBurst(prevItemPos[j], GOLD);
        }
        prevItemActive[j] = item.active;
        prevItemPos[j] = Vector2{ item.posX, item.posY };
    }

    shakeTrauma = shakeTrauma - kShakeTraumaDecayPerSecond * dt > 0.0f
        ? shakeTrauma - kShakeTraumaDecayPerSecond * dt
        : 0.0f;

    for (int i = 0; i < kMaxParticles; i++) {
        if (!particles[i].active) continue;
        particles[i].pos.x += particles[i].vel.x * dt;
        particles[i].pos.y += particles[i].vel.y * dt;
        particles[i].age += dt;
        if (particles[i].age >= particles[i].lifetime) particles[i].active = false;
    }

    for (int i = 0; i < kMaxDamageNumbers; i++) {
        if (!damageNumbers[i].active) continue;
        damageNumbers[i].pos.y -= kDamageNumberRiseSpeed * dt;
        damageNumbers[i].age += dt;
        if (damageNumbers[i].age >= damageNumbers[i].lifetime) damageNumbers[i].active = false;
    }

    hasPrevFrame = true;
}

float ClientEffectsState::GetHitFlashRatio(int slot) const {
    return Clamp01(hitFlashTimer[slot] / kHitFlashDuration);
}

float ClientEffectsState::GetShakeOffsetX() const {
    float magnitude = shakeTrauma * shakeTrauma * kShakeMaxOffsetPixels;
    return ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * magnitude;
}

float ClientEffectsState::GetShakeOffsetY() const {
    float magnitude = shakeTrauma * shakeTrauma * kShakeMaxOffsetPixels;
    return ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * magnitude;
}

void ClientEffectsState::SpawnBurst(Vector2 pos, Color color) {
    int spawned = 0;
    for (int i = 0; i < kMaxParticles && spawned < kParticlesPerBurst; i++) {
        if (particles[i].active) continue;
        float angle = ((float)rand() / (float)RAND_MAX) * 6.2831853f;
        float speed = 40.0f + ((float)rand() / (float)RAND_MAX) * 60.0f;
        particles[i].pos = pos;
        particles[i].vel = Vector2{ cosf(angle) * speed, sinf(angle) * speed };
        particles[i].color = color;
        particles[i].age = 0.0f;
        particles[i].lifetime = kParticleLifetime;
        particles[i].active = true;
        spawned++;
    }
}

void ClientEffectsState::SpawnDamageNumber(Vector2 pos, int value) {
    for (int i = 0; i < kMaxDamageNumbers; i++) {
        if (damageNumbers[i].active) continue;
        damageNumbers[i].pos = pos;
        damageNumbers[i].value = value;
        damageNumbers[i].age = 0.0f;
        damageNumbers[i].lifetime = kDamageNumberLifetime;
        damageNumbers[i].active = true;
        return;
    }
}

void ClientEffectsState::SpawnHitEffects(Vector2 pos, int damage) {
    SpawnBurst(pos, RED);
    SpawnDamageNumber(pos, damage);
}
```

Note: `SpawnHitEffects` is fully implemented here (not a stub) since it only depends on `SpawnBurst`/`SpawnDamageNumber`, which are also written in this task — there's no reason to stub them given the code is this short. This means particles and damage numbers are functionally complete after Task 1; Tasks 3-4 focus on *rendering* them, not spawning logic.

- [ ] **Step 3: Confirm the client still builds**

Run: `mingw32-make client` (ensure `C:\msys64\mingw64\bin` is on PATH first: `export PATH="/c/msys64/mingw64/bin:$PATH"` in bash, or already-configured PATH otherwise).
Expected: builds successfully. `Juice.cpp`/`Juice.h` are not yet included by `main.cpp`, so this just confirms the new files compile standalone — add a temporary `#include "Juice.h"` to `main.cpp` (without instantiating anything) if you want to force compilation of the header via the build; remove it before commit if unused, or leave it if Task 2 will need it immediately (Task 2 does need it, so leaving one `#include "Juice.h"` line in `main.cpp` here is fine and expected).

- [ ] **Step 4: Write smoke tests for the diff/decay/smoothing logic**

Create a small standalone test file `src/client/JuiceTests.h` with a single function `inline void RunJuiceSmokeTests()` using simple `if (...) { printf(...); exit(1); }`-style assertions (mirroring the style used in `src/server/main.cpp`'s `RunAllSmokeTests()`), covering:

```cpp
#pragma once

#include <cstdio>
#include <cstdlib>
#include "Juice.h"

inline void RunJuiceSmokeTests() {
    // Test 1: HP drop triggers damage number + particles, HP rise does not
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 100, 100, 100, kSnapshotStateAlive, 0, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        snap.items[0].active = false;
        snap.items[1].active = false;
        state.Update(snap, 0, 0.016f); // first frame, establishes baseline

        snap.players[0].hp = 70;
        state.Update(snap, 0, 0.016f); // hp dropped 100 -> 70

        int activeDamageNumbers = 0;
        for (int i = 0; i < state.GetDamageNumberCount(); i++) {
            if (state.GetDamageNumbers()[i].active) activeDamageNumbers++;
        }
        if (activeDamageNumbers != 1) {
            printf("FAIL: expected 1 active damage number after hp drop, got %d\n", activeDamageNumbers);
            exit(1);
        }

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles != kParticlesPerBurst) {
            printf("FAIL: expected %d active particles after hit, got %d\n", kParticlesPerBurst, activeParticles);
            exit(1);
        }

        // HP rise should not spawn a new damage number
        snap.players[0].hp = 90;
        state.Update(snap, 0, 0.016f);
        activeDamageNumbers = 0;
        for (int i = 0; i < state.GetDamageNumberCount(); i++) {
            if (state.GetDamageNumbers()[i].active) activeDamageNumbers++;
        }
        if (activeDamageNumbers != 1) {
            printf("FAIL: hp rise should not spawn a new damage number, active count changed to %d\n", activeDamageNumbers);
            exit(1);
        }
        printf("PASS: hit detection spawns damage number + particles, heal does not\n");
    }

    // Test 2: Downed -> Alive triggers a revive burst
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 50, 50, 50, kSnapshotStateDowned, 0, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 1, 0.016f);

        snap.players[0].state = kSnapshotStateAlive;
        snap.players[0].hp = 50; // ReviveFromDowned sets hp, no drop, so no hit-flash path triggers
        state.Update(snap, 1, 0.016f);

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles != kParticlesPerBurst) {
            printf("FAIL: expected %d active particles after revive, got %d\n", kParticlesPerBurst, activeParticles);
            exit(1);
        }
        printf("PASS: downed->alive transition spawns revive burst\n");
    }

    // Test 3: item active->inactive triggers a pickup burst
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0].state = kSnapshotStateAbsent;
        snap.players[1].state = kSnapshotStateAbsent;
        snap.items[0] = WorldItemSnapshot{ 200.0f, 200.0f, true };
        state.Update(snap, 0, 0.016f);

        snap.items[0].active = false;
        state.Update(snap, 0, 0.016f);

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles != kParticlesPerBurst) {
            printf("FAIL: expected %d active particles after pickup, got %d\n", kParticlesPerBurst, activeParticles);
            exit(1);
        }
        printf("PASS: item active->inactive transition spawns pickup burst\n");
    }

    // Test 4: displayedHp converges toward real hp over repeated updates
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 0, 0, 100, kSnapshotStateAlive, 0, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 1, 0.016f);

        snap.players[0].hp = 20;
        for (int i = 0; i < 200; i++) {
            state.Update(snap, 1, 0.016f);
        }
        float displayed = state.GetDisplayedHp(0);
        if (displayed > 20.5f || displayed < 19.5f) {
            printf("FAIL: expected displayedHp to converge to ~20 after 200 updates, got %f\n", displayed);
            exit(1);
        }
        printf("PASS: displayedHp converges toward real hp\n");
    }

    // Test 5: shakeTrauma decays to 0 with no further hits
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 0, 0, 100, kSnapshotStateAlive, 0, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 0, 0.016f);

        snap.players[0].hp = 50; // local player (slot 0) hit -> adds trauma
        state.Update(snap, 0, 0.016f);

        for (int i = 0; i < 200; i++) {
            state.Update(snap, 0, 0.016f); // no further hp change, trauma should decay
        }

        float offsetMagnitudeSquaredProxy = state.GetShakeOffsetX(); // near 0 once trauma is 0
        // GetShakeOffsetX is randomized; instead verify indirectly via repeated calls bounding near 0
        bool allNearZero = true;
        for (int i = 0; i < 20; i++) {
            if (state.GetShakeOffsetX() > 0.01f || state.GetShakeOffsetX() < -0.01f) {
                allNearZero = false;
                break;
            }
        }
        if (!allNearZero) {
            printf("FAIL: expected shakeTrauma (and thus shake offset) to have decayed to ~0\n");
            exit(1);
        }
        printf("PASS: shakeTrauma decays to 0 over time with no further hits\n");
    }

    // Test 6: particle/damage-number pools reuse expired slots and don't exceed capacity
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 0, 0, 100, kSnapshotStateAlive, 0, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 1, 0.016f);

        // Trigger far more hits than pool capacity allows across many frames
        for (int hitNum = 0; hitNum < 20; hitNum++) {
            snap.players[0].hp = snap.players[0].hp > 5 ? snap.players[0].hp - 5 : 100;
            state.Update(snap, 1, 0.016f);
        }

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles > kMaxParticles) {
            printf("FAIL: active particle count %d exceeds pool capacity %d\n", activeParticles, kMaxParticles);
            exit(1);
        }
        printf("PASS: particle pool never exceeds capacity across repeated spawns\n");
    }

    printf("All Juice smoke tests passed.\n");
}
```

- [ ] **Step 5: Wire the test into a runnable entry point**

Modify `src/client/main.cpp` to check for a `--test` argument before doing any windowing/networking setup, mirroring the server's pattern:

```cpp
#include "JuiceTests.h"
```

Add near the top of `main()`, before `NetClient netClient;`:

```cpp
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--test") {
            RunJuiceSmokeTests();
            return 0;
        }
    }
```

- [ ] **Step 6: Build and run the smoke tests**

Run: `mingw32-make client && bin/client --test`
Expected: all six `PASS:` lines print, followed by `All Juice smoke tests passed.`, exit code 0. No window opens (the `--test` branch returns before `raylib::Window` is constructed).

- [ ] **Step 7: Commit**

```bash
git add src/client/Juice.h src/client/Juice.cpp src/client/JuiceTests.h src/client/main.cpp
git commit -m "Add client-side effects state: hit/revive/pickup diffing, HP tween, shake trauma"
```

---

### Task 2: Screen shake camera integration

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `ClientEffectsState::GetShakeOffsetX()`, `GetShakeOffsetY()` (from Task 1, already built).
- Produces: a `Camera2D` variable in `main()`'s loop, recomputed every frame, wrapping the existing world-space draw calls (hazard rectangle, items, players via `drawPlayer`) in `BeginMode2D`/`EndMode2D`. UI text (slot label, F1 hint, debug menu) stays outside the camera block.

- [ ] **Step 1: Instantiate `ClientEffectsState` and call `Update` each frame**

In `main.cpp`, after `DebugMenu debugMenu;` add:

```cpp
    ClientEffectsState effects;
```

After `const SnapshotMsg& snap = netClient.GetLatestSnapshot();` add:

```cpp
        effects.Update(snap, mySlot, dt);
```

- [ ] **Step 2: Wrap world-space drawing in a shaking `Camera2D`**

Replace the block from `BeginDrawing();` / `ClearBackground(RAYWHITE);` through the `drawPlayer(snap.players[1], MAROON, "P2");` calls with:

```cpp
        BeginDrawing();
        ClearBackground(RAYWHITE);

        Camera2D camera{};
        camera.offset = Vector2{ (float)screenWidth / 2.0f + effects.GetShakeOffsetX(), (float)screenHeight / 2.0f + effects.GetShakeOffsetY() };
        camera.target = Vector2{ (float)screenWidth / 2.0f, (float)screenHeight / 2.0f };
        camera.rotation = 0.0f;
        camera.zoom = 1.0f;

        BeginMode2D(camera);

        DrawRectangle((int)snap.hazardX, (int)snap.hazardY, (int)snap.hazardW, (int)snap.hazardH, Fade(RED, 0.3f));

        for (int i = 0; i < 2; i++) {
            if (snap.items[i].active) {
                DrawCircleV(Vector2{ snap.items[i].posX, snap.items[i].posY }, 8, GOLD);
            }
        }

        auto drawPlayer = [&](const PlayerSnapshot& p, Color color, const char* label) {
            if (p.state == 3) return; // absent: slot not occupied by a connected player

            Color drawColor = color;
            if (p.state == 1) drawColor = GRAY;   // Downed
            if (p.state == 2) drawColor = Fade(GRAY, 0.3f); // Dead

            Vector2 pos{ p.posX, p.posY };
            DrawCircleV(pos, 16, drawColor);
            DrawText(label, (int)pos.x - 10, (int)pos.y - 34, 14, BLACK);

            int barWidth = 40;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, barWidth, 5, DARKGRAY);
            int hpWidth = (int)(barWidth * ((float)p.hp / constants.maxHp));
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, hpWidth, 5, GREEN);

            DrawText(TextFormat("Potions: %d", p.potionCount), (int)pos.x - 30, (int)pos.y + 20, 12, DARKBLUE);

            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / constants.channelDuration;
                DrawCircleSector(pos, 24, -90, -90 + 360 * ratio, 32, Fade(SKYBLUE, 0.6f));
            }
        };

        drawPlayer(snap.players[0], BLUE, "P1");
        drawPlayer(snap.players[1], MAROON, "P2");

        EndMode2D();
```

(This step only moves existing code inside `BeginMode2D`/`EndMode2D` and adds the camera — `drawPlayer`'s body is unchanged here; Tasks 3-6 will modify `drawPlayer`'s internals in later steps.)

Note: `screenWidth`/`screenHeight` are already `int` locals declared earlier in `main()` — no new variables needed for them.

- [ ] **Step 3: Verify the rest of `main.cpp` (UI text, debug menu) still renders outside the camera block**

Confirm the lines `DrawText(TextFormat("You are slot %d. ...")`, `DrawText("F1: Debug Menu", ...)`, and `debugMenu.DrawAndHandle(netClient);` remain after `EndMode2D();` and before `EndDrawing();`, unchanged.

- [ ] **Step 4: Build and manually verify**

Run: `mingw32-make client`
Then run one server (`bin/server`) and one client (`bin/client`), move around, and get hit by the hazard zone or a second client's attack — expected: the view visibly jitters briefly on a hit, then settles back to still within under a second. No change to any other visuals yet (no flash/particles/dots/numbers — those are later tasks).

- [ ] **Step 5: Commit**

```bash
git add src/client/main.cpp
git commit -m "Add screen-shake camera driven by ClientEffectsState"
```

---

### Task 3: Hit-flash color blend and tweened HP bars

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `ClientEffectsState::GetHitFlashRatio(int slot)`, `GetDisplayedHp(int slot)` (from Task 1).
- Produces: updated `drawPlayer` lambda that blends color on hit-flash and reads the tweened HP value for the bar. `drawPlayer`'s signature changes to accept a slot index so it can look up per-slot effects state.

- [ ] **Step 1: Change `drawPlayer` to accept a slot index and use it for flash + HP tween**

Replace the `drawPlayer` lambda body (inside the `BeginMode2D`/`EndMode2D` block from Task 2) with:

```cpp
        auto drawPlayer = [&](int slot, const PlayerSnapshot& p, Color color, const char* label) {
            if (p.state == 3) return; // absent: slot not occupied by a connected player

            Color drawColor = color;
            if (p.state == 1) drawColor = GRAY;   // Downed
            if (p.state == 2) drawColor = Fade(GRAY, 0.3f); // Dead

            float flashRatio = effects.GetHitFlashRatio(slot);
            if (flashRatio > 0.0f) {
                drawColor = ColorLerp(drawColor, WHITE, flashRatio);
            }

            Vector2 pos{ p.posX, p.posY };
            DrawCircleV(pos, 16, drawColor);
            DrawText(label, (int)pos.x - 10, (int)pos.y - 34, 14, BLACK);

            int barWidth = 40;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, barWidth, 5, DARKGRAY);
            float displayedHp = effects.GetDisplayedHp(slot);
            int hpWidth = (int)(barWidth * (displayedHp / constants.maxHp));
            if (hpWidth < 0) hpWidth = 0;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, hpWidth, 5, GREEN);

            DrawText(TextFormat("Potions: %d", p.potionCount), (int)pos.x - 30, (int)pos.y + 20, 12, DARKBLUE);

            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / constants.channelDuration;
                DrawCircleSector(pos, 24, -90, -90 + 360 * ratio, 32, Fade(SKYBLUE, 0.6f));
            }
        };

        drawPlayer(0, snap.players[0], BLUE, "P1");
        drawPlayer(1, snap.players[1], MAROON, "P2");
```

(`ColorLerp` is a raylib function available via `raylib-cpp.hpp`, already included. The revive-indicator `DrawCircleSector` call is replaced in Task 6, not here.)

- [ ] **Step 2: Build and manually verify**

Run: `mingw32-make client`
Then run server + client, take damage — expected: the hit player's circle briefly flashes toward white then fades back to its normal color over ~0.15s, and the HP bar visibly eases down over a few frames instead of snapping instantly. Healing (debug menu `HealFull`) should also ease the bar upward.

- [ ] **Step 3: Commit**

```bash
git add src/client/main.cpp
git commit -m "Add hit-flash color blend and tweened HP bar rendering"
```

---

### Task 4: Particle rendering

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `ClientEffectsState::GetParticles()`, `GetParticleCount()` (from Task 1).
- Produces: a draw loop rendering active particles, placed inside the `BeginMode2D`/`EndMode2D` block (world-space, so particles shake with the camera along with everything else) after the two `drawPlayer` calls.

- [ ] **Step 1: Add the particle draw loop**

Inside `main.cpp`, immediately after `drawPlayer(1, snap.players[1], MAROON, "P2");` and before `EndMode2D();`, add:

```cpp
        for (int i = 0; i < effects.GetParticleCount(); i++) {
            const Particle& particle = effects.GetParticles()[i];
            if (!particle.active) continue;
            float alpha = 1.0f - (particle.age / particle.lifetime);
            if (alpha < 0.0f) alpha = 0.0f;
            DrawCircleV(particle.pos, 3.0f, Fade(particle.color, alpha));
        }
```

- [ ] **Step 2: Build and manually verify**

Run: `mingw32-make client`
Then run server + client, take a hit — expected: a small burst of red circles appears at the hit location and fades/spreads outward over about half a second. Pick up a potion — expected: a gold burst at the item's location. Get revived — expected: a sky-blue burst at the revived player.

- [ ] **Step 3: Commit**

```bash
git add src/client/main.cpp
git commit -m "Render particle bursts for hits, pickups, and revives"
```

---

### Task 5: Floating damage numbers

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `ClientEffectsState::GetDamageNumbers()`, `GetDamageNumberCount()` (from Task 1).
- Produces: a draw loop rendering active damage numbers as rising, fading red text, placed inside `BeginMode2D`/`EndMode2D` right after the particle loop (world-space, so numbers shake and stay anchored to world positions like everything else).

- [ ] **Step 1: Add the damage-number draw loop**

Immediately after the particle loop from Task 4 (still before `EndMode2D();`), add:

```cpp
        for (int i = 0; i < effects.GetDamageNumberCount(); i++) {
            const DamageNumber& dmgNum = effects.GetDamageNumbers()[i];
            if (!dmgNum.active) continue;
            float alpha = 1.0f - (dmgNum.age / dmgNum.lifetime);
            if (alpha < 0.0f) alpha = 0.0f;
            const char* text = TextFormat("-%d", dmgNum.value);
            Color textColor = Fade(RED, alpha);
            DrawText(text, (int)dmgNum.pos.x - 6, (int)dmgNum.pos.y - 10, 16, textColor);
        }
```

(`DrawText` accepts a `Color` directly, and `Fade` produces a `Color` with adjusted alpha — no need for `DrawTextEx`; raylib's `DrawText` respects the alpha channel of the passed color.)

- [ ] **Step 2: Build and manually verify**

Run: `mingw32-make client`
Then run server + client, take damage — expected: a red "-N" number appears at the hit location, drifts upward, and fades out over about 0.7s, where N matches the actual HP lost.

- [ ] **Step 3: Commit**

```bash
git add src/client/main.cpp
git commit -m "Render floating damage numbers on hit"
```

---

### Task 6: Dot-ring revive indicator

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `kReviveRingDotCount` constant (from Task 1, `Juice.h`).
- Produces: replaces the `DrawCircleSector`-based revive indicator inside `drawPlayer` with a ring of `kReviveRingDotCount` dots that fill in one-by-one as `channelTimer` progresses.

- [ ] **Step 1: Replace the revive-sector block inside `drawPlayer`**

Within the `drawPlayer` lambda (modified in Task 3), replace:

```cpp
            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / constants.channelDuration;
                DrawCircleSector(pos, 24, -90, -90 + 360 * ratio, 32, Fade(SKYBLUE, 0.6f));
            }
```

with:

```cpp
            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / constants.channelDuration;
                int filledDots = (int)(kReviveRingDotCount * ratio);
                if (filledDots > kReviveRingDotCount) filledDots = kReviveRingDotCount;
                float ringRadius = 24.0f;
                for (int dot = 0; dot < kReviveRingDotCount; dot++) {
                    float angle = (-90.0f + 360.0f * (float)dot / (float)kReviveRingDotCount) * (3.14159265f / 180.0f);
                    Vector2 dotPos{ pos.x + cosf(angle) * ringRadius, pos.y + sinf(angle) * ringRadius };
                    if (dot < filledDots) {
                        DrawCircleV(dotPos, 3.0f, Fade(SKYBLUE, 0.9f));
                    } else {
                        DrawCircleLines((int)dotPos.x, (int)dotPos.y, 3.0f, Fade(SKYBLUE, 0.4f));
                    }
                }
            }
```

(Requires `#include <cmath>` in `main.cpp` for `cosf`/`sinf` — add it near the top alongside the existing includes if not already present via a transitive include.)

- [ ] **Step 2: Build and manually verify**

Run: `mingw32-make client`
Then run server + 2 clients, down one player, and have the other hold `E` near them to channel a revive — expected: a ring of 12 dots appears around the downed player, filling in (solid) one-by-one clockwise from the top as the channel progresses, with unfilled dots shown as hollow outlines. All 12 should be filled right as the revive completes.

- [ ] **Step 3: Commit**

```bash
git add src/client/main.cpp
git commit -m "Replace revive progress sector with a fill-in dot ring"
```

---

## Final Verification

After all six tasks:

- [ ] Run `mingw32-make client && bin/client --test` — all smoke tests pass.
- [ ] Run `bin/server`, then two `bin/client` instances; play through: movement, a PvP hit (shake + flash + damage number + particles on the hit player), hazard-zone damage (same effects, no shake since it's not necessarily the local player... note: hazard damage to the local player *does* shake, since shake triggers on any HP drop to `mySlot` regardless of cause), a full item pickup (gold particle burst), a full revive channel (dot ring fills in, sky-blue burst on completion), and death/respawn (existing Dead-state fade still works, unaffected by this plan).
- [ ] Confirm the F1 debug menu still opens/functions correctly (Task 2's camera change should not affect it, since it renders outside `BeginMode2D`/`EndMode2D`).
