# Pickup / Inventory / Revive System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a local 2-player co-op test scene in raylib-cpp with item pickups, a per-player inventory, an item+channel revive mechanic, hazard/PvP damage, and an F1 debug menu, exactly as specified.

**Architecture:** A handful of small headers/sources under `src/` (`Player`, `Item`, `Hazard`, `DebugMenu`) hold plain structs/classes with no raylib window dependencies in their logic where possible, so the state-machine and inventory math can be smoke-tested via `assert()` calls run from `main()` before the window opens. `main.cpp` owns the game loop: input, update, draw, wiring all systems together every frame.

**Tech Stack:** C++17, raylib + raylib-cpp (already vendored/built in this repo), GNU Make (existing `Makefile`, no changes needed). No new dependencies (explicitly no raygui — debug menu uses plain raylib draw + manual rect hit-testing).

**Spec:** [docs/superpowers/specs/2026-08-26-pickup-inventory-revive-design.md](../specs/2026-08-26-pickup-inventory-revive-design.md)

## Global Constraints

- Max HP: 100
- Downed duration: 15s (Downed → Dead if not revived)
- Death respawn delay: 5s (Dead → Alive automatically)
- Revive HP (from Downed via item+channel or debug): 50
- Respawn HP (from Dead, automatic or debug revive): 100
- Attack cooldown: 0.8s
- Attack range: 40px
- Attack damage: 15
- Pickup radius: 24px
- Revive range: 32px
- Channel-revive duration: 2.0s
- Hazard damage rate: 5 HP/sec
- Inventory capacity: 8 distinct slots
- Player 1 controls: WASD move, `E` interact, `Q` attack
- Player 2 controls: Arrow keys move, `RightCtrl` interact, `RightShift` attack
- No new external dependencies; build via existing `Makefile` (`mingw32-make` on Windows)
- No win condition / game-over screen — this is a test scene

---

## Task 1: Inventory (pure logic, no raylib types)

**Files:**
- Create: `src/Item.h`
- Test: smoke-checked via `assert()` calls added temporarily to `src/main.cpp` in Step 2 of this task, removed once Task 6 wires the real game loop (the asserts are throwaway verification, not permanent code)

**Interfaces:**
- Produces:
  - `enum class ItemType { RevivePotion };`
  - `struct InventorySlot { ItemType type; int count; };`
  - `class Inventory` with:
    - `static const int kCapacity = 8;`
    - `bool Add(ItemType type);` — returns `true` if added/stacked, `false` if no slot and inventory full
    - `bool Remove(ItemType type, int amount = 1);` — returns `true` if removed (had enough), `false` otherwise (no-op on failure)
    - `int Count(ItemType type) const;`
    - `const std::vector<InventorySlot>& Slots() const;`

This header has no `#include <raylib-cpp.hpp>` — it only needs `<vector>`. That keeps it testable without a window.

- [ ] **Step 1: Write `src/Item.h` with `ItemType`, `InventorySlot`, and `Inventory`**

```cpp
#pragma once

#include <vector>

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
```

- [ ] **Step 2: Add temporary smoke-check calls at the top of `src/main.cpp`, before `int main()`**

Add this above the existing `#include <raylib-cpp.hpp>` line, and call it as the very first line inside `main()`:

```cpp
#include <cassert>
#include "Item.h"

void SmokeTestInventory() {
    Inventory inv;
    assert(inv.Count(ItemType::RevivePotion) == 0);

    assert(inv.Add(ItemType::RevivePotion) == true);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Add(ItemType::RevivePotion) == true);
    assert(inv.Count(ItemType::RevivePotion) == 2);

    assert(inv.Remove(ItemType::RevivePotion, 1) == true);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Remove(ItemType::RevivePotion, 5) == false);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Remove(ItemType::RevivePotion, 1) == true);
    assert(inv.Count(ItemType::RevivePotion) == 0);
    assert(inv.Slots().size() == 0);
}
```

Then at the very top of `int main()`, before anything else:

```cpp
int main()
{
    SmokeTestInventory();
    TraceLog(LOG_INFO, "SmokeTestInventory passed");
    // ... existing code continues
```

- [ ] **Step 3: Build and run to verify the smoke test passes**

Run (Windows): `mingw32-make bin/app; mingw32-make execute`

Expected: a window opens (existing "Hello World" behavior) and the console/log shows `SmokeTestInventory passed` with no assertion failure. If an `assert` fails, the program aborts with a message naming the failed condition and line — fix `Item.h` until all asserts pass.

