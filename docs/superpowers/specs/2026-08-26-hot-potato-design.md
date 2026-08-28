# Hot Potato Game Mode Design

## Goal

Transform the existing 2-player co-op test game into "Hot Potato": a 1-4 player party game where players throw an explosive potato with charged, aimed throws; whoever is holding it when it explodes (or throws it out of bounds) loses the round; matches are best-of-3 rounds with a sudden-death tiebreak. A separate 2v2 team mode adds the existing revive mechanic back in, reworked to be round-scoped rather than continuous.

## Non-goals

- No real 3D/Z-axis physics — the potato's flight arc is faked in the existing 2D plane (distance scales with charge, simple visual arc, no true height/gravity simulation).
- No spectator mode, matchmaking, or ranking — room creation/joining via the existing 6-digit code system is unchanged.
- No mid-round auto-respawn (see Phase 5) — this is an intentional removal of existing behavior for this mode, not an oversight.
- No melee attack (`Q`) in Hot Potato mode — the potato explosion is the only "combat."
- Team selection UI for 2v2 — teams are fixed by slot order (0+1 vs 2+3).

## Build Order (5 phases, each independently testable)

1. **Networking foundation**: expand player slots from 2 to 4.
2. **Core mechanics**: charge-and-release aimed throwing, catching, arena bounds, escalating explosion timer, solo-mode wall bounce.
3. **FFA scoring**: everyone-but-the-loser scoring, best-of-3 fixed rounds, sudden-death tiebreak.
4. **Dash ability**: short-burst movement dash with cooldown.
5. **2v2 mode**: team assignment, revive mechanic (reworked to round-scoped), round-based respawn.

Each phase gets its own implementation plan (separate SDD pass) once this spec is approved — this document covers the full design so later phases don't contradict earlier ones.

---

## Phase 1: Networking Foundation (2 → 4 Player Slots)

### Current state

`Session` (`src/server/Session.h`) hardcodes `PlayerSlot slots[2]`. `SnapshotMsg`/`PlayerSnapshot` arrays in `src/shared/Protocol.h` are fixed at `[2]`. `SessionManager::HandleConnect` (`src/server/SessionManager.h`) only ever assigns slot 0 or 1. The client (`src/client/main.cpp`) renders exactly 2 `drawPlayer` calls. The debug menu draws exactly 2 columns.

### Changes

