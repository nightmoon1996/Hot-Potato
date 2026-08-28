# Hot Potato

A small server-authoritative multiplayer party game built with [raylib](https://github.com/raysan5/raylib) / [raylib-cpp](https://github.com/robloach/raylib-cpp) in C++17. Up to 4 players connect over UDP to a dedicated server and play **Hot Potato**: charge and throw an object, catch it off your teammates, and don't be the one holding it when the timer runs out.

## Contents

- [Building](#building)
- [Running](#running)
- [Game modes](#game-modes)
- [Controls](#controls)
- [Gameplay features](#gameplay-features)
- [Project structure](#project-structure)
- [Tests](#tests)
- [Design docs](#design-docs)

## Building

### Dependencies

Install platform build tools first — see [docs/InstallingDependencies.md](docs/InstallingDependencies.md) (MinGW/G++ on Windows, G++ on Linux, Clang++ on macOS).

### First-time setup

Pulls in `raylib`/`raylib-cpp` as submodules, builds raylib as a static library, and copies headers into `include/`. Only needs to run once (or after updating the vendored submodules).

**Windows**
```console
> mingw32-make setup
```

**macOS / Linux**
```console
$ make setup
```

### Build the server and client

The project builds two separate executables from a shared codebase: `server` (no raylib dependency — pure networking/simulation) and `client` (the raylib-cpp game window).

**Windows**
```console
> mingw32-make server
> mingw32-make client
```
or build both at once:
```console
> mingw32-make
```

**macOS / Linux**
```console
$ make server
$ make client
```
or
```console
$ make
```

Binaries land in `bin/server.exe` / `bin/client.exe` (Windows) or `bin/server` / `bin/client` (macOS/Linux).

If a rebuild fails with a linker "Permission denied" (Windows), a previous `server.exe`/`client.exe` is still running and locking the binary — close it first.

## Running

### Start the server

```console
> bin\server.exe
```
The server binds UDP port `7777` and runs a fixed 60Hz simulation tick. One server process can host multiple simultaneous rooms.

### Start a client

```console
> bin\client.exe [server-ip]
```
`server-ip` defaults to `127.0.0.1` (localhost). Point remote clients at the server machine's LAN/public IP instead.

On launch, the client shows a room menu:
- **Create Room** — starts a new room and gets a 6-digit room code, using whichever game mode is currently selected on the mode-cycling control.
- **Join Room** — enter an existing room's 6-digit code to join it (inherits that room's mode; you can't pick a mode when joining).

Each room supports up to 4 players.

## Game modes

Cycle the mode on the room-creation menu before clicking Create Room:

- **FFA (Free-For-All)** — the core Hot Potato loop. Charge-and-release a throw, the potato flies with drag physics, auto-catches near any player, and has an escalating explosion timer that shrinks each time it's caught. Getting caught holding it when it explodes (or letting it fly out of bounds) Downs you and everyone else scores a point. Matches are best-of-3 rounds with sudden-death tiebreak.
- **2v2** — same Hot Potato loop, teams instead of individuals. Slots 0+1 are Team A, slots 2+3 are Team B (fixed, no picker). Scoring is team-based; the losing player's team gets scored against, the other team gets the point. Requires all 4 slots filled before any potato/scoring activity begins — an under-populated room shows "Waiting for 4 players...". Revive-potion items spawn outside the court instead of inside it.
- **Revive Test** — a lightweight 2-player mode with no potato at all. A hazard zone deals HP damage over time instead; a player who reaches 0 HP goes Downed, and if not revived in time, Dies and auto-respawns a few seconds later. Exists purely to test/exercise the Downed → revive → Alive flow in isolation.

## Controls

| Input | Action |
|---|---|
| `W` `A` `S` `D` | Move |
| Mouse (hold left, release) | Charge and throw the potato |
| `E` (hold) | Pick up a nearby item, or channel-revive a nearby Downed teammate |
| `E` (press) | Instantly use the selected hotbar item (self-heal, if nothing to revive) |
| `1` `2` `3` `4` | Select a hotbar slot |
| `Q` | Melee attack (Classic-era mechanic, not used by Hot Potato modes) |
| `Left Shift` / `Right Shift` | Dash a short distance in your current movement direction (2s cooldown) |
| `F1` | Toggle the debug menu (per-player kill/revive/heal/give-potion actions, session-wide "New Match" reset) |

## Gameplay features

- **Hotbar** — a 4-slot inventory shown at the bottom of the screen. Pick up Revive Potions from the ground; select a slot with `1`-`4`; press `E` to use it. If a Downed teammate is in range, `E` channels a revive (~2s); otherwise it instantly self-heals for a fixed amount and consumes one potion.
- **Dash** — a short-range movement burst that slides over a fraction of a second (not an instant teleport), with a cooldown. Dashing through a flying potato still catches it, even mid-slide.
- **Revive** — a Downed player has a limited window before they Die. A teammate holding a Revive Potion can channel a revive if standing close enough; a Dead player's fate then depends on the mode (auto-respawn in Revive Test, full round-reset in FFA/2v2).
- **Room codes** — 6-digit codes identify rooms; a server can host several rooms concurrently, each running its own independent simulation and player set.

## Project structure

```
src/
  shared/   Wire protocol structs, serialization, reliable-channel logic, UDP socket wrapper —
            compiled into both the server and client.
  server/   Server-authoritative simulation: session/player state, Hot Potato mechanics, combat,
            revive, hazards, match/round scoring, dash, debug actions. No raylib dependency.
  client/   raylib-cpp rendering, input capture, networking client, room menu, hotbar/HUD, debug menu.
docs/
  InstallingDependencies.md   Per-platform toolchain setup
  MakefileExplanation.md      How the build system works
  superpowers/specs/          Design docs for each major feature
  superpowers/plans/          Implementation plans for each feature/phase
```

## Tests

Both executables have a built-in smoke-test suite, run via a `--test` flag instead of launching normally:

```console
> bin\server.exe --test
> bin\client.exe --test
```

The server suite (`src/server/SmokeTests.h`) covers serialization, session management, combat/revive, hazards, the Hot Potato mechanics, matches/scoring, dash, game modes, and the hotbar/self-heal interaction — all exercised directly against the simulation functions, no live networking required. The client suite covers input-parsing helpers, visual-effects state, and room-menu logic.

## Design docs

Each major feature was designed and planned before implementation; see `docs/superpowers/specs/` for the design rationale and `docs/superpowers/plans/` for the task-by-task implementation plans, including the full Hot Potato build (5 phases: networking foundation, core mechanics, FFA scoring, dash, and 2v2/Revive Test modes) and the hotbar/item-use feature.

## Licence

This project is built on the [Raylib C++ Starter](https://github.com/CapsCollective/raylib-cpp-starter) template and licenced under an unmodified zlib/libpng licence — see [`LICENCE`](LICENCE).
