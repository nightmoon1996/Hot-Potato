#pragma once

#include <vector>
#include <raylib-cpp.hpp>
#include <cmath>

enum class ItemType {
    RevivePotion
};

struct InventorySlot {
    ItemType type;
    int count;
};

class Inventory {
public:
    static const int kCapacity = 8;

    bool Add(ItemType type) {
        for (auto& slot : slots) {
            if (slot.type == type) {
                slot.count += 1;
                return true;
            }
        }
        if ((int)slots.size() >= kCapacity) {
            return false;
        }
        slots.push_back({type, 1});
        return true;
    }

    bool Remove(ItemType type, int amount = 1) {
        for (size_t i = 0; i < slots.size(); i++) {
            if (slots[i].type == type) {
                if (slots[i].count < amount) {
                    return false;
                }
                slots[i].count -= amount;
                if (slots[i].count == 0) {
                    slots.erase(slots.begin() + i);
                }
                return true;
            }
        }
        return false;
    }

    int Count(ItemType type) const {
        for (const auto& slot : slots) {
            if (slot.type == type) {
                return slot.count;
            }
        }
        return 0;
    }

    const std::vector<InventorySlot>& Slots() const {
        return slots;
    }

private:
    std::vector<InventorySlot> slots;
};

struct WorldItem {
    Vector2 position;
    ItemType type;
    bool active;
};

inline bool TryPickup(WorldItem& item, Vector2 playerPos, Inventory& inventory, float pickupRadius) {
    if (!item.active) {
        return false;
    }
    float dx = playerPos.x - item.position.x;
    float dy = playerPos.y - item.position.y;
    float distance = std::sqrt(dx * dx + dy * dy);
    if (distance > pickupRadius) {
        return false;
    }
    if (!inventory.Add(item.type)) {
        return false;
    }
    item.active = false;
    return true;
}