- `Session::slots` becomes `PlayerSlot slots[4]`. `Session::FindEmptySlot()`, `FindDisconnectedSlotByToken()` loop bound changes from 2 to 4.
- Spawn points: 4 fixed spawn positions (`kSlot0Spawn` through `kSlot3Spawn`), one near each corner of the arena, replacing the current 2-spawn (left/right) layout.
- `SnapshotMsg::players` and `WorldItemSnapshot` arrays: `PlayerSnapshot players[4]`. (World items stay at a small fixed count, not tied to player count — existing `items[2]` can stay, or grow to `items[4]` if Phase 2's revive-item spawning wants more; decided in Phase 5 since only 2v2 uses revive items.)
- `WelcomeMsg` gains a `uint8_t roomPlayerCount` or similar so the client knows how many slots are actually active-relevant (not strictly required — client can just check each slot's `state` for `kSnapshotStateAbsent`, same pattern as today).
- Server main loop (`src/server/main.cpp`): all per-session logic that currently hardcodes `p0`/`p1` and does two-player-specific pairwise checks (attack, revive, hazard) becomes a loop over `[0..3]` (or `[0..3]x[0..3]` for pairwise interactions like revive-checking, which Phase 5 touches — Phase 1 itself only needs the slot count increase and pairwise-interaction loops don't fully matter yet since Hot Potato removes melee/hazard in Phase 2).
- Client (`src/client/main.cpp`): `drawPlayer` calls become a loop over up to 4 slots; colors need 2 more values (e.g. `GREEN`, `PURPLE` alongside existing `BLUE`/`MAROON`).
- `DebugMenu` (`src/client/DebugMenu.h`): `HandleColumn` calls become a loop over up to 4 slots (4 columns instead of 2, menu panel widened).
- `RoomMenu`/`SessionManager` create/join logic: "session full" now means all 4 slots occupied, not 2. `FindEmptySlot` naturally handles this once the array is size 4.

### Testing

Server: extend `SmokeTestSessionManager` with a 4-player fill scenario (4 successive joins succeed, 5th is rejected `SessionFull`). Client: no new automated tests (pure rendering loop change); verified live once Phase 2 makes 3-4 player rooms actually playable.

---

## Phase 2: Core Hot Potato Mechanics

### The potato (shared server-authoritative object, one per session)

New struct `HotPotato` (server-side, e.g. `src/server/HotPotato.h`):

```cpp
struct HotPotato {
    bool inFlight = false;      // true while traveling between throw and catch/landing
    bool held = false;          // true while a player is holding it (not in flight, not idle)
    int holderSlot = -1;        // which slot currently holds it, -1 if in flight or unheld
    Vector2 position{0, 0};     // current position (authoritative)
    Vector2 velocity{0, 0};     // current velocity while in flight
    float explodeTimer = 0.0f;  // countdown to explosion, starts at kPotatoStartTimer
    int catchCount = 0;         // number of successful catches this round, drives timer shrink
};
```

At round start, the potato spawns held by a randomly chosen player (or, in solo mode, held by the lone player).

### Charge-and-release throwing

- Client sends charge state via a new input field: `bool chargingThrow` (held) and, on release, the accumulated charge duration is what determines force — the SERVER tracks charge duration authoritatively (client only reports "charging: yes/no" each tick and a release event) to avoid trusting client-reported force values.
- `InputMsg` (`src/shared/Protocol.h`) gains: `bool chargingThrow`, `bool releaseThrow`, `float aimDirX`, `float aimDirY` (normalized direction from player position to mouse cursor, computed client-side since only the client knows the cursor position, sent every tick while relevant).
- Server tracks `chargeTimer` per-holder (in `Player` or a parallel per-slot field): increments while `chargingThrow` is true, capped at `kMaxChargeDuration` (e.g. 1.5s for max force). On `releaseThrow` (only meaningful if this player is `holderSlot`), the throw fires:
  - `force = kMinThrowForce + (chargeTimer / kMaxChargeDuration) * (kMaxThrowForce - kMinThrowForce)`, clamped.
  - `potato.velocity = aimDirection * force`.
  - `potato.held = false`, `potato.inFlight = true`, `potato.holderSlot = -1`.
  - `chargeTimer` resets to 0.

### Flight and catching

- While `inFlight`, each server tick: `potato.position += potato.velocity * dt`, with a fixed drag/deceleration factor so it eventually slows and "lands" (`speed` drops below `kLandingSpeedThreshold` → transitions to landed-but-uncaught state, still `inFlight` conceptually until caught or it fully stops... simplify: treat "landed" as inFlight with velocity decayed to ~0, still catchable by proximity).
- Catching: each tick while `inFlight`, check distance from `potato.position` to every ACTIVE, non-Dead player's position; if any is within `kCatchRadius` (e.g. 20px) AND that player is not the one who just threw it (prevent instant self-catch on release — actually, self-catch should be fine/allowed for solo mode; only prevent catching during the very first few frames right at the thrower's own position to avoid a zero-distance same-tick catch artifact), that player becomes the new `holderSlot`, `potato.held = true`, `potato.inFlight = false`, `potato.velocity = {0,0}`, `potato.position` snaps to holder's position (so it visually "sticks" to them), `catchCount += 1`.
- Timer on catch: `explodeTimer = max(kPotatoTimerFloor, kPotatoStartTimer - catchCount * kPotatoTimerShrink)` — i.e. `10.0 - catchCount * 2.0`, floored at `3.0`. Applied every catch, including the very first "pickup" at round start (`catchCount` starts at 0, so the first hold uses the full `kPotatoStartTimer`).
- While `held`, `explodeTimer -= dt` each tick. At `<= 0`, the potato explodes (see Round End below).
- While `held`, potato position is pinned to the holder's position each tick (so it visually follows them).

### Arena bounds and out-of-bounds

- Reuses the existing hazard-zone-style `Rectangle` concept for arena bounds (a `Rectangle courtBounds` on the server), sized to cover the visible play area (e.g. `{0, 0, 1000, 600}` matching the client's window, minus a small margin).
- While `inFlight`, each tick check if `potato.position` is outside `courtBounds`. In a MULTIPLAYER match (2+ active players), going outside bounds triggers the same "loss" resolution as an explosion (see Round End), attributed to whoever last threw it (tracked via a `lastThrowerSlot` field on `HotPotato`, set on every `releaseThrow`).
- In SOLO mode (exactly 1 active player in the room, confirmed by counting active slots), instead of the out-of-bounds loss rule, the potato's velocity reflects off whichever boundary edge it crossed (`velocity.x *= -1` for left/right edges, `velocity.y *= -1` for top/bottom edges), and position is clamped back inside bounds by the overshoot amount (simple reflection, not a physically perfect bounce, but visually reads as "bouncing off the wall"). This lets a lone player keep throwing against the wall and catching the rebound.

### Round end (explosion or out-of-bounds, multiplayer only)

- Trigger: `explodeTimer <= 0` while held, OR `inFlight` and position leaves `courtBounds` (multiplayer only).
- The "losing" player is: `holderSlot` if it exploded while held; `lastThrowerSlot` if it left bounds in flight.
- Resolution (exact scoring rules are Phase 3's concern; Phase 2 just defines the round-end EVENT and who's attributed as the cause) — Phase 2 itself is responsible for: the losing player takes an instant-Down (see Damage below), and the round-end event fires for Phase 3's scoring logic to consume.
- After resolution, a brief pause (e.g. 2 seconds) then the next round begins: potato respawns held by a random active player, `catchCount` resets to 0, explosion/scoring state resets.

### Damage: explosion downs the holder

- Hot Potato mode removes the existing melee-attack (`Q`) input entirely — `TryAttack`/`Combat.h` logic is not called in this mode.
- The existing hazard zone is not spawned/active in Hot Potato mode (no `HazardZone` instance created for Hot Potato sessions).
- On round-end due to explosion (held, timer hit 0): the losing player takes damage equivalent to an instant Down — call the existing `Player::ForceDown()` (already exists, already correctly guards `state != Alive` no-ops) rather than a graduated damage/HP mechanic. This keeps Phase 2 simple: explosion = instant Downed, not a partial-HP mechanic.
- On round-end due to out-of-bounds (in flight, left the court): same treatment — the last thrower is `ForceDown()`'d. (This means even the thrower, not just a holder, can be downed — a deliberate consequence of "don't throw it out.")

### Testing

Server: new `HotPotato.h` gets its own smoke tests (mirroring the existing `Combat.h`/`Hazard.h` pattern) covering: charge accumulates and caps, force scales with charge, catch radius detection, timer shrinks correctly per catch and floors at 3.0, out-of-bounds detection in multiplayer downs the last thrower, solo-mode wall-bounce reflects velocity correctly and does NOT trigger the down/loss path.

---

## Phase 3: FFA Scoring, Best-of-3, Tiebreak

### Round scoring

- On round-end (from Phase 2's event), every ACTIVE player except the one attributed as the cause gets `+1` to a new per-session `roundScore[4]` array (server-side, part of `Session` or a parallel match-state struct).
- A Dead player (from a PREVIOUS round, not yet respawned — see Phase 5 interaction) does not score even if they'd otherwise not be "the cause," since they're out of play for that round. (In Phase 2/3 without 2v2's revive rework, this mostly matters once Phase 5 adds round-scoped Dead state; for FFA-only testing before Phase 5 lands, this is a no-op since Phase 2's instant-Down-only model doesn't yet produce lingering Dead players across rounds — flag this as a Phase 3/5 integration point.)

### Match structure: exactly 3 rounds, most points wins

- New per-session state: `int roundsPlayed` (0-3), `roundScore[4]`.
- After round 3 completes, the match ends. If exactly one player has the highest `roundScore`, they're the match winner. If 2+ players tie for highest, trigger a sudden-death tiebreak round (see below) among ONLY the tied players (others are still present/rendered but excluded from scoring/attribution in the tiebreak — simplest to implement as: the tiebreak round runs exactly like a normal round, but only tied players' scores are compared afterward to determine the winner; non-tied players can still physically participate/be thrown to, to avoid needing to freeze/exclude them from the physics simulation).
- Tiebreak round: identical rules to a normal round (potato spawns, timer counts down, explosion/out-of-bounds attributes a loser). After it ends, compare `roundScore` deltas among the originally-tied players from this one extra round using the same "everyone but the cause scores" rule; whoever now has the higher total among the tied group wins. If STILL tied after one tiebreak round (possible with 3+ tied players and different subsets scoring), repeat the tiebreak round again (uncapped repeat — vanishingly unlikely to loop many times in practice, but no artificial cap needed since each round has decent odds of breaking symmetry).
- `WelcomeMsg`/`SnapshotMsg` gain fields for the client to render: current round number (1-3, or "Tiebreak"), each active player's current round score, and a match-winner announcement when the match concludes.

### Testing

Server: smoke tests for the scoring loop (round-end correctly credits all-but-the-cause), the 3-round match completion logic, and the tiebreak trigger/resolution (construct a scenario with 2 tied players, run one tiebreak round, confirm winner determination).

---

## Phase 4: Dash Ability

### Mechanic

- New input: a dash key (e.g. `Shift`), sent as `bool dashPressed` in `InputMsg` (a new field, or reuse a spare bit if convenient — additive protocol change either way).
- Server-side per-player state: `float dashCooldownTimer` (counts down each tick, new field on `Player` or a parallel per-slot array).
- On `dashPressed` with `dashCooldownTimer <= 0` and `state == Alive`: instantly displace the player's position by `kDashDistance` (150px) in their CURRENT MOVEMENT INPUT direction (the same `moveX`/`moveY` from the existing `InputMsg`, normalized) — if no movement input is held at the moment of dash, use the player's last non-zero movement direction (tracked as a new small per-player "facing" vector, updated whenever `moveX`/`moveY` is non-zero) so dashing while stationary still does something sensible.
- Sets `dashCooldownTimer = kDashCooldown` (2.0s).
- No collision/wall-clamping beyond what movement already does (if the existing movement has no wall-clamping today, dash doesn't add any either — consistent with current behavior). If the destination would be outside the arena `courtBounds`, clamp the final position to just inside the boundary (prevents dashing out of the playable area entirely, though this doesn't interact with the potato's out-of-bounds rule since players aren't "the potato").
- No invincibility frames, no interaction with the potato's collision/catch radius beyond normal position updates (a dash can, incidentally, dash a player into or out of catch range, which is an intended emergent use).

### Testing

Server: smoke test confirming dash moves the player kDashDistance in the expected direction, respects cooldown (a second dash attempt before cooldown expires is a no-op), and the "last facing direction" fallback works when dashing with no current input.

---

## Phase 5: 2v2 Mode (Teams, Revive Rework, Round-Based Respawn)

### Mode selection

- The pre-game `RoomMenu` (or a new selection step after room join, before match start) offers a mode choice: FFA or 2v2. 2v2 requires exactly 4 connected players to start (room stays in a "waiting" state otherwise, same pattern as the existing 2-player wait-for-slot-fill).
- Team assignment: slots 0+1 = Team A, slots 2+3 = Team B (fixed, no picker UI).

### Scoring change for 2v2

- Round-end attribution: the LOSING TEAM is whichever team the "cause" player (holder-on-explosion or last-thrower-on-out-of-bounds) belongs to. The OTHER team scores `+1` as a team (both members credited, or just a shared team score — simplest: one `teamScore[2]` array instead of `roundScore[4]`).
- Best-of-3/tiebreak logic (Phase 3) reuses identically, just comparing 2 team scores instead of up to 4 individual scores (no tiebreak-among-3+-parties complexity — with exactly 2 teams, a tie is only ever a 2-way tie, so the tiebreak round is simpler: whichever team doesn't cause the next round's loss wins outright).

### Revive mechanic (reworked, round-scoped)

- The existing `Player` state machine (Alive → Downed → Dead) and `ReviveFromDowned()`/`Kill()` methods stay as-is structurally, but:
  - Downed still starts a 15s timer (`kDownedDuration`, unchanged constant) — if a teammate revives them within that window (existing channel-revive mechanic: hold interact near a Downed teammate while carrying a revive potion), they return to Alive at `kReviveHp`... but Hot Potato has no HP/damage system in the traditional sense (Phase 2 established explosion = instant-Down, not HP loss) — so "hp" on revive is largely vestigial for Hot Potato; a revived player just returns to `Alive` and remains in-round.
  - If the Downed timer expires with no revive, the player becomes Dead — but Dead now has NO auto-respawn timer (`Player::UpdateTimers`'s existing Dead-state branch, which currently ticks `deathRespawnTimer` down to 0 and calls `RespawnFull()`, is DISABLED/skipped for Hot Potato sessions). A Dead player stays Dead, is excluded from the potato-catch/throw simulation and from being a valid throw target, and is rendered as such (existing `Fade(GRAY, 0.3f)` dead-state visual already does this) until the CURRENT ROUND ends.
  - At the start of each new round (Phase 2/3's round-end → next-round-begin transition), ALL players (regardless of Alive/Downed/Dead state) are fully reset to Alive via `RespawnFull()` at their team's spawn point. This is the only respawn trigger now.
- Revive-potion items: the existing `WorldItem`/pickup system is reused, but item SPAWN LOCATIONS must be OUTSIDE the arena's `courtBounds` (per your requirement: "spawn random revive item outside of course, player have to run pickem up and run back to dead player") — a small set of designated pickup spots just outside the playable rectangle (e.g. 4 fixed spots, one beyond each edge of the court), rather than the fully random-within-arena placement items use today. Picking one up works identically to today (proximity-based `TryPickup`), just relocated conceptually outside the court so fetching one costs the traveling player time away from the potato action — a deliberate risk/reward tradeoff already implied by your description.
- FFA mode does NOT use revive/Downed-with-a-reviver logic at all (per the earlier "no teammates" ruling). Phase 2's `ForceDown()` call on round-loss still applies uniformly in both FFA and 2v2 (it's the shared round-end consequence from Phase 2) — the DIFFERENCE in FFA is simply that no one can revive a Downed player (no teammate exists to channel-revive them), so the Downed timer will always expire naturally into Dead. This is not a special FFA code path; it falls out of FFA mode simply never spawning revive-potion items and the channel-revive check already requiring a nearby ally in `PlayerState::Downed` — with no ally present, revive never triggers, and the existing 15s-Downed-then-Dead timer plays out unassisted. (2v2 is the only mode where a revive can actually interrupt that timer.)

### Testing

Server: smoke tests for team assignment (slots 0+1 vs 2+3), team-based round scoring, the Dead-state auto-respawn REMOVAL (confirm `UpdateTimers` no longer respawns a Dead player mid-round when in Hot Potato/2v2 mode — likely needs a mode flag threaded into `Player` or handled at the call-site level rather than inside `Player::UpdateTimers` itself, to avoid breaking the ORIGINAL non-Hot-Potato game mode's auto-respawn, which should stay unchanged for any non-Hot-Potato session), the round-start full-reset-to-Alive behavior, and revive-item spawn-location constraint (all spawn points are outside `courtBounds`).

---

## Cross-Phase Integration Notes

- **Mode flag**: sessions need a `GameMode` concept (e.g. `enum class GameMode { Classic, HotPotatoFFA, HotPotato2v2 }`) so the server can branch cleanly between "the original pickup/inventory/revive test game" (already shipped, must keep working unmodified) and the new Hot Potato modes. This flag likely gets chosen at room-creation time (a new field in `ConnectRequestMsg` or a separate pre-match mode-select step) and stored on `Session`.
- **Preserving the existing game**: nothing in this spec should break the already-shipped "Classic" mode (pickup/inventory/revive/hazard/PvP). Hot Potato's removal of melee/hazard and its reworked respawn timers are SCOPED TO Hot Potato sessions only, gated by the `GameMode` flag, not global changes to `Player`/`Combat.h`/`Hazard.h`'s existing behavior.
- **Client rendering**: the client needs to know which mode it's in (from `WelcomeMsg` or an early snapshot field) to decide whether to draw potato-specific UI (charge meter, explosion timer over the holder's head, round/score HUD) versus the existing debug-menu-driven Classic UI.

## Open items carried into the plans

- Exact arena `courtBounds` rectangle dimensions and the 4 revive-item spawn-point coordinates (Phase 5) are implementation details for that phase's plan, not blocking design decisions.
- Whether `GameMode` selection happens via `RoomMenu` (before room creation) or a separate lobby step (after joining, before match start) is an implementation-plan-level UI decision for Phase 1/5, not a blocking design question — either works with the server-side `GameMode` flag design above.
