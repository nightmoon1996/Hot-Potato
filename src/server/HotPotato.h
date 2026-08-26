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
