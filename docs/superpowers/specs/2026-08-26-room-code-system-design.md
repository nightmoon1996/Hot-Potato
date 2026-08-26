# Room Code System Design

## Goal

Let a player either create a new game room (server generates a 6-digit code) or join an existing one by typing that code, via a client-side pre-game menu, and show the current room code + connection state persistently during play.

## Non-goals

- No reconnect/stale-connection detection or "Reconnecting..." UI state — the client has no existing mechanism for this (no snapshot-staleness timeout, `NetClient::Reconnect()` is currently unused), and building it is separate scope. The HUD's connection indicator only ever shows "Connected" once the client has connected.
- No lobby listing, spectating, or room browsing — a code must be typed in, not chosen from a list.
- No change to the 2-player-per-session limit, the simulation tick, or any gameplay logic.

## Context

Today, `src/client/main.cpp` connects immediately on launch using a free-text session name from `argv[1]` (default `"default"`), via `NetClient::Connect(ip, port, sessionName)`. Server-side, `SessionManager::HandleConnect` (`src/server/SessionManager.h`) already implements create-or-join-by-name: if the name doesn't exist, it's created and the caller becomes slot 0; if it exists with an empty slot, the caller joins as the other slot; if full, they're rejected. This logic is reused almost as-is — the only new behavior is what a room *name* is (a server-generated 6-digit code instead of arbitrary free text) and who is allowed to implicitly create one (only an explicit "Create Room" request, not any join-by-name-that-happens-to-not-exist).

## Wire Protocol Changes (`src/shared/Protocol.h`)

- `ConnectRequestMsg` (unchanged struct, changed meaning): `sessionName` empty (`""`) and `reconnectToken == 0` now means "create a new room — you choose the code." A non-empty `sessionName` means "join this exact room."
- `WelcomeMsg` gains one new field: `char roomCode[7]` (6 digits + null terminator), populated with the actual session name/code the client ended up in, whether by creating or joining. Placed at the end of the struct to avoid disturbing existing field offsets for anything that might assume the previous layout (though raw-memcpy serialization means append-only changes are always safe here).
- `RejectReason` gains a new enumerator `RoomNotFound`, used when a non-empty `sessionName` doesn't match any existing session.

## Server Changes

### `SessionManager::HandleConnect` (`src/server/SessionManager.h`)

Add a create-vs-join branch at the top, before the existing reconnect-token check:

- If `sessionName` is empty and `reconnectToken == 0`: generate a 6-digit code (see below), guaranteed not to collide with an existing session, and proceed with today's "Case 1: session doesn't exist" logic using that generated code as the session name. Returns `ConnectResult::Created` as today, with the generated code now also written into the outcome so the caller (server main loop) can put it in `WelcomeMsg`.
- If `sessionName` is non-empty: keep the existing reconnect-token check, then look up the session by name. If it exists, proceed with existing join-or-full logic unchanged. If it does NOT exist, return `ConnectResult::Rejected` with `RejectReason::RoomNotFound` — this is a behavior change from today, where a fresh non-empty name silently creates a session. Only the empty-name "create" path may create a session now.

Code generation: a simple retry loop — generate a random 6-digit numeric string (`"000000"`–`"999999"`, zero-padded, via `snprintf` or manual digit formatting from `std::rand() % 1000000`), check `sessions.find(code) == sessions.end()`, retry (bounded, e.g. 20 attempts, extremely unlikely to matter at this scale) if it collides.

