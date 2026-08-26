#pragma once

#include "../shared/Geometry.h"
#include "Item.h"
#include <utility>

enum class PlayerState {
    Alive,
    Downed,
    Dead
};

class Player {
public:
    static constexpr int kMaxHp = 100;
    static constexpr float kDownedDuration = 15.0f;
    static constexpr float kDeathRespawnDelay = 5.0f;
    static constexpr int kReviveHp = 50;
    static constexpr int kRespawnHp = 100;
    static constexpr float kAttackCooldown = 0.8f;
    static constexpr float kAttackRange = 40.0f;
    static constexpr int kAttackDamage = 15;
    static constexpr float kChannelDuration = 2.0f;
    static constexpr float kDashDuration = 0.2f; // how long a dash's travel is visibly spread over,
                                                  // rather than snapping to the destination instantly

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

    // Dash-in-progress state: while dashElapsed < kDashDuration, position is being
    // interpolated from dashStartPos to dashTargetPos tick-by-tick (see AdvanceDash) instead
    // of having already snapped there. dashElapsed >= kDashDuration means no dash is active.
    Vector2 dashStartPos{ 0.0f, 0.0f };
    Vector2 dashTargetPos{ 0.0f, 0.0f };
    float dashElapsed = kDashDuration;

    explicit Player(Vector2 spawn)
        : position(spawn), spawnPoint(spawn), hp(kMaxHp), state(PlayerState::Alive),
          downedTimer(0.0f), deathRespawnTimer(0.0f), attackCooldownTimer(0.0f), channelTimer(0.0f),
          dashCooldownTimer(0.0f), facingDirection(Vector2{1.0f, 0.0f}) {}

    bool IsDashing() const { return dashElapsed < kDashDuration; }

    // Starts a new dash: travel from the player's CURRENT position to `targetPos`, spread
    // over kDashDuration. Overwrites any dash already in progress (TryApplyDash only calls
    // this when the cooldown has elapsed, so a new dash never legitimately overlaps one
    // still finishing).
    void StartDash(Vector2 targetPos) {
        dashStartPos = position;
        dashTargetPos = targetPos;
        dashElapsed = 0.0f;
    }

    // Advances an in-progress dash by dt, moving `position` toward dashTargetPos. Returns
    // the sub-segment (from, to) actually traveled this call, for catch-sweep checks at the
    // call site — {position, position} (a zero-length segment) when no dash is in progress.
    // Snaps exactly to dashTargetPos on the tick that completes the dash, so float
    // accumulation across many small steps can never leave the player short of the intended
    // destination.
    std::pair<Vector2, Vector2> AdvanceDash(float dt) {
        Vector2 from = position;
        if (!IsDashing()) return { from, from };

        dashElapsed += dt;
        if (dashElapsed >= kDashDuration) {
            position = dashTargetPos;
        } else {
            float t = dashElapsed / kDashDuration;
            position = Vector2{
                dashStartPos.x + (dashTargetPos.x - dashStartPos.x) * t,
                dashStartPos.y + (dashTargetPos.y - dashStartPos.y) * t
            };
        }
        return { from, position };
    }

    void TakeDamage(int amount) {
        if (state != PlayerState::Alive) {
            return;
        }
        hp -= amount;
        if (hp <= 0) {
            hp = 0;
            state = PlayerState::Downed;
            downedTimer = kDownedDuration;
        }
    }

    void Kill() {
        state = PlayerState::Dead;
        hp = 0;
        deathRespawnTimer = kDeathRespawnDelay;
    }

    void ReviveFromDowned() {
        if (state != PlayerState::Downed) {
            return;
        }
        state = PlayerState::Alive;
        hp = kReviveHp;
        downedTimer = 0.0f;
        channelTimer = 0.0f;
    }

    void ForceDown() {
        if (state != PlayerState::Alive) {
            return;
        }
        state = PlayerState::Downed;
        hp = 0;
        downedTimer = kDownedDuration;
    }

    void RespawnFull() {
        state = PlayerState::Alive;
        hp = kRespawnHp;
        position = spawnPoint;
        downedTimer = 0.0f;
        deathRespawnTimer = 0.0f;
    }

    void UpdateTimers(float dt) {
        if (state == PlayerState::Downed) {
            downedTimer -= dt;
            if (downedTimer <= 0.0f) {
                Kill();
            }
        } else if (state == PlayerState::Dead) {
            deathRespawnTimer -= dt;
            if (deathRespawnTimer <= 0.0f) {
                RespawnFull();
            }
        }
    }
};