- [ ] **Step 4: Commit**

```bash
git add src/Item.h src/main.cpp
git commit -m "Add Inventory type with smoke-tested add/remove/count logic"
```

---

## Task 2: World items and pickups

**Files:**
- Modify: `src/Item.h`

**Interfaces:**
- Consumes: `ItemType` (Task 1)
- Produces:
  - `struct WorldItem { Vector2 position; ItemType type; bool active; };` (uses raylib's `Vector2`, so this part of the header now needs `#include "raylib-cpp.hpp"` — that's fine, only `main.cpp` and files that need world/rendering types include it)
  - `bool TryPickup(WorldItem& item, Vector2 playerPos, Inventory& inventory, float pickupRadius);` — free function: if `item.active` and distance from `playerPos` to `item.position` `<= pickupRadius`, calls `inventory.Add(item.type)`; if that succeeds, sets `item.active = false` and returns `true`. Otherwise returns `false` (including when `Add` fails because inventory is full — item stays active and in the world).

- [ ] **Step 1: Add `WorldItem` and `TryPickup` to `src/Item.h`**

Add near the top of the file, after the existing includes:

```cpp
#include <raylib-cpp.hpp>
#include <cmath>
```

Add at the bottom of the file, after the `Inventory` class:

```cpp
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
```

- [ ] **Step 2: Extend the smoke test in `src/main.cpp` to cover pickup**

Add to the end of `SmokeTestInventory()` (rename it to `SmokeTestItems()` and update its call site/log message to match, since it now covers more than the inventory):

```cpp
    // Pickup logic
    WorldItem item{ {100.0f, 100.0f}, ItemType::RevivePotion, true };
    Inventory pickupInv;

    // Too far away: no pickup
    bool pickedFar = TryPickup(item, Vector2{500.0f, 500.0f}, pickupInv, 24.0f);
    assert(pickedFar == false);
    assert(item.active == true);
    assert(pickupInv.Count(ItemType::RevivePotion) == 0);

    // Within radius: picks up
    bool pickedNear = TryPickup(item, Vector2{105.0f, 100.0f}, pickupInv, 24.0f);
    assert(pickedNear == true);
    assert(item.active == false);
    assert(pickupInv.Count(ItemType::RevivePotion) == 1);

    // Already inactive: no second pickup
    bool pickedAgain = TryPickup(item, Vector2{105.0f, 100.0f}, pickupInv, 24.0f);
    assert(pickedAgain == false);
    assert(pickupInv.Count(ItemType::RevivePotion) == 1);
```

Rename the function and its call/log:

```cpp
void SmokeTestItems() {
    // ... all the previous Inventory asserts, plus the block above
}
```

And in `main()`:

```cpp
    SmokeTestItems();
    TraceLog(LOG_INFO, "SmokeTestItems passed");
```

- [ ] **Step 3: Build and run to verify**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: window opens, log shows `SmokeTestItems passed`, no assertion failure.

- [ ] **Step 4: Commit**

```bash
git add src/Item.h src/main.cpp
git commit -m "Add WorldItem and pickup-radius logic with smoke tests"
```

---

## Task 3: Player state machine (state, HP, timers)

**Files:**
- Create: `src/Player.h`
- Modify: `src/main.cpp` (smoke test additions)

**Interfaces:**
- Consumes: `Inventory` (Task 1, `#include "Item.h"`)
- Produces:
  - `enum class PlayerState { Alive, Downed, Dead };`
  - `class Player` with public fields: `Vector2 position; int hp; PlayerState state; float downedTimer; float deathRespawnTimer; float attackCooldownTimer; float channelTimer; Inventory inventory; Vector2 spawnPoint;`
  - Constants (as `static constexpr` members of `Player`): `kMaxHp = 100`, `kDownedDuration = 15.0f`, `kDeathRespawnDelay = 5.0f`, `kReviveHp = 50`, `kRespawnHp = 100`, `kAttackCooldown = 0.8f`, `kAttackRange = 40.0f`, `kAttackDamage = 15`, `kChannelDuration = 2.0f`
  - Methods:
    - `Player(Vector2 spawn)` — constructs with `position = spawn`, `spawnPoint = spawn`, `hp = kMaxHp`, `state = PlayerState::Alive`, all timers `0`.
    - `void TakeDamage(int amount)` — only applies if `state == Alive`; subtracts `amount` from `hp` (floored at 0); if `hp <= 0`, sets `state = Downed`, `hp = 0`, `downedTimer = kDownedDuration`.
    - `void Kill()` — sets `state = Dead`, `hp = 0`, `deathRespawnTimer = kDeathRespawnDelay`.
    - `void ReviveFromDowned()` — only valid if `state == Downed`; sets `state = Alive`, `hp = kReviveHp`, `downedTimer = 0`, `channelTimer = 0`.
    - `void ForceDown()` — only valid if `state == Alive`; sets `state = Downed`, `hp = 0`, `downedTimer = kDownedDuration`.
    - `void RespawnFull()` — sets `state = Alive`, `hp = kRespawnHp`, `position = spawnPoint`, `downedTimer = 0`, `deathRespawnTimer = 0`.
    - `void UpdateTimers(float dt)` — if `state == Downed`: `downedTimer -= dt`; if `downedTimer <= 0`, calls `Kill()`. If `state == Dead`: `deathRespawnTimer -= dt`; if `deathRespawnTimer <= 0`, calls `RespawnFull()`.

