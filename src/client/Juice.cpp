#include "Juice.h"
#include <cstdlib>
#include <cmath>

static float Clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

void ClientEffectsState::Update(const SnapshotMsg& snap, uint8_t mySlot, float dt) {
    for (int i = 0; i < 4; i++) {
        const PlayerSnapshot& p = snap.players[i];
        bool presentNow = (p.state != kSnapshotStateAbsent);
        bool presentBefore = hasPrevFrame && (prevState[i] != kSnapshotStateAbsent);

        // An absent slot's snapshot fields (hp, state, pos) are stale/meaningless, so any
        // transition involving absence must be skipped rather than diffed as a real change.
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
    // Squaring trauma is the standard shake curve: small hits barely shake, repeated/big hits shake hard.
    float magnitude = shakeTrauma * shakeTrauma * kShakeMaxOffsetPixels;
    return ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * magnitude;
}

float ClientEffectsState::GetShakeOffsetY() const {
    // Squaring trauma is the standard shake curve: small hits barely shake, repeated/big hits shake hard.
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
