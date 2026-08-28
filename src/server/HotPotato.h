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
    bool justThrown = false; // grace flag: excludes the thrower from catch checks until the potato first clears kCatchRadius of the thrower's position post-release, avoiding an instant self-catch while the thrower hasn't moved away yet
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

// Minimum distance from `point` to the line SEGMENT from `segStart` to `segEnd` (not the
// infinite line through them). Used by the dash-time catch check: a dash teleports the
// player kDashDistance(150) in one step, which is 7.5x the kCatchRadius(20) catch diameter,
// so a point-sample of only the post-dash position can miss a potato the dash swept through.
inline float DistancePointToSegment(Vector2 point, Vector2 segStart, Vector2 segEnd) {
    Vector2 segVec{ segEnd.x - segStart.x, segEnd.y - segStart.y };
    float segLenSq = segVec.x * segVec.x + segVec.y * segVec.y;
    if (segLenSq < 0.0001f) {
        // Degenerate segment (start == end): just measure to the single point.
        return DistanceBetween2(point, segStart);
    }
    Vector2 toPoint{ point.x - segStart.x, point.y - segStart.y };
    float t = (toPoint.x * segVec.x + toPoint.y * segVec.y) / segLenSq;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    Vector2 closest{ segStart.x + segVec.x * t, segStart.y + segVec.y * t };
    return DistanceBetween2(point, closest);
}

// True if the swept dash segment from `dashStart` to `dashEnd` passes within kCatchRadius
// of the potato's current position, i.e. the dashing player should catch it.
inline bool DashSegmentCatchesPotato(Vector2 dashStart, Vector2 dashEnd, Vector2 potatoPos) {
    return DistancePointToSegment(potatoPos, dashStart, dashEnd) <= kCatchRadius;
}

// Applies the catch resolution to `potato` for `catcherSlot` at `catcherPos`. This is the
// single source of truth for "someone caught the potato": mirrored by both the tick loop's
// FindCatchTarget path and the dash-time swept-segment path.
inline void ResolveCatch(HotPotato& potato, int catcherSlot, Vector2 catcherPos) {
    potato.held = true;
    potato.inFlight = false;
    potato.holderSlot = catcherSlot;
    potato.velocity = Vector2{0.0f, 0.0f};
    potato.position = catcherPos;
    potato.catchCount += 1;
    potato.explodeTimer = ComputeExplodeTimerForCatch(potato.catchCount);
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