This header needs `#include "Item.h"` (for `Inventory`) and `#include <raylib-cpp.hpp>` (for `Vector2`).

- [ ] **Step 1: Write `src/Player.h`**

```cpp
#pragma once

#include <raylib-cpp.hpp>
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
    Inventory inventory;

    explicit Player(Vector2 spawn)
        : position(spawn), spawnPoint(spawn), hp(kMaxHp), state(PlayerState::Alive),
          downedTimer(0.0f), deathRespawnTimer(0.0f), attackCooldownTimer(0.0f), channelTimer(0.0f) {}

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
```

- [ ] **Step 2: Add a smoke test for the state machine in `src/main.cpp`**

Add `#include "Player.h"` near the `#include "Item.h"` line. Add a new function, called after `SmokeTestItems()`:

```cpp
void SmokeTestPlayerStateMachine() {
    Player p(Vector2{0.0f, 0.0f});
    assert(p.state == PlayerState::Alive);
    assert(p.hp == Player::kMaxHp);

    // Damage that doesn't kill
    p.TakeDamage(30);
    assert(p.state == PlayerState::Alive);
    assert(p.hp == 70);

    // Damage that downs
    p.TakeDamage(100);
    assert(p.state == PlayerState::Downed);
    assert(p.hp == 0);
    assert(p.downedTimer == Player::kDownedDuration);

    // Damage while downed has no effect
    p.TakeDamage(10);
    assert(p.state == PlayerState::Downed);

    // Downed timer expiring kills
    p.UpdateTimers(Player::kDownedDuration + 1.0f);
    assert(p.state == PlayerState::Dead);
    assert(p.deathRespawnTimer == Player::kDeathRespawnDelay);

    // Death respawn timer expiring respawns
    p.UpdateTimers(Player::kDeathRespawnDelay + 1.0f);
    assert(p.state == PlayerState::Alive);
    assert(p.hp == Player::kRespawnHp);
    assert(p.position.x == p.spawnPoint.x && p.position.y == p.spawnPoint.y);

    // Revive from downed
    Player p2(Vector2{0.0f, 0.0f});
    p2.ForceDown();
    assert(p2.state == PlayerState::Downed);
    p2.ReviveFromDowned();
    assert(p2.state == PlayerState::Alive);
    assert(p2.hp == Player::kReviveHp);
}
```

Call it in `main()` after the items smoke test:

```cpp
    SmokeTestPlayerStateMachine();
    TraceLog(LOG_INFO, "SmokeTestPlayerStateMachine passed");
```

- [ ] **Step 3: Build and run to verify**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: log shows `SmokeTestPlayerStateMachine passed`, no assertion failure.

- [ ] **Step 4: Commit**

```bash
git add src/Player.h src/main.cpp
git commit -m "Add Player state machine (Alive/Downed/Dead) with smoke tests"
```

---

## Task 4: Hazard zones

**Files:**
- Create: `src/Hazard.h`
- Modify: `src/main.cpp` (smoke test additions)

