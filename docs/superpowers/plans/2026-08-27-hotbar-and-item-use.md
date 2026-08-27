# Hotbar and Manual Item Use Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the player a 4-slot hotbar (keys 1-4 select a slot), rendered on the client HUD, and make the RevivePotion usable two ways from the same E key: unchanged proximity-based auto-channel-revive of a nearby Downed teammate (now additionally gated on having the potion slot selected), or — when no one is revivable — an instant self-heal on a fresh press of E, consuming one potion.

**Architecture:** `Inventory` shrinks from a dynamic, auto-stacking `std::vector<InventorySlot>` (capacity 8) to a fixed `slots[4]` array (one item type per fixed index, still stacking counts within a slot) — this is what "hotbar slot" means server-side: index == hotbar position. `Player` gains a `selectedSlot` (0-3), set from a new `InputMsg::selectedSlot` field the client sends whenever keys 1-4 are pressed. `InputMsg` also gains `usePressed` — an edge-triggered ("just pressed," not "held") signal parallel to the existing `attackPressed` pattern (`IsKeyPressed`, not `IsKeyDown`) — sent alongside the unchanged, still-held `interactHeld`. The tick loop's existing revive-channel logic gains one extra gate (selected slot must hold RevivePotion); a new, separate self-heal check fires on `usePressed` when revive doesn't apply. `PlayerSnapshot` replaces its single scalar `potionCount` with a small fixed `HotbarSlotSnapshot slots[4]` (item type + count per slot) so the client can render real hotbar contents instead of one aggregate number. Client gets a new `Hotbar.h` module: draws 4 boxes with icon/count/selection-highlight, and turns 1-4 key presses into the outgoing `selectedSlot`.

**Tech Stack:** C++17, existing UDP client/server protocol (additive + one field replacement), raylib/raylib-cpp for the new hotbar UI.

**Spec:** No separate spec document — this is a bounded feature built directly on the existing `Item.h`/`Combat.h`/`Player.h`/`SimulateSessionTick` inventory and revive code, approved via an in-chat design discussion (recorded here): 4 hotbar slots (matches keys 1-4, shrinking `Inventory::kCapacity` from 8); RevivePotion self-heals 30 HP (capped at `Player::kMaxHp`) when used with no one revivable nearby, instantly (no channel); pickup (E, held, unchanged) is NOT gated by hotbar selection — only the revive-channel and self-heal uses are; revive-channel keeps its existing 2s channel duration and mechanics, just gated on slot selection additionally.

## Global Constraints

