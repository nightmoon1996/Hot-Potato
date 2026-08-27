#pragma once

#include <array>
#include "../shared/Geometry.h"
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
    static const int kCapacity = 4;

    // count == 0 means the slot is empty (its `type` value is then meaningless — do not
    // read it). This mirrors the previous vector-based code's own convention, where a slot
    // reaching count == 0 was immediately erased ("gone"); here it just stays in place as an
    // empty fixed slot instead of being removed and shifting later slots.
    Inventory() {
        for (auto& slot : slots) {
            slot.type = ItemType::RevivePotion; // arbitrary; count == 0 makes this unread
            slot.count = 0;
        }
    }

    bool Add(ItemType type) {
        for (auto& slot : slots) {
            if (slot.count > 0 && slot.type == type) {
                slot.count += 1;
                return true;
            }
        }
        for (auto& slot : slots) {
            if (slot.count == 0) {
                slot.type = type;
                slot.count = 1;
                return true;
            }
        }
        return false; // no matching stack and no empty slot
    }

    bool Remove(ItemType type, int amount = 1) {
        for (auto& slot : slots) {
            if (slot.count > 0 && slot.type == type) {
                if (slot.count < amount) {
                    return false;
                }
                slot.count -= amount;
                return true; // slot stays in place at count==0 (now empty), not erased
            }
        }
        return false;
    }

    int Count(ItemType type) const {
        for (const auto& slot : slots) {
            if (slot.count > 0 && slot.type == type) {
                return slot.count;
            }
        }
        return 0;
    }

    // Fixed hotbar-index access: returns the slot at `index` (0..kCapacity-1), count==0 if
    // empty. Used by the hotbar UI (via the snapshot) and by the server's revive/self-heal
    // gating (via the player's actual Inventory) to answer "what does the CURRENTLY SELECTED
    // slot hold". Asserts index is in range — callers must clamp/validate selectedSlot first
    // (a later task does this), since an out-of-range hotbar index from a malformed/malicious
    // client packet must never reach here un-clamped.
    const InventorySlot& SlotAt(int index) const {
        return slots[index];
    }

    const std::array<InventorySlot, kCapacity>& Slots() const {
        return slots;
    }

private:
    std::array<InventorySlot, kCapacity> slots;
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