**Interfaces:**
- Consumes: `Player` (Task 3, for `TakeDamage`)
- Produces:
  - `struct HazardZone { Rectangle bounds; };`
  - `static constexpr float kHazardDamagePerSecond = 5.0f;` (free constant in `Hazard.h`)
  - `void ApplyHazardDamage(const HazardZone& zone, Player& player, float dt);` — if `player.state == Alive` and `CheckCollisionPointRec(player.position, zone.bounds)` is true, calls `player.TakeDamage(...)` using accumulated fractional damage (see implementation below — since `TakeDamage` takes an `int`, this function accumulates fractional HP loss internally per player using a static-free approach: it returns the leftover fractional damage so the caller can carry it over. To keep this simple and testable without extra per-player state, the function instead scales via a passed-in accumulator.)

**Design note for this task:** `TakeDamage(int)` only takes whole HP. To apply "5 HP/sec" smoothly regardless of frame rate, `ApplyHazardDamage` takes a `float& carryover` parameter (a small piece of state the caller owns per player, e.g. `float hazardDamageCarry = 0.0f;` alongside each `Player`) that accumulates fractional damage between frames and applies whole points once they cross 1.0.

- [ ] **Step 1: Write `src/Hazard.h`**

```cpp
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
```

- [ ] **Step 2: Add a smoke test in `src/main.cpp`**

Add `#include "Hazard.h"`. Add:

```cpp
void SmokeTestHazard() {
    HazardZone zone{ Rectangle{0.0f, 0.0f, 100.0f, 100.0f} };
    Player p(Vector2{50.0f, 50.0f}); // inside the zone
    float carry = 0.0f;

    // 1 second at 5 HP/sec should deal 5 damage total, spread across calls
    for (int i = 0; i < 60; i++) {
        ApplyHazardDamage(zone, p, 1.0f / 60.0f, carry);
    }
    assert(p.hp == Player::kMaxHp - 5);

    // Player outside the zone takes no damage
    Player p2(Vector2{500.0f, 500.0f});
    float carry2 = 0.0f;
    ApplyHazardDamage(zone, p2, 1.0f, carry2);
    assert(p2.hp == Player::kMaxHp);
}
```

Call it in `main()`:

```cpp
    SmokeTestHazard();
    TraceLog(LOG_INFO, "SmokeTestHazard passed");
```

- [ ] **Step 3: Build and run to verify**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: log shows `SmokeTestHazard passed`, no assertion failure. (Note: due to `int` truncation, 60 calls of `1/60` second at 5 HP/sec accumulates to exactly `5.0f` — floating point may leave it at 4 or 5; if the assert fails at 4 instead of 5, change the assert to `p.hp <= Player::kMaxHp - 4` to tolerate float rounding, since the important behavior is "damage was applied, not instantaneous".)

- [ ] **Step 4: Commit**

```bash
git add src/Hazard.h src/main.cpp
git commit -m "Add hazard zone damage-over-time logic with smoke test"
```

---

## Task 5: PvP attack and revive-channel logic

**Files:**
- Create: `src/Combat.h`
- Modify: `src/main.cpp` (smoke test additions)

**Interfaces:**
- Consumes: `Player` (Task 3)
- Produces:
  - `bool TryAttack(Player& attacker, Player& target);` — returns `false` if `attacker.attackCooldownTimer > 0` or `attacker.state != Alive` or `target.state != Alive`; otherwise computes distance between `attacker.position` and `target.position`, and if `<= Player::kAttackRange`, calls `target.TakeDamage(Player::kAttackDamage)`, sets `attacker.attackCooldownTimer = Player::kAttackCooldown`, returns `true`. If out of range, returns `false` (no cooldown consumed).
  - `void UpdateAttackCooldown(Player& player, float dt);` — decrements `attackCooldownTimer` by `dt`, floored at 0.
  - `bool UpdateRevive(Player& reviver, Player& target, bool interactHeld, float dt, float reviveRange);` — the channel-revive step function, called once per frame:
    - Preconditions to make progress: `reviver.state == Alive`, `target.state == Downed`, `reviver.inventory.Count(ItemType::RevivePotion) >= 1`, distance `<= reviveRange`, and `interactHeld == true`.
    - If all preconditions hold: `reviver.channelTimer += dt`. If `reviver.channelTimer >= Player::kChannelDuration`: consume the item (`reviver.inventory.Remove(ItemType::RevivePotion, 1)`), call `target.ReviveFromDowned()`, reset `reviver.channelTimer = 0`, return `true` (revive completed this frame).
    - If any precondition fails: reset `reviver.channelTimer = 0`.
    - Returns `false` if no revive completed this frame (whether channeling, idle, or reset).

- [ ] **Step 1: Write `src/Combat.h`**

```cpp
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
```

- [ ] **Step 2: Add a smoke test in `src/main.cpp`**