- `Inventory::kCapacity` changes from 8 to 4 — this is the hotbar size. No UI or logic anywhere should assume more than 4 slots.
- Item pickup (`TryPickup`, driven by `interactHeld` in the tick loop's pickup block) is completely UNCHANGED — no gating on `selectedSlot` added there. Only the revive-channel logic (`UpdateRevive`'s call site) gains the new "is RevivePotion the selected slot's item" gate.
- Self-heal amount: 30 HP flat, added via a new `Player` method that clamps to `kMaxHp` (mirrors the existing clamping pattern already used elsewhere in `Player.h`, e.g. `TakeDamage`'s `hp <= 0` clamp).
- Self-heal is INSTANT (no channel), fires on `usePressed` (edge-triggered — "just pressed this tick," not "held") — never re-fires on repeated ticks while E is held, since `usePressed` is derived from the client's `IsKeyPressed` (one-shot per physical press), matching the existing precedent set by `attackPressed`'s `IsKeyPressed(KEY_Q)` in `src/client/main.cpp`, not a new edge-detection mechanism invented on the server.
- Revive-channel's existing per-tick loop structure, timer semantics, and the specific "reviver channels against at most one target, breaks on first progress, explicit reset otherwise" behavior (see the large comment above that loop in `main.cpp`) must NOT be altered beyond adding the one new slot-selection gate — this logic has a documented history of subtle bugs in this project and should be touched minimally.
- Protocol changes: `InputMsg` gains two new fields (`selectedSlot`, `usePressed`), appended at the end. `PlayerSnapshot::potionCount` (a scalar) is REMOVED and replaced with `HotbarSlotSnapshot slots[4]` (a new struct, 4-element array) — this is a breaking change to that one field, acceptable since client and server ship together in this project (no separate versioned rollout requirement).
- Builds with `mingw32-make server` / `mingw32-make client` using the existing Makefile.
- Toolchain: MinGW at `C:\msys64\mingw64\bin` — add to PATH if needed: `export PATH="/c/msys64/mingw64/bin:$PATH"`. If a rebuild fails with a linker "Permission denied," a previous `server.exe`/`client.exe` may be running — check `tasklist //FI "IMAGENAME eq server.exe"` / `client.exe` and ASK before killing (do not force-kill without asking).

---

### Task 1: Fixed-size 4-slot `Inventory` + protocol plumbing

**Files:**
- Modify: `src/server/Item.h`
- Modify: `src/shared/Protocol.h`

**Interfaces:**
- Produces: `Inventory::kCapacity = 4`; `Inventory` internally uses a fixed `std::array<InventorySlot, 4>` (or equivalent fixed storage) instead of `std::vector<InventorySlot>`, with an explicit "empty" marker per slot (an `ItemType` alone can't represent "no item," since `ItemType::RevivePotion` is a valid value — add a `bool occupied` to `InventorySlot`, or a `count == 0` convention; use `count == 0` meaning empty, since `Add`/`Remove` already treat a slot reaching `count == 0` as "gone" in the current vector-based code, so this preserves that exact semantic with the least behavior change). `Inventory::SlotAt(int index) const` (new: returns the `InventorySlot` at a fixed hotbar index, `count == 0` if empty — this is what the hotbar UI and the new revive/self-heal gating both read). `Inventory::Add`/`Remove`/`Count` keep their existing signatures and observable behavior for existing callers (pickup, revive-consume) — only the STORAGE changes from dynamic/compacting to fixed-index; `Add` now needs to either find an existing slot with the same type (as today) or claim the first `count == 0` slot (replacing "push_back" with "write into the first empty fixed slot"), and `Remove` clearing a slot to `count == 0` in place rather than erasing/shifting the vector. `InputMsg` gains `int selectedSlot;` (client's currently-selected hotbar index, 0-3) and `bool usePressed;` (edge-triggered "just pressed use this tick"), both appended at the end. `PlayerSnapshot` REMOVES `int potionCount;` and adds `HotbarSlotSnapshot slots[4];` (new struct: `uint8_t itemType; int count;` — `count == 0` means empty, mirroring `InventorySlot`'s own convention) as its last field.

- [ ] **Step 1: Rewrite `Inventory` to use fixed 4-slot storage**

In `src/server/Item.h`, replace the whole `Inventory` class body. Current:

```cpp
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

Replace with:

```cpp
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
    // (see Task 3), since an out-of-range hotbar index from a malformed/malicious client
    // packet must never reach here un-clamped.
    const InventorySlot& SlotAt(int index) const {
        return slots[index];
    }

    const std::array<InventorySlot, kCapacity>& Slots() const {
        return slots;
    }

private:
    std::array<InventorySlot, kCapacity> slots;
};
```

Add `#include <array>` to `Item.h`'s includes (alongside the existing `<vector>` — `<vector>` can stay or go; `WorldItem`/other code in this file doesn't otherwise need it, check whether anything else in `Item.h` still uses `std::vector` before removing the include, since `TryPickup` and `WorldItem` are unaffected by this change and might not need it — if nothing else in the file uses `std::vector`, remove the now-unnecessary `#include <vector>`).

- [ ] **Step 2: Add the two new `InputMsg` fields**

In `src/shared/Protocol.h`, find:

```cpp
struct InputMsg {
    float moveX;
    float moveY;
    bool interactHeld;
    bool attackPressed;
    bool chargingThrow;
    bool releaseThrow;
    float aimDirX;
    float aimDirY;
    bool dashPressed;
};
```

replace with:

```cpp
struct InputMsg {
    float moveX;
    float moveY;
    bool interactHeld;
    bool attackPressed;
    bool chargingThrow;
    bool releaseThrow;
    float aimDirX;
    float aimDirY;
    bool dashPressed;
    int selectedSlot;  // currently-selected hotbar index, 0..3 (set by keys 1-4 client-side)
    bool usePressed;   // edge-triggered "use" — true only on the tick E was FRESHLY pressed,
                       // mirroring attackPressed's IsKeyPressed-based one-shot semantics, NOT
                       // interactHeld's held/repeating semantics. Drives instant self-heal;
                       // never drives the (still interactHeld-gated) pickup or revive-channel.
};
```

- [ ] **Step 3: Replace `PlayerSnapshot::potionCount` with a 4-slot hotbar snapshot**

In `src/shared/Protocol.h`, add a new struct just above `PlayerSnapshot`:

```cpp
struct HotbarSlotSnapshot {
    uint8_t itemType; // meaningless when count == 0 (empty slot)
    int count;
};
```

Find:

```cpp
struct PlayerSnapshot {
    float posX;
    float posY;
    int hp;
    uint8_t state; // 0 = Alive, 1 = Downed, 2 = Dead
    int potionCount;
    float channelTimer;
};
```

replace with:

```cpp
struct PlayerSnapshot {
    float posX;
    float posY;
    int hp;
    uint8_t state; // 0 = Alive, 1 = Downed, 2 = Dead
    HotbarSlotSnapshot slots[4]; // fixed 4-slot hotbar contents, index-for-index with Inventory
    float channelTimer;
};
```

- [ ] **Step 4: Build**

Run: `mingw32-make server && mingw32-make client`

Expected: this will NOT yet build cleanly — `src/server/main.cpp` (the `.potionCount` snapshot-population call site) and `src/client/main.cpp`/`Juice.cpp` (the `.potionCount` read sites) still reference the now-removed field. That's expected and intentional: Task 2 fixes the server-side population site, Task 4 fixes the client-side read sites. Confirm the ONLY compile errors are about `potionCount` not being a member (in `main.cpp` on both server and client, and in `Juice.cpp`) — if there are OTHER unrelated errors, investigate before proceeding.

- [ ] **Step 5: Commit**

```bash
git add src/server/Item.h src/shared/Protocol.h
git commit -m "Shrink Inventory to a fixed 4-slot hotbar; add selectedSlot/usePressed to InputMsg"
```

(This commit intentionally leaves the build red — Tasks 2 and 4 fix the two call sites. Note this clearly in the commit message body if you want, or leave it — the next tasks fix it immediately after.)

---

### Task 2: Server tick logic — slot-gated revive-channel, instant self-heal, snapshot population

**Files:**
- Modify: `src/server/Player.h`
- Modify: `src/server/Combat.h`
- Modify: `src/server/main.cpp`

**Interfaces:**
- Consumes: `Inventory::SlotAt` (Task 1), `InputMsg::selectedSlot`/`usePressed` (Task 1), `HotbarSlotSnapshot` (Task 1).
- Produces: `Player::selectedSlot` (new field, clamped 0..3, updated every tick from input); `Player::TryHeal(int amount)` (new method, clamps to `kMaxHp`); `UpdateRevive`'s call site in `SimulateSessionTick` gains a "selected slot holds RevivePotion" gate; a new self-heal check fires on `usePressed` when revive isn't applicable; the snapshot-building code populates `PlayerSnapshot::slots[4]` from the player's actual `Inventory` instead of the removed `potionCount`.

- [ ] **Step 1: Add `selectedSlot` and `TryHeal` to `Player`**

In `src/server/Player.h`, add a field and a method. Add to the class body (alongside the other simple fields like `dashCooldownTimer`):

```cpp
    int selectedSlot = 0; // clamped 0..(Inventory::kCapacity-1); updated from InputMsg::selectedSlot each tick
```

Add a method (alongside `TakeDamage`/`Kill`/etc.):

```cpp
    // Instant self-heal (NOT gated on PlayerState here — callers gate on Alive themselves,
    // matching TakeDamage's own pattern of leaving state-gating to callers... actually,
    // TakeDamage self-gates on Alive internally; mirror that exactly for consistency: only
    // an Alive player can be healed, silently no-op otherwise (a Downed/Dead player has no
    // meaningful "current HP" to add to in a way that matters, and gameplay-wise neither
    // state should be self-healable via this path).
    void TryHeal(int amount) {
        if (state != PlayerState::Alive) return;
        hp += amount;
        if (hp > kMaxHp) hp = kMaxHp;
    }
```

- [ ] **Step 2: Gate `UpdateRevive`'s call site on the selected slot holding RevivePotion**

Read the CURRENT exact revive-loop code in `src/server/main.cpp` (search for `UpdateRevive` — it's inside a loop with specific break/reset semantics described by a large comment directly above it; read that comment and the loop body in full before editing, since this logic has a documented history of subtle correctness bugs in this project and must be touched minimally). The call currently looks like:

```cpp
                UpdateRevive(session.slots[i].player, session.slots[j].player, interact[i], dt, kReviveRange);
```

Do NOT change `UpdateRevive`'s own signature or internals (in `Combat.h`) — instead, gate the call site itself: only call `UpdateRevive` (i.e., only let reviver `i` make channel progress against target `j`) when reviver `i`'s currently-selected slot holds at least one RevivePotion. Change the call to something like:

```cpp
                bool potionSelected = session.slots[i].player.inventory.SlotAt(session.slots[i].player.selectedSlot).count > 0 &&
                                      session.slots[i].player.inventory.SlotAt(session.slots[i].player.selectedSlot).type == ItemType::RevivePotion;
                UpdateRevive(session.slots[i].player, session.slots[j].player, interact[i] && potionSelected, dt, kReviveRange);
```

This preserves every existing line of `UpdateRevive` and the loop's break/reset structure exactly as-is — the ONLY change is that the boolean passed in as "is interact held" now ALSO requires the potion slot to be selected, so a reviver with a different slot selected gets `false` here (identical to "not holding E"), which `UpdateRevive` already handles correctly today (resets `channelTimer` to 0, as the existing comment describes). Read the actual surrounding loop variable names (`i`, `j`, or whatever they really are) and match them exactly — do not assume the sketch above is verbatim correct against the real code.

- [ ] **Step 3: Add the instant self-heal check**

Immediately after the revive loop (same location/scope, still inside `SimulateSessionTick`, after the loop from Step 2 but you may place it either right after that loop or integrated into it — your judgment on the cleanest placement that doesn't disturb the existing loop's structure; a SEPARATE small loop immediately after is the safer, more isolated choice given the existing loop's documented fragility), add:

```cpp
    // Instant self-heal: fires once per FRESH press of use (usePressed is edge-triggered
    // client-side, see InputMsg), only when the selected slot holds RevivePotion AND the
    // player did NOT make revive-channel progress this tick (i.e., no one was revivable in
    // range — if someone WAS revivable, the press should have gone toward the channel, not
    // a self-heal; checking channelTimer > 0.0f after the revive loop tells us which case we
    // are in, since UpdateRevive resets it to 0 whenever it made no progress against anyone
    // this tick, per its own existing logic already exercised in Step 2).
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (!active[i]) continue;
        Player& p = session.slots[i].player;
        if (!usePressed || !usePressed[i]) continue;
        if (p.state != PlayerState::Alive) continue;
        if (p.channelTimer > 0.0f) continue; // was channeling a revive this tick instead
        const InventorySlot& sel = p.inventory.SlotAt(p.selectedSlot);
        if (sel.count > 0 && sel.type == ItemType::RevivePotion) {
            if (p.inventory.Remove(ItemType::RevivePotion, 1)) {
                p.TryHeal(kSelfHealAmount);
            }
        }
    }
```

This introduces a NEW `usePressed` parameter to `SimulateSessionTick` (a `bool*` array, following the exact same pattern as the existing `attack`/`interact` parameters — read `SimulateSessionTick`'s current signature and add `bool* usePressed` alongside them, in the same style) and a new constant `kSelfHealAmount = 30` (place it near `kReviveRange`/other tick-related constants already declared near the top of `main.cpp` — find the exact existing constants block and match its style, e.g. `static constexpr int kSelfHealAmount = 30;`).

Update `SimulateSessionTick`'s forward declaration (near the top of the file) AND its real definition to add the new `bool* usePressed` parameter, and update every existing call site (the live tick loop AND every smoke test in `SmokeTests.h`) to pass a `usePressed` array — for all EXISTING smoke tests (which predate this feature and don't exercise self-heal), pass an all-`false` array (e.g. `bool usePressed[kMaxPlayersPerSession] = {};`) to preserve their existing behavior exactly unchanged. Also update the live tick loop's call site to build a real `usePressed` array from `sessionLatestInput[...][i].usePressed` (mirroring exactly how `interact`/`attack` arrays are already built from `latestInputs` there today — read that exact existing code and match its pattern).

Also update `Player::selectedSlot` from input each tick: find wherever `interact[i]`/`attack[i]` (or similar per-player input arrays) are derived from `latestInputs[i]` inside `SimulateSessionTick`, and add, in the same place: `session.slots[i].player.selectedSlot = Clamp(latestInputs[i].selectedSlot, 0, Inventory::kCapacity - 1);` (write a small local clamp inline, e.g. `int sel = latestInputs[i].selectedSlot; if (sel < 0) sel = 0; if (sel >= Inventory::kCapacity) sel = Inventory::kCapacity - 1; session.slots[i].player.selectedSlot = sel;` — a malformed or malicious client packet must never be allowed to set an out-of-range `selectedSlot`, since `SlotAt` indexes a fixed-size array with no bounds checking of its own per Task 1's design).

- [ ] **Step 4: Populate `PlayerSnapshot::slots[4]` from the real `Inventory`**

Find the snapshot-building call site (search for `PlayerSnapshot{` — the aggregate-init that currently includes `p.inventory.Count(ItemType::RevivePotion)` as its `potionCount` argument). Replace that single scalar argument with a small loop or inline construction of the new `slots[4]` array. Read the EXACT current aggregate-init code first (it's a single-line construction inline in a larger snapshot-building loop) and restructure minimally — e.g.:

```cpp
                    PlayerSnapshot ps{ p.position.x, p.position.y, p.hp, active[i] ? (uint8_t)p.state : kSnapshotStateAbsent, {}, p.channelTimer };
                    for (int s = 0; s < Inventory::kCapacity; s++) {
                        const InventorySlot& slot = p.inventory.SlotAt(s);
                        ps.slots[s] = HotbarSlotSnapshot{ (uint8_t)slot.type, slot.count };
                    }
                    snap.players[i] = ps;
```

(Adjust to match the ACTUAL current variable names/structure at that call site — this is a sketch of the transformation, not verbatim code to paste blindly. `Inventory::kCapacity` is now 4, matching `PlayerSnapshot::slots[4]`'s fixed size exactly — if these ever diverge in a future change, this loop would silently read/write out of bounds, so a `static_assert` here would be reasonable defensive hardening: `static_assert(Inventory::kCapacity == 4, "PlayerSnapshot::slots size must match Inventory::kCapacity");` placed near this code, though this is not required if the implementer judges the existing single-source-of-truth risk acceptable given both values are Task-1-authored constants unlikely to independently drift — use your judgment.)

- [ ] **Step 5: Update existing smoke tests referencing the old `Inventory`/`potionCount` API**

Search `src/server/SmokeTests.h` for any place that constructs an `Inventory`, calls `.Slots()` (which now returns `std::array` instead of `std::vector` — any test that assigns its result to a `std::vector<InventorySlot>` or calls vector-specific methods like `.size()` in a way incompatible with `std::array` needs adjusting — `std::array` also has `.size()`, so most usages should still compile, but check for anything that assumed dynamic growth, e.g. asserting `Slots().size() == 0` after removing the last item of a type — that assertion is NO LONGER TRUE under the fixed-array model, since a removed slot stays present at `count == 0` rather than shrinking the container; find this exact assertion (it exists in the current `SmokeTestItems` — you saw this in the plan's own research) and fix it to check `inv.Count(ItemType::RevivePotion) == 0` instead, or check the slot's `count == 0` directly via `SlotAt`, whichever fits the test's actual intent), and any place expecting `PlayerSnapshot::potionCount` to exist.

- [ ] **Step 6: Build**

Run: `mingw32-make server`

Expected: builds cleanly (server-side only — client still has its own `potionCount` references to fix in Task 4).

- [ ] **Step 7: Add smoke tests for the new behavior**

Add smoke tests (in `src/server/SmokeTests.h`, following that file's existing style and registered in `RunAllSmokeTests()`) covering:

```cpp
void SmokeTestHotbarAndSelfHeal() {
    // Inventory: fixed 4-slot behavior — Add claims the first empty slot, Remove clears in
    // place (does not compact/shift), Count and SlotAt agree.
    {
        Inventory inv;
        assert(inv.Add(ItemType::RevivePotion) == true);
        assert(inv.Count(ItemType::RevivePotion) == 1);
        assert(inv.SlotAt(0).count == 1 && inv.SlotAt(0).type == ItemType::RevivePotion);

        assert(inv.Remove(ItemType::RevivePotion, 1) == true);
        assert(inv.Count(ItemType::RevivePotion) == 0);
        assert(inv.SlotAt(0).count == 0); // slot stays in place, empty — not erased/shifted

        // Re-adding after removal claims the same now-empty slot again (not a new one).
        assert(inv.Add(ItemType::RevivePotion) == true);
        assert(inv.SlotAt(0).count == 1);
    }

    // Player::TryHeal: clamps to kMaxHp, no-ops when not Alive.
    {
        Player p(Vector2{0.0f, 0.0f});
        p.hp = 90;
        p.TryHeal(30);
        assert(p.hp == Player::kMaxHp); // clamped, not 120

        p.state = PlayerState::Downed;
        int hpBefore = p.hp;
        p.TryHeal(30);
        assert(p.hp == hpBefore); // no-op while not Alive
    }

    // Self-heal via SimulateSessionTick: selected slot holds RevivePotion, no one revivable
    // nearby, usePressed fires once -> instant heal, one potion consumed, no channel.
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[0].player = Player(kSpawnPoints[0]);
        session.slots[0].player.hp = 50;
        session.slots[0].player.selectedSlot = 0;
        session.slots[0].player.inventory.Add(ItemType::RevivePotion);
        // Slot 1 present but far away and Alive (not revivable), so self-heal is the only
        // applicable path — proves the gate correctly distinguishes "no one to revive".
        session.slots[1].state = SlotState::Connected;
        session.slots[1].player = Player(Vector2{900.0f, 500.0f});

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = { true, false, false, false };
        bool usePressed[kMaxPlayersPerSession] = { true, false, false, false };
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{};
        bool active[kMaxPlayersPerSession];

        SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed,
                             1.0f / 60.0f, active, potato, chargeTimer, latestInputs, courtBounds,
                             match, GameMode::RevivePotionTest);

        assert(session.slots[0].player.hp == 80); // 50 + 30, no clamp needed
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 0); // consumed
        assert(session.slots[0].player.channelTimer == 0.0f); // no channel occurred
    }

    // Revive-channel is blocked when a DIFFERENT slot is selected, even with a Downed
    // teammate in range and a potion in inventory (just not in the selected slot).
    {
        Session session;
        session.slots[0].state = SlotState::Connected;
        session.slots[0].player = Player(Vector2{0.0f, 0.0f});
        session.slots[0].player.selectedSlot = 1; // NOT the slot holding the potion
        session.slots[0].player.inventory.Add(ItemType::RevivePotion); // lands in slot 0

        session.slots[1].state = SlotState::Connected;
        session.slots[1].player = Player(Vector2{5.0f, 0.0f}); // within reviveRange
        session.slots[1].player.ForceDown();

        HazardZone hazard{ Rectangle{ 5000.0f, 5000.0f, 10.0f, 10.0f } };
        std::vector<WorldItem> items;
        Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };
        float hazardCarry[kMaxPlayersPerSession] = {};
        bool attack[kMaxPlayersPerSession] = {};
        bool interact[kMaxPlayersPerSession] = { true, false, false, false };
        bool usePressed[kMaxPlayersPerSession] = {};
        float chargeTimer[kMaxPlayersPerSession] = {};
        InputMsg latestInputs[kMaxPlayersPerSession] = {};
        MatchState match{};
        HotPotato potato{};
        bool active[kMaxPlayersPerSession];

        for (int tick = 0; tick < 200; tick++) { // far more than kChannelDuration(2s) at 60Hz
            SimulateSessionTick(session, items, hazard, hazardCarry, attack, interact, usePressed,
                                 1.0f / 60.0f, active, potato, chargeTimer, latestInputs, courtBounds,
                                 match, GameMode::RevivePotionTest);
        }

        assert(session.slots[1].player.state == PlayerState::Downed); // never revived
        assert(session.slots[0].player.inventory.Count(ItemType::RevivePotion) == 1); // never consumed
    }
}
```

Adjust the exact `SimulateSessionTick` call-site argument order/count in these tests to match whatever the REAL signature ends up being after Step 3 (this sketch assumes `usePressed` is inserted right after `interact` — verify against your own edit and correct if you placed it elsewhere in the parameter list). Register `SmokeTestHotbarAndSelfHeal()` in `RunAllSmokeTests()`.

- [ ] **Step 8: Build and run**

Run: `mingw32-make server && ./bin/server.exe --test`

Expected: all smoke tests pass, including the new ones and every pre-existing test (adjusted per Step 5) unchanged in intent.

- [ ] **Step 9: Commit**

```bash
git add src/server/Player.h src/server/Combat.h src/server/main.cpp src/server/SmokeTests.h
git commit -m "Gate revive-channel on hotbar slot selection; add instant self-heal on usePressed"
```

---

### Task 3: Client input — hotbar key selection, use-pressed edge signal

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `InputMsg::selectedSlot`/`usePressed` (Task 1).
- Produces: client-side hotbar selection state (which slot 1-4 last selected), sent every tick via `netClient.SendInput(...)`'s now-larger parameter list.

- [ ] **Step 1: Track the selected hotbar slot from keys 1-4**

In `src/client/main.cpp`, find the input-gathering block (where `move`, `interactHeld`, `attackPressed`, etc. are computed each frame — right before the `netClient.SendInput(...)` call). Add a `static int selectedSlot = 0;` (persists across frames, since selection should stick until changed — a local `static` inside `main()`'s loop body is fine here, matching how this file already has no separate "client state" struct for this kind of thing) and:

```cpp
        static int selectedSlot = 0;
        if (IsKeyPressed(KEY_ONE)) selectedSlot = 0;
        if (IsKeyPressed(KEY_TWO)) selectedSlot = 1;
        if (IsKeyPressed(KEY_THREE)) selectedSlot = 2;
        if (IsKeyPressed(KEY_FOUR)) selectedSlot = 3;

        bool usePressed = IsKeyPressed(KEY_E);
```

(`interactHeld` already exists via `IsKeyDown(KEY_E)` right above — `usePressed` is a SEPARATE read of the same physical key via `IsKeyPressed`, which raylib supports calling both ways on the same key within one frame without conflict; this mirrors `chargingThrow`/`releaseThrow` already being two different edge/hold reads of the same mouse button in this exact file.)

- [ ] **Step 2: Thread the two new values into `SendInput`**

Find `netClient.SendInput(move.x, move.y, interactHeld, attackPressed, chargingThrow, releaseThrow, aimDirX, aimDirY, dashPressed);` and add the two new arguments at the end, matching `InputMsg`'s new field order from Task 1:

```cpp
        netClient.SendInput(move.x, move.y, interactHeld, attackPressed, chargingThrow, releaseThrow, aimDirX, aimDirY, dashPressed, selectedSlot, usePressed);
```

This requires updating `NetClient::SendInput`'s signature and implementation (in `src/client/NetClient.h`/`.cpp`) to accept and forward the two new parameters into the `InputMsg` it constructs — read the current exact signature/implementation there and extend it consistently with how every other field is already threaded through.

- [ ] **Step 3: Build**

Run: `mingw32-make client`

Expected: does NOT yet build cleanly — `PlayerSnapshot::potionCount` reads in this same file and in `Juice.cpp` still reference the removed field (Task 4 fixes this). Confirm the ONLY remaining errors are about `potionCount`.

- [ ] **Step 4: Commit**

```bash
git add src/client/main.cpp src/client/NetClient.h src/client/NetClient.cpp
git commit -m "Send hotbar slot selection and edge-triggered use signal from the client"
```

(Intentionally still red — Task 4 finishes the client build.)

---

### Task 4: Client — hotbar UI rendering, remove old potion-count text

**Files:**
- Create: `src/client/Hotbar.h`
- Modify: `src/client/main.cpp`
- Modify: `src/client/Juice.h`
- Modify: `src/client/Juice.cpp`

**Interfaces:**
- Consumes: `PlayerSnapshot::slots[4]` (Task 1), the client's `selectedSlot` local (Task 3 — needs to become accessible here too, or the hotbar draw function takes it as a parameter).
- Produces: `Hotbar.h`'s `DrawHotbar(const PlayerSnapshot& mySnapshot, int selectedSlot, int screenWidth, int screenHeight)` (or similar — a small, focused draw function, matching the style of this codebase's other small client modules like `AimDirection.h`); the old "Potions: N" text and `prevPotionCount` tracking are removed.

- [ ] **Step 1: Remove `potionCount`/`prevPotionCount` references**

In `src/client/main.cpp`, find and remove the line:

```cpp
            DrawText(TextFormat("Potions: %d", p.potionCount), (int)pos.x - 30, (int)pos.y + 20, 12, DARKBLUE);
```

(This per-player-above-their-head potion text is being replaced by the new bottom-of-screen hotbar for the LOCAL player only — other players' inventories were never meaningfully visualized beyond this single number anyway, and the plan's approved design didn't ask for other players' inventories to be shown; only the label above each player's head disappears, the hotbar itself only shows YOUR OWN slots.)

In `src/client/Juice.h`, remove the `int prevPotionCount[kMaxPlayersPerSession] = {0, 0, 0, 0};` field. In `src/client/Juice.cpp`, remove the line `prevPotionCount[i] = p.potionCount;`. (Confirm via a search that `prevPotionCount` isn't read anywhere else before removing — per this plan's own research, it currently isn't.)

- [ ] **Step 2: Create `Hotbar.h`**

```cpp
#pragma once

#include <raylib-cpp.hpp>
#include "../shared/Protocol.h"

// Draws the local player's own 4-slot hotbar along the bottom of the screen: one box per
// slot, showing the held item's count (blank if empty) and highlighting whichever slot is
// currently selected. Screen-space (not affected by the world camera), drawn OUTSIDE
// BeginMode2D/EndMode2D in main.cpp, same as other pure-HUD elements in this codebase.
inline void DrawHotbar(const PlayerSnapshot& mySnapshot, int selectedSlot, int screenWidth, int screenHeight) {
    const int kSlotSize = 48;
    const int kSlotGap = 8;
    const int kSlotCount = 4;
    int totalWidth = kSlotCount * kSlotSize + (kSlotCount - 1) * kSlotGap;
    int startX = (screenWidth - totalWidth) / 2;
    int y = screenHeight - kSlotSize - 16;

    for (int i = 0; i < kSlotCount; i++) {
        int x = startX + i * (kSlotSize + kSlotGap);
        Rectangle box{ (float)x, (float)y, (float)kSlotSize, (float)kSlotSize };

        bool selected = (i == selectedSlot);
        DrawRectangleRec(box, selected ? Fade(SKYBLUE, 0.35f) : Fade(LIGHTGRAY, 0.25f));
        DrawRectangleLinesEx(box, selected ? 3 : 1, selected ? SKYBLUE : DARKGRAY);

        const HotbarSlotSnapshot& slot = mySnapshot.slots[i];
        DrawText(TextFormat("%d", i + 1), x + 4, y - 18, 14, BLACK); // key label above the box

        if (slot.count > 0) {
            // Only one item type exists today (RevivePotion) — draw a simple filled circle as
            // its icon rather than a real sprite/texture, matching this project's existing
            // placeholder-shape visual style (e.g. the gold circle used for world items).
            DrawCircleV(Vector2{ (float)(x + kSlotSize / 2), (float)(y + kSlotSize / 2) }, 14, GOLD);
            DrawText(TextFormat("%d", slot.count), x + kSlotSize - 16, y + kSlotSize - 18, 14, BLACK);
        }
    }
}
```

- [ ] **Step 3: Call `DrawHotbar` from `main.cpp`**

In `src/client/main.cpp`, `#include "Hotbar.h"` alongside the other client includes. Call `DrawHotbar(...)` OUTSIDE the `BeginMode2D(camera)`/`EndMode2D()` block (it's a screen-space HUD element, not a world-space one — find where other screen-space HUD elements, if any, are drawn relative to that block, or place it right after `EndMode2D()` and before `EndDrawing()`), passing the local player's own snapshot:

```cpp
        if (netClient.HasReceivedSnapshot() && mySlot < kMaxPlayersPerSession) {
            DrawHotbar(snap.players[mySlot], selectedSlot, screenWidth, screenHeight);
        }
```

(`selectedSlot` here is the SAME local variable Task 3 introduced in the input-gathering block earlier in the same function — since Task 3 declared it as a `static int` local to `main()`'s loop body, it's already in scope at this later point in the same function; no plumbing needed beyond making sure the two blocks are in the same function scope, which they are.)

- [ ] **Step 4: Build**

Run: `mingw32-make client`

Expected: builds cleanly now (this is the task that resolves the `potionCount` compile errors left open since Task 1).

- [ ] **Step 5: Build and run full test suite**

Run: `mingw32-make server && mingw32-make client && ./bin/server.exe --test && ./bin/client.exe --test`

Expected: both build cleanly, all server and client smoke tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/client/Hotbar.h src/client/main.cpp src/client/Juice.h src/client/Juice.cpp
git commit -m "Add hotbar UI rendering, remove old per-player potion-count text"
```

---

## Final Verification

After all four tasks:

- [ ] Run `mingw32-make server && mingw32-make client` — both build cleanly.
- [ ] Run `./bin/server.exe --test` — all server smoke tests pass, including the new hotbar/self-heal tests and every pre-existing test (with the `Inventory::Slots()`-related assertion adjustments from Task 2 Step 5).
- [ ] Run `./bin/client.exe --test` — all client smoke tests pass.
- [ ] Live verification: connect 2+ clients. Confirm a 4-box hotbar appears at the bottom of the screen, slot 1 highlighted by default. Pick up a potion (walk over one, hold E) and confirm it appears as a filled slot with count 1.
- [ ] Live verification, revive-channel gating: with the potion slot selected, stand near a Downed teammate and hold E — confirm the existing revive-channel ring UI still appears and revive still completes after ~2s, exactly as before. Then switch to a DIFFERENT hotbar slot (press 2/3/4) and hold E near the same Downed teammate — confirm NO channel progress occurs (no ring, no revive).
- [ ] Live verification, self-heal: take damage (e.g. via the hazard zone in RevivePotionTest mode), select the potion slot, and with no one Downed nearby, press E once — confirm HP increases by 30 (capped at max) instantly, with no channel delay, and the potion count decrements by 1.
- [ ] Live verification: confirm pickup (walking over a world item and holding E) still works regardless of which hotbar slot is currently selected.
