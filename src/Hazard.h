#pragma once

#include <raylib-cpp.hpp>
#include "Player.h"

static constexpr float kHazardDamagePerSecond = 5.0f;

struct HazardZone {
    Rectangle bounds;
};

inline void ApplyHazardDamage(const HazardZone& zone, Player& player, float dt, float& carryover) {
    if (player.state != PlayerState::Alive) {
        return;
    }
    if (!CheckCollisionPointRec(player.position, zone.bounds)) {
        return;
    }
    carryover += kHazardDamagePerSecond * dt;
    int wholeDamage = (int)carryover;
    if (wholeDamage > 0) {
        player.TakeDamage(wholeDamage);
        carryover -= wholeDamage;
    }
}