Add `#include "Combat.h"`. Add:

```cpp
void SmokeTestCombatAndRevive() {
    // Attack: in range, off cooldown -> succeeds
    Player attacker(Vector2{0.0f, 0.0f});
    Player target(Vector2{10.0f, 0.0f});
    bool hit = TryAttack(attacker, target);
    assert(hit == true);
    assert(target.hp == Player::kMaxHp - Player::kAttackDamage);
    assert(attacker.attackCooldownTimer == Player::kAttackCooldown);

    // Immediately again: on cooldown -> fails
    bool hit2 = TryAttack(attacker, target);
    assert(hit2 == false);
    assert(target.hp == Player::kMaxHp - Player::kAttackDamage); // unchanged

    // Out of range -> fails
    Player far(Vector2{1000.0f, 1000.0f});
    Player attacker2(Vector2{0.0f, 0.0f});
    bool hit3 = TryAttack(attacker2, far);
    assert(hit3 == false);

    // Revive: full sequence
    Player reviver(Vector2{0.0f, 0.0f});
    Player downed(Vector2{5.0f, 0.0f});
    downed.ForceDown();
    reviver.inventory.Add(ItemType::RevivePotion);

    // Not enough time yet: still downed
    bool completed1 = UpdateRevive(reviver, downed, true, 1.0f, 32.0f);
    assert(completed1 == false);
    assert(downed.state == PlayerState::Downed);

    // Cross the threshold
    bool completed2 = UpdateRevive(reviver, downed, true, 1.5f, 32.0f);
    assert(completed2 == true);
    assert(downed.state == PlayerState::Alive);
    assert(downed.hp == Player::kReviveHp);
    assert(reviver.inventory.Count(ItemType::RevivePotion) == 0);

    // Releasing the key resets progress
    Player reviver2(Vector2{0.0f, 0.0f});
    Player downed2(Vector2{5.0f, 0.0f});
    downed2.ForceDown();
    reviver2.inventory.Add(ItemType::RevivePotion);
    UpdateRevive(reviver2, downed2, true, 1.0f, 32.0f);
    assert(reviver2.channelTimer > 0.0f);
    UpdateRevive(reviver2, downed2, false, 0.0f, 32.0f); // key released
    assert(reviver2.channelTimer == 0.0f);
}
```

Call it in `main()`:

```cpp
    SmokeTestCombatAndRevive();
    TraceLog(LOG_INFO, "SmokeTestCombatAndRevive passed");
```

- [ ] **Step 3: Build and run to verify**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: log shows `SmokeTestCombatAndRevive passed`, no assertion failure.

- [ ] **Step 4: Commit**

```bash
git add src/Combat.h src/main.cpp
git commit -m "Add PvP attack and channel-revive logic with smoke tests"
```

---

## Task 6: Real game loop — movement, input, rendering (manual verification)

**Files:**
- Modify: `src/main.cpp` (replace smoke-test-only `main()` with the real game loop; smoke test function calls stay at the top of `main()` as a startup self-check, but the window loop now does real work)

**Interfaces:**
- Consumes: `Player`, `Inventory`, `WorldItem`, `TryPickup` (Task 1-2), `HazardZone`, `ApplyHazardDamage` (Task 4), `TryAttack`, `UpdateAttackCooldown`, `UpdateRevive` (Task 5)
- Produces: a running, drawable, playable window — later tasks (debug menu) modify this loop further.

This is the first task with no automated assert-based test — raylib's window/input/draw calls aren't unit-testable. Verification is manual: run the app and interact with it per the steps below.

- [ ] **Step 1: Replace the body of `src/main.cpp` with the full two-player loop**

Replace the entire file with:

