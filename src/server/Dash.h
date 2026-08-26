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
