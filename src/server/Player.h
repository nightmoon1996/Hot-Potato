#pragma once

#include "../shared/Geometry.h"
#include "Item.h"

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
