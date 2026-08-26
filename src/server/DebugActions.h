#pragma once
#include "Player.h"
#include "../shared/Protocol.h"

inline void ApplyDebugAction(Player& target, DebugAction action) {
    switch (action) {
        case DebugAction::Kill:
            target.Kill();
            break;
        case DebugAction::Revive:
            if (target.state == PlayerState::Downed) target.ReviveFromDowned();
            else if (target.state == PlayerState::Dead) target.RespawnFull();
            break;
        case DebugAction::HealFull:
            if (target.state == PlayerState::Alive) target.hp = Player::kMaxHp;
            break;
        case DebugAction::GivePotion:
            target.inventory.Add(ItemType::RevivePotion);
            break;
        case DebugAction::ForceDown:
            target.ForceDown();
            break;
    }
}
