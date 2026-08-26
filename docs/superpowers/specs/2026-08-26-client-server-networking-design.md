# Client/Server Networking — Design Spec

Date: 2026-08-26
Status: Approved for implementation planning

## Purpose

Convert the existing single-process, local same-screen 2-player game
(built in the prior pickup/inventory/revive feature — see
[2026-08-26-pickup-inventory-revive-design.md](2026-08-26-pickup-inventory-revive-design.md))
into a proper client/server architecture: a headless authoritative C++
server process that owns all game simulation, and two separate raylib
client processes (one per player) that connect to it over a network,
send input, and render server-authoritative state.

This spec covers networking only. Client-side visual polish ("juice":
screen shake, hit flash/damage numbers, tweened HP bars, particle
bursts, and the dot-ring revive indicator) is a separate, subsequent
spec/plan, built after this one so the polish work lands on the final
client rendering shape rather than being reworked mid-migration.

## Why Authoritative Server

The server is 100% authoritative: it owns the only real `Player`,
`WorldItem`, and `HazardZone` state, runs the entire simulation
(movement, damage, state machine, timers, combat, revive), and is the
only place game logic executes. Clients never mutate game state
directly — they send *intent* (input, debug-action requests) and
receive *truth* (snapshots). This is the standard pattern used by most
competitive multiplayer games (e.g. Counter-Strike, Valorant) because a
client cannot desync or cheat by lying about state it doesn't own.

No client-side prediction is implemented in this pass — the client is
fully passive, rendering exactly what the server's snapshots say, with
no local simulation of its own player's movement. On localhost/LAN the
round-trip latency is on the order of a few milliseconds, which is not
perceptible; prediction can be added later without changing the wire
protocol, since it is purely a client-side reconciliation concern.

## Repository & Build Layout

Same repository, no submodule/separate-repo split (avoids
synchronizing a shared protocol definition across two repos for a
project this size).

```
src/
  shared/
    Protocol.h            — message type enum, wire message structs
    Serialize.h/.cpp       — pack/unpack structs to/from raw byte buffers
    Socket.h/.cpp           — cross-platform UDP socket wrapper
                              (#ifdef _WIN32 Winsock, else BSD sockets)
    ReliableChannel.h/.cpp  — seq/ack tracking, retransmit timers,
                              in-order delivery for the reliable channel
  server/
    Session.h/.cpp          — one game session: 2 player slots + tokens
                              + the simulation (reuses Player/Item/
                              Hazard/Combat logic from the prior feature)
    SessionManager.h/.cpp    — session-name -> Session map, create-or-
                              join, reconnect-by-token matching
    main.cpp                 — headless: socket recv loop, dispatch to
                              SessionManager, 60Hz tick + broadcast
  client/
    NetClient.h/.cpp         — connect/reconnect flow, sends input and
                              debug-action requests, buffers latest
                              received snapshot
    main.cpp                 — raylib window: reads NetClient's latest
                              snapshot, renders, sends input every frame
```

The existing `Player.h`, `Item.h`, `Hazard.h`, `Combat.h` (the full
gameplay simulation from the prior feature) move to be used by
`src/server/` only — the client has no need for simulation logic, only
the rendered fields carried in a snapshot. `DebugMenu.h` moves to
`src/client/` and changes from directly mutating a local `Player` to
sending a `DebugActionRequest` message; the actual state-transition
logic (`Kill()`, `ReviveFromDowned()`, etc.) is unchanged and stays
server-side.

### Build changes

The current `Makefile` compiles every `.cpp` under `src/` into a single
`bin/app` binary. This must change to produce two binaries:

- `bin/server` — compiles `src/shared/*.cpp` + `src/server/*.cpp` only.
  Does **not** link raylib (headless, no window, no rendering).
- `bin/client` — compiles `src/shared/*.cpp` + `src/client/*.cpp` only.
  Links raylib exactly as `bin/app` does today.

Two new Makefile targets, `server` and `client`, replace the single
`all`/`bin/app` target (or coexist alongside it during migration — the
implementation plan will decide the exact transition). Running one
`bin/server` and two `bin/client` instances (each a separate OS
process) reproduces the 2-player experience, now over the network
instead of one shared keyboard.

