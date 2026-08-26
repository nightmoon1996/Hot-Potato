#pragma once

#include "../shared/Geometry.h"
#include "Player.h"
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

// Clamps a position to stay inside courtBounds. Shared by dash and ordinary movement so
// both agree on where the arena ends (otherwise dash would act as a "return to arena"
// button for a player who had walked outside).
inline Vector2 ClampToCourtBounds(Vector2 pos, Rectangle courtBounds) {
    if (pos.x < courtBounds.x) pos.x = courtBounds.x;
    if (pos.x > courtBounds.x + courtBounds.width) pos.x = courtBounds.x + courtBounds.width;
    if (pos.y < courtBounds.y) pos.y = courtBounds.y;
    if (pos.y > courtBounds.y + courtBounds.height) pos.y = courtBounds.y + courtBounds.height;
    return pos;
}

// Computes the dash's destination position, clamped to stay inside courtBounds.
inline Vector2 ComputeDashDestination(Vector2 currentPosition, Vector2 dashDirection, Rectangle courtBounds) {
    return ClampToCourtBounds(
        Vector2{ currentPosition.x + dashDirection.x * kDashDistance, currentPosition.y + dashDirection.y * kDashDistance },
        courtBounds);
}

// The single production implementation of "apply one input packet's dash attempt".
// Called by main()'s Input-message-handling block and directly by smoke tests.
//
// Always updates facingDirection when real movement input is present (that happens whether
// or not a dash fires, since it's what a stationary dash falls back to) — but ONLY for an
// Alive player. Returns true iff a dash was actually applied; when it returns false the
// player's position and dashCooldownTimer are left untouched.
inline bool TryApplyDash(Player& player, Vector2 moveInput, bool dashPressed, Rectangle courtBounds) {
    if (player.state != PlayerState::Alive) return false;

    float moveLen = std::sqrt(moveInput.x * moveInput.x + moveInput.y * moveInput.y);
    if (moveLen > 0.0001f) {
        player.facingDirection = Vector2{ moveInput.x / moveLen, moveInput.y / moveLen };
    }

    if (!dashPressed || player.dashCooldownTimer > 0.0f) return false;

    Vector2 dashDir = ResolveDashDirection(moveInput, player.facingDirection);
    player.position = ComputeDashDestination(player.position, dashDir, courtBounds);
    player.dashCooldownTimer = kDashCooldown;
    return true;
}
