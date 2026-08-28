# Pickup / Inventory / Revive System — Design Spec

Date: 2026-08-26
Status: Approved for implementation planning

## Purpose

Implement a local 2-player co-op test build in raylib-cpp demonstrating:
item pickups, a per-player inventory, and a revive mechanic gated on
both proximity/channeling and holding a revive item. The build also
needs a way for players to take damage, become downed, die, and
respawn, plus a debug menu for manually manipulating player state
during testing.

This is a test/demo scope, not a full game: no win condition, no
game-over screen, single test map.

## Players & Controls

Two players, both on keyboard (local split-keyboard co-op), same
screen:

- **Player 1**: WASD to move, `E` to interact/channel-revive/pickup, `Q` to attack.
- **Player 2**: Arrow keys to move, `RightCtrl` to interact/channel-revive/pickup, `RightShift` to attack.

## Player State Machine

Each player is in exactly one state:

- **Alive** — can move, attack, pick up items, channel revives on a downed ally.
- **Downed** — cannot move or act. `downedTimer` counts down from 15s.
  If revived before it reaches 0, returns to Alive. If it reaches 0,
  becomes Dead.
- **Dead** — cannot move or act, not revivable via the normal item+channel
  mechanic (only via debug menu). `deathRespawnTimer` counts down from
  5s, then the player automatically respawns: state → Alive, HP → 100,
  position → that player's spawn point.

Transitions:

```
Alive --(HP reaches 0)--> Downed
Downed --(revived: item + channel, or debug "Revive")--> Alive (50 HP)
Downed --(downedTimer expires)--> Dead
Dead --(deathRespawnTimer expires, or debug "Revive")--> Alive (100 HP, at spawn)
Any state --(debug "Kill")--> Dead
Any state --(debug "Force Down")--> Downed
```

## Damage Sources

1. **Hazard zone** — one or more static rectangular zones on the map.
   Any player whose position is inside a hazard zone takes 5 HP/sec
   (applied continuously, scaled by delta time) while Alive.
2. **Player vs player melee** — each player has an attack key
   (separate from interact). Pressing it, if off cooldown (0.8s) and
   the other player is within attack range (40px) and Alive, deals 15
   damage to that player and starts the attacker's cooldown. No effect
   on Downed/Dead players.

Both damage sources only apply to players in the Alive state.

## Items & Inventory

- Item types: `RevivePotion` only for now (`enum class ItemType`,
  extensible for future item types).
- World items are placed at fixed positions on the map as
  `WorldItem { position, type, active }`. When an Alive player is
  within pickup radius (24px) of an active world item and presses
  their interact key, the item is added to that player's inventory and
  removed from the world (`active = false`).
- Each player has an `Inventory`: up to 8 distinct slots, each holding
  `{ type, count }`. Picking up a `RevivePotion` when a slot for that
  type already exists increments its count; otherwise it takes a new
  slot (up to 8 slots max — additional pickups are ignored/dropped if
  full, no UI feedback needed beyond this being a rare edge case in
  testing).

## Revive Mechanic

Preconditions: reviver is Alive, has `count >= 1` of `RevivePotion` in
inventory, and is within revive range (32px) of a Downed teammate.

- Holding the interact key while preconditions hold fills a channel
  timer from 0 to 2.0s.
- If the reviver moves out of range, releases the key, or a
  precondition stops holding (e.g. runs out of item — shouldn't happen
  mid-channel since it's only consumed on completion) before the timer
  completes, the channel resets to 0 — no partial progress carries over.
- On completion: consume 1 `RevivePotion` from the reviver's inventory,
  the downed player becomes Alive with 50 HP, downedTimer/channel reset.
- Rendered as a dashed progress ring around the reviver, filling
  clockwise as the channel timer advances (matches the mockup's visual
  language).

## Debug Menu

- Toggled with `F1`. While open, shows an overlay panel listing each
  player (P1, P2) with manually-drawn buttons (rectangles + mouse-click
  detection, no raygui dependency added) for:
  - **Kill** — sets state to Dead immediately (starts respawn timer).
  - **Revive** — sets state to Alive (50 HP if was Downed, 100 HP if
    was Dead), regardless of items/proximity.
  - **Heal Full** — sets HP to 100 if Alive.
  - **Give Revive Item** — adds 1 `RevivePotion` to that player's inventory.
  - **Force Down** — sets state to Downed (resets downedTimer to 15s),
    only valid from Alive.
- The debug menu bypasses all normal preconditions — it's a raw state
  editor for testing.

## Rendering / HUD

- Hazard zone(s) drawn as tinted rectangles.
- World items drawn as small icons/shapes at their position (while active).
- Players drawn as colored circles (distinct color per player), tinted
  grey when Downed, dim/translucent when Dead.
- HP bar above each player.
- Per-player inventory slots (icons + counts) drawn in a HUD corner.
- Channel-revive progress ring drawn around the reviving player while
  channeling.
- On-screen control hints (movement/interact/attack keys) and F1 hint
  for the debug menu.
- Debug overlay panel drawn on top when toggled.

## File Structure

```
src/
  main.cpp        — window setup, game loop, wires systems together
  Player.h/.cpp    — Player struct/class: state machine, movement, attack,
                      timers, inventory member
  Item.h/.cpp      — ItemType enum, WorldItem struct, Inventory struct,
                      pickup logic
  Hazard.h/.cpp    — Hazard zone struct + damage-tick logic
  DebugMenu.h/.cpp — F1-toggled overlay: layout, click handling, per-player actions
```

No new external dependencies (no raygui) — the debug menu uses plain
raylib draw calls and manual mouse-rect hit-testing.

## Numeric Defaults

| Value | Default |
|---|---|
| Max HP | 100 |
| Downed duration | 15s |
| Death respawn delay | 5s |
| Revive HP (from Downed) | 50 |
| Respawn HP (from Dead) | 100 |
| Attack cooldown | 0.8s |
| Attack range | 40px |
| Attack damage | 15 |
| Pickup radius | 24px |
| Revive range | 32px |
| Channel-revive duration | 2.0s |
| Hazard damage rate | 5 HP/sec |
| Inventory capacity | 8 slots |

## Testing Approach

This is a real-time game loop, not unit-testable business logic in the
traditional sense. Verification is manual: build with
`mingw32-make`/`make`, run the executable, and walk through each
mechanic end-to-end (movement for both players, item pickup, hazard
damage, PvP damage, becoming Downed, channel-revive with the ring
visible, consuming the revive item, dying after downed timer expires,
auto-respawn, and each F1 debug menu action) to confirm behavior
matches this spec. State-transition and damage-math logic is kept in
plain methods on `Player`/`Inventory` so it stays readable and
reviewable even without an automated test harness.