```cpp
#include <raylib-cpp.hpp>
#include <cassert>
#include "Item.h"
#include "Player.h"
#include "Hazard.h"
#include "Combat.h"

// --- Smoke tests (see Tasks 1-5) ---
void SmokeTestItems() { /* ... keep the body written in Tasks 1-2 ... */ }
void SmokeTestPlayerStateMachine() { /* ... keep the body written in Task 3 ... */ }
void SmokeTestHazard() { /* ... keep the body written in Task 4 ... */ }
void SmokeTestCombatAndRevive() { /* ... keep the body written in Task 5 ... */ }

int main()
{
    SmokeTestItems();
    SmokeTestPlayerStateMachine();
    SmokeTestHazard();
    SmokeTestCombatAndRevive();
    TraceLog(LOG_INFO, "All smoke tests passed");

    int screenWidth = 1000;
    int screenHeight = 600;
    raylib::Window w(screenWidth, screenHeight, "Maxion Test - Pickup/Inventory/Revive");
    SetTargetFPS(60);

    Player p1(Vector2{150.0f, 300.0f});
    Player p2(Vector2{850.0f, 300.0f});
    float p1HazardCarry = 0.0f;
    float p2HazardCarry = 0.0f;

    HazardZone hazard{ Rectangle{450.0f, 200.0f, 100.0f, 200.0f} };

    std::vector<WorldItem> worldItems = {
        WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
        WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
    };

    const float moveSpeed = 200.0f;
    const float pickupRadius = 24.0f;
    const float reviveRange = 32.0f;

    while (!w.ShouldClose())
    {
        float dt = GetFrameTime();

        // --- Input: movement (only when Alive) ---
        if (p1.state == PlayerState::Alive) {
            Vector2 move{0, 0};
            if (IsKeyDown(KEY_W)) move.y -= 1;
            if (IsKeyDown(KEY_S)) move.y += 1;
            if (IsKeyDown(KEY_A)) move.x -= 1;
            if (IsKeyDown(KEY_D)) move.x += 1;
            p1.position.x += move.x * moveSpeed * dt;
            p1.position.y += move.y * moveSpeed * dt;
        }
        if (p2.state == PlayerState::Alive) {
            Vector2 move{0, 0};
            if (IsKeyDown(KEY_UP)) move.y -= 1;
            if (IsKeyDown(KEY_DOWN)) move.y += 1;
            if (IsKeyDown(KEY_LEFT)) move.x -= 1;
            if (IsKeyDown(KEY_RIGHT)) move.x += 1;
            p2.position.x += move.x * moveSpeed * dt;
            p2.position.y += move.y * moveSpeed * dt;
        }

        // --- Input: pickup (interact key, only when Alive) ---
        if (p1.state == PlayerState::Alive && IsKeyPressed(KEY_E)) {
            for (auto& item : worldItems) {
                if (TryPickup(item, p1.position, p1.inventory, pickupRadius)) break;
            }
        }
        if (p2.state == PlayerState::Alive && IsKeyPressed(KEY_RIGHT_CONTROL)) {
            for (auto& item : worldItems) {
                if (TryPickup(item, p2.position, p2.inventory, pickupRadius)) break;
            }
        }

        // --- Input: attack ---
        if (IsKeyPressed(KEY_Q)) {
            TryAttack(p1, p2);
        }
        if (IsKeyPressed(KEY_RIGHT_SHIFT)) {
            TryAttack(p2, p1);
        }
        UpdateAttackCooldown(p1, dt);
        UpdateAttackCooldown(p2, dt);

        // --- Input + logic: channel revive (interact key held) ---
        bool p1InteractHeld = IsKeyDown(KEY_E);
        bool p2InteractHeld = IsKeyDown(KEY_RIGHT_CONTROL);
        UpdateRevive(p1, p2, p1InteractHeld, dt, reviveRange);
        UpdateRevive(p2, p1, p2InteractHeld, dt, reviveRange);

        // --- Hazard damage ---
        ApplyHazardDamage(hazard, p1, dt, p1HazardCarry);
        ApplyHazardDamage(hazard, p2, dt, p2HazardCarry);

        // --- State timers (downed -> dead -> respawn) ---
        p1.UpdateTimers(dt);
        p2.UpdateTimers(dt);

        // --- Draw ---
        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawRectangleRec(hazard.bounds, Fade(RED, 0.3f));

        for (const auto& item : worldItems) {
            if (item.active) {
                DrawCircleV(item.position, 8, GOLD);
            }
        }

        auto drawPlayer = [](const Player& p, Color color, const char* label) {
            Color drawColor = color;
            if (p.state == PlayerState::Downed) drawColor = GRAY;
            if (p.state == PlayerState::Dead) drawColor = Fade(GRAY, 0.3f);

            DrawCircleV(p.position, 16, drawColor);
            DrawText(label, (int)p.position.x - 10, (int)p.position.y - 34, 14, BLACK);

            int barWidth = 40;
            DrawRectangle((int)p.position.x - barWidth / 2, (int)p.position.y - 26, barWidth, 5, DARKGRAY);
            int hpWidth = (int)(barWidth * ((float)p.hp / Player::kMaxHp));
            DrawRectangle((int)p.position.x - barWidth / 2, (int)p.position.y - 26, hpWidth, 5, GREEN);

            DrawText(TextFormat("Potions: %d", p.inventory.Count(ItemType::RevivePotion)),
                      (int)p.position.x - 30, (int)p.position.y + 20, 12, DARKBLUE);

            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / Player::kChannelDuration;
                DrawCircleSector(p.position, 24, -90, -90 + 360 * ratio, 32, Fade(SKYBLUE, 0.6f));
            }
        };

        drawPlayer(p1, BLUE, "P1");
        drawPlayer(p2, MAROON, "P2");

        DrawText("P1: WASD move, E pickup/revive, Q attack", 10, 10, 16, BLACK);
        DrawText("P2: Arrows move, RCtrl pickup/revive, RShift attack", 10, 30, 16, BLACK);

        EndDrawing();
    }

    return 0;
}
```