## Wire Protocol

Every UDP packet begins with a fixed header:

```
struct PacketHeader {
    uint8_t  channel;     // 0 = unreliable-sequenced, 1 = reliable-ordered
    uint32_t seq;         // per-channel, per-sender sequence number
    uint16_t payloadLen;  // length of the payload that follows
};
```

### Channel 0: unreliable-sequenced

Used for per-tick state that is naturally superseded by the next
message — no retransmission needed, since a dropped packet's
information is obsolete by the time a resend would arrive anyway.

- **Client → Server, `InputMessage`** (sent every client frame):
  movement direction (x, y each -1/0/1), `interactHeld` (bool),
  `attackPressed` (bool, edge-triggered this frame), `reviveHeld`
  (bool — same physical key as interact, see note below).
- **Server → Client, `SnapshotMessage`** (sent every server tick, to
  both connected clients in a session): for each of the 2 player
  slots — position, hp, `PlayerState`, inventory potion count,
  `channelTimer`; plus each `WorldItem`'s active flag; plus the
  hazard zone bounds (static, but included for a self-contained
  snapshot).

Receiver behavior: track the highest `seq` seen per (channel, sender)
pair. A received packet with `seq` less than or equal to the
highest already seen is dropped silently (stale, duplicate, or
reordered-and-superseded). No acks, no retransmits on this channel.

### Channel 1: reliable-ordered

Used for one-off events that must never be silently lost, and must be
processed in the order they were sent.

- **Client → Server:**
  - `ConnectRequest { sessionName, reconnectToken (optional, zeroed if absent) }`
  - `DebugActionRequest { action (enum: Kill/Revive/HealFull/GivePotion/ForceDown), targetSlot }`
  - `DisconnectNotice` (best-effort graceful disconnect on client exit)
- **Server → Client:**
  - `Welcome { playerSlot, sessionToken, gameConstants (the numeric
    defaults table from the prior spec, sent once so the client can
    label UI/debug menu correctly) }`
  - `Rejected { reason (enum: SessionFull, InvalidToken) }`
  - `Ack { seq }` (acknowledges a specific reliable message)

Sender behavior: every reliable message is kept in a per-peer resend
buffer until acked. If no `Ack` arrives within 100ms (suitable for
LAN/localhost), the message is retransmitted. Receiver behavior: every
reliable message is acked on receipt (even if it is a duplicate — acks
are themselves idempotent to send). Reliable messages are delivered to
the application layer strictly in `seq` order — an out-of-order
arrival is buffered until the gap is filled by the missing lower-`seq`
message arriving (whether original or retransmitted).

### Note on the interact/revive key overload

The prior spec's client key bindings overload one physical key (E /
RightCtrl) for pickup, revive-channel initiation, and holding to
continue channeling. Over the network this remains a client-side
concern only: the client still decides, using its last received
snapshot, whether to prioritize sending "the other player is Downed
and in range" style intent — but since the server is authoritative,
the simplest correct approach is: the client always sends its raw
`interactHeld` state every frame, and the **server** replicates the
exact same priority logic already implemented in the prior feature's
`main.cpp` (skip pickup attempt if a valid Downed revive target is in
range) — because only the server has the authoritative positions and
states needed to decide this correctly. This logic moves from the old
single-process `main.cpp` into the server's per-tick simulation step,
unchanged in behavior.

## Session & Reconnect

`SessionManager` holds `std::map<std::string, Session>`, keyed by
session name (see Sessions below). A `Session` has exactly 2 player
slots. Each slot is in one of three states:

- **Empty** — no one has ever connected to this slot.
- **Connected** — actively receiving packets from a known client
  address; owns a `sessionToken` and the live `Player` simulation
  state.
- **Disconnected-pending** — was Connected, but no packet (of any
  kind, including `InputMessage`) has been received from that client's
  address for 60 seconds. The slot retains its `sessionToken` and the
  `Player`'s last state (position, hp, inventory, everything) exactly
  as it was; the `Player` is excluded from movement, damage, and timer
  updates while in this state (frozen, not deleted). If a further 60
  seconds pass with no reconnect, the slot transitions to Empty and
  the `Player` state is discarded.

