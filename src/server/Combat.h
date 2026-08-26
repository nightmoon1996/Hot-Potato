#pragma once

#include <raylib-cpp.hpp>
#include <cmath>
#include "Player.h"

inline float DistanceBetween(Vector2 a, Vector2 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

inline bool TryAttack(Player& attacker, Player& target) {
    if (attacker.attackCooldownTimer > 0.0f) {
        return false;
    }
    if (attacker.state != PlayerState::Alive || target.state != PlayerState::Alive) {
        return false;
    }
    if (DistanceBetween(attacker.position, target.position) > Player::kAttackRange) {
        return false;
    }
    target.TakeDamage(Player::kAttackDamage);
    attacker.attackCooldownTimer = Player::kAttackCooldown;
    return true;
}

inline void UpdateAttackCooldown(Player& player, float dt) {
    player.attackCooldownTimer -= dt;
    if (player.attackCooldownTimer < 0.0f) {
        player.attackCooldownTimer = 0.0f;
    }
}

inline bool UpdateRevive(Player& reviver, Player& target, bool interactHeld, float dt, float reviveRange) {
    bool canProgress =
        reviver.state == PlayerState::Alive &&
        target.state == PlayerState::Downed &&
        reviver.inventory.Count(ItemType::RevivePotion) >= 1 &&
        DistanceBetween(reviver.position, target.position) <= reviveRange &&
        interactHeld;

    if (!canProgress) {
        reviver.channelTimer = 0.0f;
        return false;
    }

    reviver.channelTimer += dt;
    if (reviver.channelTimer >= Player::kChannelDuration) {
        reviver.inventory.Remove(ItemType::RevivePotion, 1);
        target.ReviveFromDowned();
        reviver.channelTimer = 0.0f;
        return true;
    }
    return false;
}
