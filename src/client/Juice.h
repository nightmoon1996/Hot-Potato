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
constexpr float kReviveRingRadius = 24.0f;
constexpr float kReviveRingDotRadius = 3.0f;
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