`ConnectOutcome` gains a `std::string roomCode` field (the final session name, whether generated or the caller's input), set in every non-Rejected path.

### `src/server/main.cpp`

- In the `ConnectRequest` handling branch, `msg.sessionName` may now legitimately be empty (`""`) for a create request — pass it through to `HandleConnect` unchanged (it already does the right thing with an empty string via `std::string sessionName(msg.sessionName)`).
- When building `WelcomeMsg`, copy `outcome.roomCode` into the new `welcome.roomCode` field (bounded copy, same pattern as existing `strncpy` usage for `ConnectRequestMsg::sessionName`).
- The `Rejected` response path already forwards `outcome.rejectReason` — no change needed there beyond `RoomNotFound` being a valid value now.

## Client Changes

### `NetClient` (`src/client/NetClient.h` / `.cpp`)

- Replace the single `Connect(ip, port, sessionName)` entry point with two: `bool CreateRoom(const std::string& ip, uint16_t port)` (calls `AttemptConnect` with `sessionName = ""`) and `bool JoinRoom(const std::string& ip, uint16_t port, const std::string& code)` (calls `AttemptConnect` with `sessionName = code`). Both set `serverIp`/`serverPort`/`sessionName` exactly as `Connect` does today, then delegate to the existing `AttemptConnect(0)`.
- `AttemptConnect` gains one new bit of state on a `Rejected` response: store the `RejectReason` it received (new private member `lastRejectReason`), so the menu can distinguish `RoomNotFound` from `SessionFull` for its error message.
- New getters: `const char* GetRoomCode() const` (from `gameConstants.roomCode`, populated once `Welcome` arrives) and `RejectReason GetLastRejectReason() const`.
- `Reconnect()` is unaffected (still uses the stored `sessionToken`/`sessionName` from whichever Create/Join call happened first).

### `RoomMenu` (new: `src/client/RoomMenu.h`)

A pre-game screen class, structurally mirroring `DebugMenu`'s plain-rectangle-button style (no new visual language introduced):

- State: a text buffer for the code being typed (digits only, max 6 chars), an error message string (empty when none), and a `bool done` flag once connected.
- `Draw()`: renders two sections — a "Create Room" button, and a "Join Room" row (a text-input-style box showing the typed digits + a "Join" button). Below both, the error message in red if set.
- `HandleInput(NetClient&)`: on Create-button click, calls `netClient.CreateRoom(...)`; on Join-button click or Enter (only if exactly 6 digits typed), calls `netClient.JoinRoom(..., code)`. Both are blocking calls (matching `AttemptConnect`'s existing blocking retry-loop design — this is consistent with how the game already connects today, just deferred until the button press instead of at startup). On success, sets `done = true`. On failure, sets the error message from `netClient.GetLastRejectReason()` (`"Room not found"` / `"Room is full"` / generic `"Connection failed"` fallback) and stays on the menu.
- Digit-only text input: use raylib's `GetCharPressed()` loop, filtering to `'0'`–`'9'`, appending until length 6; `KEY_BACKSPACE` removes the last character.

### `main.cpp`

- Window (`raylib::Window`) now opens BEFORE any connection attempt (previously connection happened before the window existed).
- After opening the window, run a loop driving `RoomMenu::Draw()`/`HandleInput()` each frame until `RoomMenu::done` is true, before entering the existing main game loop. This reuses the same `while (!w.ShouldClose())`-style loop shape, just gated on menu completion first.
- Once connected, the existing game loop proceeds completely unchanged, with one addition: a persistent HUD line drawn alongside the existing "You are slot N..." text: `TextFormat("Room: %s | Connected", netClient.GetRoomCode())`.

## Testing

- Server: extend `SmokeTestSessionManager` (`src/server/main.cpp`) with:
  - Create request (empty name) generates a valid-looking 6-digit code and creates a session under it.
  - Two sequential create requests generate different codes (no collision in the common case).
  - Joining a non-empty name that doesn't exist returns `Rejected` with `RoomNotFound`.
  - Joining a non-empty name that DOES exist (created via the create path) still works via the existing join logic, confirming the two paths compose correctly.
- Client: `RoomMenu`'s input-filtering logic (digit-only, 6-char cap, backspace) can get a small standalone smoke test in the existing `--test` harness (`JuiceTests.h`-style, or a new `RoomMenuTests.h`) since it's pure state logic with no rendering dependency. The connect/create/join network flow itself is verified live: create a room on one client, note the code, join it from a second client, confirm both see the same room code in the HUD and can play together; also verify joining a made-up code shows "Room not found" and stays on the menu.

## Open items carried into the plan

None — scope is fully bounded. The implementation plan should sequence as: (1) protocol field additions (non-breaking, additive), (2) server-side create/join-by-code logic + smoke tests, (3) NetClient CreateRoom/JoinRoom split + reject-reason tracking, (4) RoomMenu class + its own smoke test, (5) main.cpp integration (window-first restructuring + HUD line).