### Connect handling (on `ConnectRequest`)

1. No `reconnectToken` (zeroed) + named session does not exist yet →
   create the session, assign the new client to slot 1, generate a
   fresh random `sessionToken`, reply `Welcome`.
2. No `reconnectToken` + session exists with an Empty slot → assign
   that slot, generate a fresh token, reply `Welcome`.
3. No `reconnectToken` + session exists but both slots are
   Connected/Disconnected-pending (i.e. no Empty slot) → reply
   `Rejected{SessionFull}`.
4. `reconnectToken` present + matches a Disconnected-pending slot in
   the named session → reattach: mark that slot Connected again with
   the new client address, resume the `Player` exactly as it was
   (position/hp/state/inventory unchanged), reply `Welcome` with the
   same `playerSlot` as before.
5. `reconnectToken` present but does not match any Disconnected-pending
   slot (session doesn't exist, wrong session, already reconnected by
   someone else, or slot expired to Empty) → fall back to case 1/2/3
   logic as if no token were supplied.

### Sessions

Sessions are created implicitly by the first client to connect with a
given `sessionName` (a client-supplied string) and joined by the
second client using the same name — no session listing/browsing;
players agree on a name out-of-band. The server can host multiple
independent sessions (each with its own 2 slots, its own simulation
tick, its own hazard zone / world items) simultaneously in the same
process.

## Debug Menu Over the Network

The client's F1 overlay still renders locally (using the last received
`SnapshotMessage` to know current HP/`PlayerState` for button
labels/state), but a button click sends `DebugActionRequest{action,
targetSlot}` over the reliable channel instead of calling a local
`Player` method directly. The server validates the request (slot
exists and is occupied, action is a recognized enum value) and applies
it to its authoritative `Player` using the exact same logic as the
prior feature's `DebugMenu::ApplyAction` (`Kill()`,
`ReviveFromDowned()`/`RespawnFull()`, Heal Full gated on Alive, Give
Potion unconditional, `ForceDown()` gated on Alive internally). The
result becomes visible to both clients on the next snapshot.

## Numeric Defaults (new, in addition to the prior spec's gameplay table)

| Value | Default |
|---|---|
| Server tick / snapshot rate | 60 Hz |
| Reliable-channel retransmit interval | 100ms |
| Disconnect detection timeout (no packets received) | 60s |
| Reconnect grace period after disconnect detected | 60s |

## Testing Approach

Consistent with the prior feature's approach: pure logic gets
`assert()`-based smoke tests that don't require a real socket.
Specifically:

- **Serialization round-trip**: pack a message struct to bytes, unpack
  it, assert the result equals the original — for each message type.
- **Reliable channel logic**: using in-memory byte buffers standing in
  for the wire (no real socket), verify seq/ack tracking, retransmit
  triggering after the timeout, duplicate-ack handling, and in-order
  delivery/buffering of an out-of-order arrival.
- **Unreliable channel logic**: verify a lower-or-equal `seq` is
  dropped and a higher `seq` is accepted, using in-memory sequences
  with no real socket.
- **SessionManager state machine**: verify all 5 connect-handling
  cases above (fresh create, join open slot, reject when full,
  reconnect success, reconnect-token-miss fallback) and the
  Connected → Disconnected-pending → Empty timeout transitions, all
  driven by directly calling the manager's methods with fake
  timestamps (no real time.sleep, no real sockets).

Actual end-to-end socket communication between real processes is
verified manually: build `bin/server` and `bin/client`, run one server
and two clients, and confirm the 2-player experience matches the
prior single-process version's manual test scenarios (movement,
pickup, hazard, PvP, revive with progress ring, debug menu), now
running as separate processes over localhost UDP. Reconnect is
verified by killing and restarting a client mid-session and confirming
it resumes with its prior state. Multi-session is verified by running
a third client pair with a different session name and confirming no
cross-session interference.