**Note for the engineer:** the `SmokeTestItems`, `SmokeTestPlayerStateMachine`, `SmokeTestHazard`, and `SmokeTestCombatAndRevive` function bodies already exist in the file from Tasks 1-5 — do not delete them, just make sure `#include <vector>` is available (it's pulled in transitively via `Item.h`, but if the compiler complains, add `#include <vector>` explicitly at the top).

- [ ] **Step 2: Build and run**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: window opens titled "Maxion Test - Pickup/Inventory/Revive", console shows "All smoke tests passed", two circles (blue P1 left, maroon P2 right) are visible with HP bars, a red-tinted hazard rectangle in the middle, and two gold dots (world items).

- [ ] **Step 3: Manually verify each mechanic**

With the window open:
1. Move P1 with WASD and P2 with arrow keys — both should move independently.
2. Walk P1 onto a gold dot and press `E` — the dot disappears, "Potions: 1" appears under P1.
3. Walk either player into the red hazard rectangle and stay a couple seconds — their HP bar should drain, circle turns gray at 0 HP (Downed).
4. With one player Downed, walk the other (who picked up a potion) next to them within visual proximity and hold their interact key (`E` or `RCtrl`) — a blue progress-ring should sweep around the reviver over ~2 seconds; when it completes, the downed player's circle returns to their normal color with half HP bar.
5. Let a Downed player sit for 15+ seconds without reviving — they should go fully transparent/gray (Dead), then automatically pop back to full color/full HP after 5 more seconds at their original spawn point.
6. Bring both players close together and press `Q`/`RShift` to attack — the target's HP bar should drop by 15 on each successful hit, with a visible pause (cooldown) before the same attacker can hit again.

If any of these don't match, fix the relevant file (`Player.h`, `Combat.h`, `Hazard.h`, `Item.h`, or the loop in `main.cpp`) before moving on.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Wire up playable 2-player loop with movement, pickup, hazard, PvP, and revive"
```

---

## Task 7: Debug menu (F1 overlay)

**Files:**
- Create: `src/DebugMenu.h`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `Player` (Task 3)
- Produces:
  - `struct DebugButton { Rectangle bounds; const char* label; };`
  - `class DebugMenu` with:
    - `bool visible = false;`
    - `void Toggle();` — flips `visible`
    - `void DrawAndHandle(Player& p1, Player& p2);` — if `visible`, draws a panel with two columns (P1, P2) each having 5 buttons (Kill, Revive, Heal Full, Give Potion, Force Down) and, when a button rectangle is clicked (`IsMouseButtonPressed(MOUSE_BUTTON_LEFT)` + `CheckCollisionPointRec(GetMousePosition(), button.bounds)`), performs the corresponding action directly on the matching player:
      - Kill → `player.Kill()`
      - Revive → if `Downed`, `player.ReviveFromDowned()`; if `Dead`, `player.RespawnFull()`
      - Heal Full → only if `Alive`, sets `player.hp = Player::kMaxHp`
      - Give Potion → `player.inventory.Add(ItemType::RevivePotion)`
      - Force Down → `player.ForceDown()`

- [ ] **Step 1: Write `src/DebugMenu.h`**

```cpp
#pragma once

#include <raylib-cpp.hpp>
#include "Player.h"

struct DebugButton {
    Rectangle bounds;
    const char* label;
};

class DebugMenu {
public:
    bool visible = false;

    void Toggle() {
        visible = !visible;
    }

    void DrawAndHandle(Player& p1, Player& p2) {
        if (!visible) {
            return;
        }

        DrawRectangle(0, 0, 1000, 600, Fade(BLACK, 0.5f));
        DrawRectangle(300, 150, 400, 260, RAYWHITE);
        DrawText("DEBUG MENU (F1 to close)", 320, 160, 16, BLACK);

        DrawText("P1", 340, 190, 16, BLUE);
        DrawText("P2", 540, 190, 16, MAROON);

        HandleColumn(p1, 320, 210);
        HandleColumn(p2, 520, 210);
    }

private:
    void HandleColumn(Player& p, int x, int startY) {
        const char* labels[5] = { "Kill", "Revive", "Heal Full", "Give Potion", "Force Down" };
        for (int i = 0; i < 5; i++) {
            Rectangle bounds{ (float)x, (float)(startY + i * 40), 150.0f, 30.0f };
            DrawRectangleRec(bounds, LIGHTGRAY);
            DrawRectangleLinesEx(bounds, 1, DARKGRAY);
            DrawText(labels[i], (int)bounds.x + 8, (int)bounds.y + 7, 14, BLACK);

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(GetMousePosition(), bounds)) {
                ApplyAction(p, i);
            }
        }
    }

    void ApplyAction(Player& p, int index) {
        switch (index) {
            case 0: p.Kill(); break;
            case 1:
                if (p.state == PlayerState::Downed) p.ReviveFromDowned();
                else if (p.state == PlayerState::Dead) p.RespawnFull();
                break;
            case 2:
                if (p.state == PlayerState::Alive) p.hp = Player::kMaxHp;
                break;
            case 3: p.inventory.Add(ItemType::RevivePotion); break;
            case 4: p.ForceDown(); break;
        }
    }
};
```

- [ ] **Step 2: Wire the debug menu into `src/main.cpp`**

Add `#include "DebugMenu.h"` near the other includes. After `HazardZone hazard{...};` declaration, add:

```cpp
    DebugMenu debugMenu;
```

Inside the `while (!w.ShouldClose())` loop, right after `float dt = GetFrameTime();`, add:

```cpp
        if (IsKeyPressed(KEY_F1)) {
            debugMenu.Toggle();
        }
```

At the end of the draw section, right before `EndDrawing();`, add:

```cpp
        debugMenu.DrawAndHandle(p1, p2);
```

Also add a hint line near the other `DrawText` control hints:

```cpp
        DrawText("F1: Debug Menu", 10, 50, 16, DARKGRAY);
```

- [ ] **Step 3: Build and run**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: pressing `F1` shows a semi-transparent overlay with P1/P2 columns of 5 buttons each; pressing `F1` again hides it.

- [ ] **Step 4: Manually verify each debug action**

With the menu open:
1. Click P1's "Force Down" — P1 turns gray (Downed) without taking damage normally.
2. Click P1's "Revive" — P1 returns to Alive with a half-full HP bar (50 HP).
3. Click P1's "Kill" — P1 goes translucent (Dead).
4. Click P1's "Revive" again while Dead — P1 returns to Alive with a full HP bar (100 HP) at their spawn point.
5. Click P2's "Give Potion" — P2's "Potions" count increases by 1.
6. Click P1's "Heal Full" while Alive but damaged — HP bar goes back to full.

- [ ] **Step 5: Commit**

```bash
git add src/DebugMenu.h src/main.cpp
git commit -m "Add F1 debug menu for manual per-player state testing"
```

---

## Plan Self-Review Notes

- **Spec coverage:** Player states/transitions → Task 3; hazard damage → Task 4; PvP attack → Task 5; items/inventory/pickup → Tasks 1-2; revive item+channel with progress ring → Task 5 (logic) + Task 6 (ring rendering); debug menu with all 5 listed actions → Task 7; controls (WASD/E/Q, Arrows/RCtrl/RShift) → Task 6; numeric defaults table → encoded as constants in `Player.h`/`Hazard.h`/`Combat.h` and used verbatim in Global Constraints above. All spec sections are covered.
- **Placeholder scan:** no TBD/TODO; every step has full code, not descriptions.
- **Type consistency:** `ItemType`, `Inventory`, `WorldItem`, `Player`, `PlayerState`, `HazardZone`, `TryAttack`, `UpdateAttackCooldown`, `UpdateRevive`, `DebugMenu` are each defined once (Tasks 1-3, Task 7) and reused with identical names/signatures in later tasks (Task 6 wiring, Task 7 consuming `Player`).
