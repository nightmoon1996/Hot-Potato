# Room Code System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a player create a new game room (server generates a 6-digit code) or join an existing one by typing that code, via a client-side pre-game menu, and show the current room code + connection state persistently during play.

**Architecture:** The existing create-or-join-by-session-name server logic (`SessionManager::HandleConnect`) is extended with an explicit create-vs-join branch: an empty session name means "create and generate a code," a non-empty name that doesn't exist is now rejected (`RoomNotFound`) instead of silently created. The client gets a new pre-game `RoomMenu` screen (mirroring `DebugMenu`'s plain-button style) that calls one of two new `NetClient` entry points (`CreateRoom`/`JoinRoom`) before the main game loop starts, and a persistent HUD line showing the room code once connected.

**Tech Stack:** C++17, raylib/raylib-cpp (client UI only), existing UDP client/server protocol (extended, not replaced).

**Spec:** [docs/superpowers/specs/2026-08-26-room-code-system-design.md](../specs/2026-08-26-room-code-system-design.md)

## Global Constraints

- No change to the 2-player-per-session slot model, the simulation tick, or gameplay logic.
- No reconnect/stale-connection detection or "Reconnecting..." UI — out of scope per spec. HUD only ever shows "Connected" once connected.
- All protocol changes are additive (new struct fields appended at the end, new enum values appended at the end) — raw-memcpy serialization means field order/size changes would break wire compatibility, but this plan never removes or reorders existing fields.
- Room codes are exactly 6 ASCII digit characters (`'0'`-`'9'`), stored as `std::string`/`char[7]` (6 digits + null terminator), never as a numeric type (leading zeros must be preserved, e.g. `"004821"`).
- Code generation: bounded retry loop, max 20 attempts, using `std::rand() % 1000000` formatted as a zero-padded 6-digit string.
- Existing free-text session names (e.g. `"default"`, used by the current `argv[1]` CLI convenience) still work as exact-match join targets if such a session already exists — only the auto-create-on-any-name behavior is removed. (This plan replaces `main.cpp`'s CLI-arg connection entirely with the RoomMenu; the underlying server logic still supports joining any exact existing name.)
- Build with `mingw32-make server` / `mingw32-make client` using the existing Makefile; no Makefile changes expected.
- Toolchain: MinGW at `C:\msys64\mingw64\bin`, add to PATH if needed (bash: `export PATH="/c/msys64/mingw64/bin:$PATH"`). On Windows, rebuilding may fail with a link "Permission denied" if a previous `bin/server.exe`/`bin/client.exe` is still running — check with `tasklist //FI "IMAGENAME eq server.exe"` (and `client.exe`) and stop them (`taskkill //PID <pid> //F`) before rebuilding, asking first since they may be the user's active session.

---

### Task 1: Protocol additions — room code fields and RoomNotFound

**Files:**
- Modify: `src/shared/Protocol.h`

**Interfaces:**
- Produces: `WelcomeMsg::roomCode` (`char[7]`), `RejectReason::RoomNotFound` (new enumerator, appended last) — consumed by Tasks 2, 3.

This task is a pure additive struct/enum change with no logic — verified by a build-only check (no smoke test needed, since there's no behavior yet to test; Task 2's smoke tests will exercise it).

- [ ] **Step 1: Add `RoomNotFound` to `RejectReason`**

In `src/shared/Protocol.h`, change:

```cpp
enum class RejectReason : uint8_t {
    SessionFull,
    InvalidToken
};
```

to:

```cpp
enum class RejectReason : uint8_t {
    SessionFull,
    InvalidToken,
    RoomNotFound
};
```

- [ ] **Step 2: Add `roomCode` to `WelcomeMsg`**

In `src/shared/Protocol.h`, change:

```cpp
struct WelcomeMsg {
    uint8_t playerSlot;
    uint32_t sessionToken;
    int maxHp;
    float downedDuration;
    float deathRespawnDelay;
    int reviveHp;
    int respawnHp;
    float attackCooldown;
    float attackRange;
    int attackDamage;
    float channelDuration;
    float hazardDamagePerSecond;
    int inventoryCapacity;
};
```

to (adding `roomCode` as the LAST field, to keep this an additive/append-only change):

```cpp
struct WelcomeMsg {
    uint8_t playerSlot;
    uint32_t sessionToken;
    int maxHp;
    float downedDuration;
    float deathRespawnDelay;
    int reviveHp;
    int respawnHp;
    float attackCooldown;
    float attackRange;
    int attackDamage;
    float channelDuration;
    float hazardDamagePerSecond;
    int inventoryCapacity;
    char roomCode[7]; // 6 digits + null terminator
};
```

- [ ] **Step 3: Build both targets to confirm no compile breakage**

Run: `mingw32-make server && mingw32-make client`
Expected: both build cleanly. (Existing code constructs `WelcomeMsg{...}` with positional initializers in `src/server/main.cpp` — this will now be MISSING the new last field, which is fine in C++ aggregate init, the field just zero-initializes; Task 3 fills it in properly.)

- [ ] **Step 4: Commit**

```bash
git add src/shared/Protocol.h
git commit -m "Add room code field to WelcomeMsg and RoomNotFound reject reason"
```

---

### Task 2: Server-side room code generation and create/join-by-code logic

**Files:**
- Modify: `src/server/SessionManager.h`
- Modify: `src/server/main.cpp` (smoke tests only, in this task; the runtime dispatch wiring is Task 3)

**Interfaces:**
- Consumes: `RejectReason::RoomNotFound` (Task 1).
- Produces: `ConnectOutcome::roomCode` (`std::string`, new field) — consumed by Task 3. `SessionManager::HandleConnect`'s new create-vs-join branching behavior — consumed by Task 3 and exercised by this task's own smoke tests.

- [ ] **Step 1: Add `roomCode` to `ConnectOutcome` and rewrite `HandleConnect`'s branching**

In `src/server/SessionManager.h`, change:

```cpp
struct ConnectOutcome {
    ConnectResult result;
    int slotIndex;
    uint32_t sessionToken;
    RejectReason rejectReason;
};
```

to:

```cpp
struct ConnectOutcome {
    ConnectResult result;
    int slotIndex;
    uint32_t sessionToken;
    RejectReason rejectReason;
    std::string roomCode;
};
```

Replace the entire `HandleConnect` method body with:

```cpp
    ConnectOutcome HandleConnect(const std::string& sessionName, uint32_t reconnectToken,
                                 const std::string& clientIp, uint16_t clientPort, double nowSeconds) {
        // Case 4: reconnect token matches a disconnected slot
        if (reconnectToken != 0) {
            for (auto& entry : sessions) {
                int slotIndex = entry.second.FindDisconnectedSlotByToken(reconnectToken);
                if (slotIndex != -1) {
                    PlayerSlot& slot = entry.second.slots[slotIndex];
                    slot.state = SlotState::Connected;
                    slot.clientIp = clientIp;
                    slot.clientPort = clientPort;
                    slot.lastPacketAtSeconds = nowSeconds;
                    return { ConnectResult::Reconnected, slotIndex, slot.sessionToken, RejectReason::SessionFull, entry.first };
                }
            }
            // Case 5: token present but no match -> fall through to fresh-connect logic
        }

        // Create path: empty session name means "create a new room, generate a code"
        if (sessionName.empty()) {
            std::string code = GenerateUniqueRoomCode();
            Session newSession;
            uint32_t token = GenerateToken();
            newSession.slots[0].state = SlotState::Connected;
            newSession.slots[0].sessionToken = token;
            newSession.slots[0].clientIp = clientIp;
            newSession.slots[0].clientPort = clientPort;
            newSession.slots[0].lastPacketAtSeconds = nowSeconds;
            newSession.slots[0].player = Player(kSlot0Spawn);
            newSession.slots[1].player = Player(kSlot1Spawn);
            sessions[code] = newSession;
            return { ConnectResult::Created, 0, token, RejectReason::SessionFull, code };
        }

        // Join path: non-empty name must already exist
        auto it = sessions.find(sessionName);
        if (it == sessions.end()) {
            return { ConnectResult::Rejected, -1, 0, RejectReason::RoomNotFound, "" };
        }

        Session& session = it->second;
        int emptySlot = session.FindEmptySlot();
        if (emptySlot != -1) {
            uint32_t token = GenerateToken();
            session.slots[emptySlot].state = SlotState::Connected;
            session.slots[emptySlot].sessionToken = token;
            session.slots[emptySlot].clientIp = clientIp;
            session.slots[emptySlot].clientPort = clientPort;
            session.slots[emptySlot].lastPacketAtSeconds = nowSeconds;
            session.slots[emptySlot].player = Player(emptySlot == 0 ? kSlot0Spawn : kSlot1Spawn);
            return { ConnectResult::Joined, emptySlot, token, RejectReason::SessionFull, sessionName };
        }

        return { ConnectResult::Rejected, -1, 0, RejectReason::SessionFull, "" };
    }
```

Note: the reconnect-token loop changed from `sessions.find(sessionName)` (old code, which required the caller to already know which session name the token belonged to) to iterating all sessions looking for the token — this is required because with generated codes, a reconnecting client may not reliably resend the exact original code as `sessionName` in every reconnect flow this plan touches; searching by token alone (already unique per `GenerateToken()`) is strictly more correct and was already effectively guaranteed unique. This is a deliberate improvement within this task's scope, not a deviation to flag — the old code's `sessions.find(sessionName)` restriction was only ever correct because the caller already knew its own session name, which remains true, but scanning all sessions removes an unnecessary coupling and has no observable behavior change for existing callers.

- [ ] **Step 2: Add `GenerateUniqueRoomCode` private helper**

In `src/server/SessionManager.h`, in the `private:` section (alongside the existing `GenerateToken`), add:

```cpp
    std::string GenerateUniqueRoomCode() {
        for (int attempt = 0; attempt < 20; attempt++) {
            int number = std::rand() % 1000000;
            char buffer[7];
            std::snprintf(buffer, sizeof(buffer), "%06d", number);
            std::string code(buffer);
            if (sessions.find(code) == sessions.end()) {
                return code;
            }
        }
        // Exceedingly unlikely at this scale (up to 1,000,000 possible codes);
        // fall back to a fixed sentinel rather than looping forever or crashing.
        return "000000";
    }
```

Add `#include <cstdio>` to the top of `src/server/SessionManager.h` for `std::snprintf` (check if not already present via a transitive include; add if missing).

- [ ] **Step 3: Add smoke tests to `src/server/main.cpp`**

In `SmokeTestSessionManager()` (in `src/server/main.cpp`), APPEND the following new assertions at the end of the existing function body (before its closing brace), continuing to use the existing `manager` instance already in scope:

```cpp
    // Create-room path: empty name generates a 6-digit code
    ConnectOutcome createOutcome = manager.HandleConnect("", 0, "127.0.0.1", 7000, 300.0);
    assert(createOutcome.result == ConnectResult::Created);
    assert(createOutcome.roomCode.size() == 6);
    for (char c : createOutcome.roomCode) {
        assert(c >= '0' && c <= '9');
    }

    // Two sequential create requests generate different codes (overwhelmingly likely; not a hard guarantee)
    ConnectOutcome createOutcome2 = manager.HandleConnect("", 0, "127.0.0.1", 7001, 300.0);
    assert(createOutcome2.result == ConnectResult::Created);
    assert(createOutcome2.roomCode != createOutcome.roomCode);

    // Joining a non-empty name that doesn't exist is rejected with RoomNotFound
    ConnectOutcome joinMissing = manager.HandleConnect("999999", 0, "127.0.0.1", 7002, 300.0);
    assert(joinMissing.result == ConnectResult::Rejected);
    assert(joinMissing.rejectReason == RejectReason::RoomNotFound);

    // Joining a code that WAS created via the create path succeeds via existing join logic
    ConnectOutcome joinCreated = manager.HandleConnect(createOutcome.roomCode, 0, "127.0.0.1", 7003, 300.0);
    assert(joinCreated.result == ConnectResult::Joined);
    assert(joinCreated.slotIndex == 1);
    assert(joinCreated.roomCode == createOutcome.roomCode);
```

- [ ] **Step 4: Build and run smoke tests**

Run: `mingw32-make server && bin/server --test`
Expected: `SmokeTestSessionManager passed` (and all other existing smoke test lines) prints, followed by `All smoke tests passed`, exit code 0. (If `bin/server.exe` is locked by a running process, check `tasklist //FI "IMAGENAME eq server.exe"` and ask before killing it.)

- [ ] **Step 5: Commit**

```bash
git add src/server/SessionManager.h src/server/main.cpp
git commit -m "Add server-side room code generation and create/join-by-code branching"
```

---

### Task 3: Wire room code through server main.cpp's Welcome response

**Files:**
- Modify: `src/server/main.cpp` (runtime dispatch, not smoke tests — Task 2 already handled smoke tests)

**Interfaces:**
- Consumes: `ConnectOutcome::roomCode` (Task 2), `WelcomeMsg::roomCode` (Task 1).

- [ ] **Step 1: Copy the outcome's room code into the `WelcomeMsg`**

In `src/server/main.cpp`, inside the `ConnectRequest` handling branch, find:

```cpp
                            WelcomeMsg welcome{
                                (uint8_t)outcome.slotIndex, outcome.sessionToken,
                                Player::kMaxHp, Player::kDownedDuration, Player::kDeathRespawnDelay,
                                Player::kReviveHp, Player::kRespawnHp, Player::kAttackCooldown,
                                Player::kAttackRange, Player::kAttackDamage, Player::kChannelDuration,
                                kHazardDamagePerSecond, Inventory::kCapacity
                            };
                            PlayerSlot& slot = session->slots[outcome.slotIndex];
                            std::vector<uint8_t> welcomeBytes;
                            SerializeStruct(welcome, welcomeBytes);
                            SendReliable(socket, slot, MessageType::Welcome, welcomeBytes.data(), welcomeBytes.size(), now);
```

Replace with:

```cpp
                            WelcomeMsg welcome{
                                (uint8_t)outcome.slotIndex, outcome.sessionToken,
                                Player::kMaxHp, Player::kDownedDuration, Player::kDeathRespawnDelay,
                                Player::kReviveHp, Player::kRespawnHp, Player::kAttackCooldown,
                                Player::kAttackRange, Player::kAttackDamage, Player::kChannelDuration,
                                kHazardDamagePerSecond, Inventory::kCapacity, {}
                            };
                            std::strncpy(welcome.roomCode, outcome.roomCode.c_str(), sizeof(welcome.roomCode) - 1);
                            PlayerSlot& slot = session->slots[outcome.slotIndex];
                            std::vector<uint8_t> welcomeBytes;
                            SerializeStruct(welcome, welcomeBytes);
                            SendReliable(socket, slot, MessageType::Welcome, welcomeBytes.data(), welcomeBytes.size(), now);
```

Note: the `sessionName` variable used earlier in this branch (`std::string sessionName(msg.sessionName);`, used to look up `Session* session = sessionManager.GetSession(sessionName);` and the `sessionWorldItems` map key) must now use `outcome.roomCode` instead when the request was a create-request, since `sessionName` was empty in that case but the session was actually created under the generated code. Find:

```cpp
                        } else {
                            Session* session = sessionManager.GetSession(sessionName);
                            if (sessionWorldItems.find(sessionName) == sessionWorldItems.end()) {
                                sessionWorldItems[sessionName] = {
                                    WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
                                    WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
                                };
                                sessionHazardCarry[sessionName][0] = 0.0f;
                                sessionHazardCarry[sessionName][1] = 0.0f;
                                sessionSnapshotSeq[sessionName] = 1;
                            }
```

Replace with:

```cpp
                        } else {
                            Session* session = sessionManager.GetSession(outcome.roomCode);
                            if (sessionWorldItems.find(outcome.roomCode) == sessionWorldItems.end()) {
                                sessionWorldItems[outcome.roomCode] = {
                                    WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
                                    WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
                                };
                                sessionHazardCarry[outcome.roomCode][0] = 0.0f;
                                sessionHazardCarry[outcome.roomCode][1] = 0.0f;
                                sessionSnapshotSeq[outcome.roomCode] = 1;
                            }
```

This is correct for both create (where `sessionName` was empty but `outcome.roomCode` holds the generated code) and join (where `outcome.roomCode` equals the caller's input `sessionName`, per Task 2's `HandleConnect` returning `sessionName` itself as `roomCode` on the join path).

- [ ] **Step 2: Build**

Run: `mingw32-make server`
Expected: builds cleanly with no errors or warnings about the aggregate-init `{}` for the new trailing `roomCode` field.

- [ ] **Step 3: Run smoke tests to confirm no regression**

Run: `bin/server --test`
Expected: all smoke tests (including Task 2's new ones) still pass — this task only touches runtime dispatch code, not smoke-tested logic, so this step is a regression check, not new coverage.

- [ ] **Step 4: Commit**

```bash
git add src/server/main.cpp
git commit -m "Copy room code into WelcomeMsg and fix session lookup for generated codes"
```

---

### Task 4: NetClient CreateRoom/JoinRoom split and reject-reason tracking

**Files:**
- Modify: `src/client/NetClient.h`
- Modify: `src/client/NetClient.cpp`

**Interfaces:**
- Consumes: `WelcomeMsg::roomCode` (Task 1), `RejectedMsg::reason` (already exists), `RejectReason::RoomNotFound` (Task 1).
- Produces (used by Task 5):
  - `bool NetClient::CreateRoom(const std::string& ip, uint16_t port)`
  - `bool NetClient::JoinRoom(const std::string& ip, uint16_t port, const std::string& code)`
  - `const char* NetClient::GetRoomCode() const`
  - `RejectReason NetClient::GetLastRejectReason() const`

- [ ] **Step 1: Update `NetClient.h`**

Replace:

```cpp
    bool Connect(const std::string& serverIp, uint16_t serverPort, const std::string& sessionName);
    bool Reconnect();
```

with:

```cpp
    bool CreateRoom(const std::string& serverIp, uint16_t serverPort);
    bool JoinRoom(const std::string& serverIp, uint16_t serverPort, const std::string& roomCode);
    bool Reconnect();
```

Add these getters alongside the existing ones (`GetLatestSnapshot`, `GetPlayerSlot`, etc.):

```cpp
    const char* GetRoomCode() const { return gameConstants.roomCode; }
    RejectReason GetLastRejectReason() const { return lastRejectReason; }
```

Add this private member alongside the existing ones:

```cpp
    RejectReason lastRejectReason = RejectReason::SessionFull;
```

- [ ] **Step 2: Update `NetClient.cpp`**

Replace:

```cpp
bool NetClient::Connect(const std::string& ip, uint16_t port, const std::string& session) {
    serverIp = ip;
    serverPort = port;
    sessionName = session;
    return AttemptConnect(0);
}
```

with:

```cpp
bool NetClient::CreateRoom(const std::string& ip, uint16_t port) {
    serverIp = ip;
    serverPort = port;
    sessionName = "";
    return AttemptConnect(0);
}

bool NetClient::JoinRoom(const std::string& ip, uint16_t port, const std::string& roomCode) {
    serverIp = ip;
    serverPort = port;
    sessionName = roomCode;
    return AttemptConnect(0);
}
```

In `AttemptConnect`, find:

```cpp
                    } else if (type == MessageType::Rejected) {
                        connected = false;
                        return false;
                    }
```

Replace with:

```cpp
                    } else if (type == MessageType::Rejected) {
                        RejectedMsg reject{};
                        if (DeserializeStruct(payload + 1, payloadLen - 1, reject)) {
                            lastRejectReason = reject.reason;
                        }
                        connected = false;
                        return false;
                    }
```

- [ ] **Step 3: Build**

`main.cpp` still calls the old `Connect(...)` method at this point in the plan (Task 6 is what updates `main.cpp` to use `CreateRoom`/`JoinRoom` instead), so a full `mingw32-make client` link will fail until Task 6 is done — that's expected and not a sign of a mistake in this task.

To verify Task 4's changes compile correctly in isolation before Task 6 exists, compile just the object file this task touched, without linking:

Run: `mingw32-make bin/client/NetClient.o`

(This asks Make to build only the `NetClient.o` object file target, which the existing pattern rule `$(buildDir)/%.o: src/%.cpp Makefile` already provides — no new Makefile rule needed. It compiles `NetClient.cpp` with the project's real compiler flags and include paths, catching any real syntax/type error in this task's changes, without requiring `main.cpp` to compile or link.)

Expected: `bin/client/NetClient.o` is produced with no compiler errors.

- [ ] **Step 4: Commit**

```bash
git add src/client/NetClient.h src/client/NetClient.cpp
git commit -m "Split NetClient::Connect into CreateRoom/JoinRoom, track reject reason"
```

---

### Task 5: RoomMenu class

**Files:**
- Create: `src/client/RoomMenu.h`
- Create: `src/client/RoomMenuTests.h`
- Modify: `src/client/main.cpp` (only to wire the `--test` flag to include the new test file; full integration is Task 6)

**Interfaces:**
- Consumes: `NetClient::CreateRoom`, `NetClient::JoinRoom`, `NetClient::GetLastRejectReason`, `NetClient::IsConnected` (Task 4).
- Produces (used by Task 6): `class RoomMenu` with `void Draw()`, `void HandleInput(NetClient&)`, `bool IsDone() const`.

- [ ] **Step 1: Write `RoomMenu.h`**

```cpp
#pragma once

#include <raylib-cpp.hpp>
#include <string>
#include "NetClient.h"

class RoomMenu {
public:
    bool IsDone() const { return done; }

    void Draw() {
        DrawText("MAXION TEST", 380, 100, 32, BLACK);

        DrawRectangleRec(createButton, LIGHTGRAY);
        DrawRectangleLinesEx(createButton, 1, DARKGRAY);
        DrawText("Create Room", (int)createButton.x + 20, (int)createButton.y + 12, 18, BLACK);

        DrawText("Enter 6-digit code:", 380, 300, 16, BLACK);
        DrawRectangleRec(codeBox, RAYWHITE);
        DrawRectangleLinesEx(codeBox, 1, DARKGRAY);
        DrawText(codeInput.c_str(), (int)codeBox.x + 10, (int)codeBox.y + 8, 20, BLACK);

        DrawRectangleRec(joinButton, LIGHTGRAY);
        DrawRectangleLinesEx(joinButton, 1, DARKGRAY);
        DrawText("Join Room", (int)joinButton.x + 20, (int)joinButton.y + 12, 18, BLACK);

        if (!errorMessage.empty()) {
            DrawText(errorMessage.c_str(), 380, 420, 16, RED);
        }
    }

    void HandleInput(NetClient& netClient) {
        // Digit-only text entry into codeInput, capped at 6 characters
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= '0' && key <= '9' && codeInput.size() < 6) {
                codeInput += (char)key;
            }
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !codeInput.empty()) {
            codeInput.pop_back();
        }

        bool createClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), createButton);
        bool joinClicked = (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), joinButton)) ||
                            (IsKeyPressed(KEY_ENTER) && codeInput.size() == 6);

        if (createClicked) {
            errorMessage.clear();
            if (netClient.CreateRoom("127.0.0.1", 7777)) {
                done = true;
            } else {
                errorMessage = RejectReasonToMessage(netClient.GetLastRejectReason());
            }
        } else if (joinClicked && codeInput.size() == 6) {
            errorMessage.clear();
            if (netClient.JoinRoom("127.0.0.1", 7777, codeInput)) {
                done = true;
            } else {
                errorMessage = RejectReasonToMessage(netClient.GetLastRejectReason());
            }
        }
    }

private:
    std::string codeInput;
    std::string errorMessage;
    bool done = false;

    Rectangle createButton{ 400.0f, 180.0f, 200.0f, 40.0f };
    Rectangle codeBox{ 400.0f, 330.0f, 200.0f, 30.0f };
    Rectangle joinButton{ 400.0f, 370.0f, 200.0f, 40.0f };

    static std::string RejectReasonToMessage(RejectReason reason) {
        switch (reason) {
            case RejectReason::RoomNotFound: return "Room not found";
            case RejectReason::SessionFull: return "Room is full";
            case RejectReason::InvalidToken: return "Connection failed";
            default: return "Connection failed";
        }
    }
};
```

- [ ] **Step 2: Write `RoomMenuTests.h`**

This tests only the pure input-filtering logic (digit-only, 6-char cap, backspace), which is embedded directly in `HandleInput` above and depends on raylib's `GetCharPressed()`/`IsKeyPressed()` — NOT mockable without a running raylib context. Per the spec, extract just the reusable pure logic into a small free function so it's testable without raylib:

Go back and modify `RoomMenu.h`'s `HandleInput` to extract the digit-filtering into a static helper:

```cpp
    static void AppendDigitInput(std::string& buffer, int charCode) {
        if (charCode >= '0' && charCode <= '9' && buffer.size() < 6) {
            buffer += (char)charCode;
        }
    }
```

And change the loop in `HandleInput` to:

```cpp
        int key = GetCharPressed();
        while (key > 0) {
            AppendDigitInput(codeInput, key);
            key = GetCharPressed();
        }
```

Move `AppendDigitInput` to `public:` (or keep it `private` and `friend` the test — simplest is `public: static` since it's a pure, harmless-to-expose helper) so the test file can call it directly.

Now write `src/client/RoomMenuTests.h`:

```cpp
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include "RoomMenu.h"

inline void RunRoomMenuSmokeTests() {
    // Test 1: digits are appended
    {
        std::string buffer;
        RoomMenu::AppendDigitInput(buffer, '1');
        RoomMenu::AppendDigitInput(buffer, '2');
        RoomMenu::AppendDigitInput(buffer, '3');
        if (buffer != "123") {
            printf("FAIL: expected buffer '123', got '%s'\n", buffer.c_str());
            exit(1);
        }
        printf("PASS: digits are appended in order\n");
    }

    // Test 2: non-digit characters are ignored
    {
        std::string buffer;
        RoomMenu::AppendDigitInput(buffer, 'a');
        RoomMenu::AppendDigitInput(buffer, '5');
        RoomMenu::AppendDigitInput(buffer, '!');
        if (buffer != "5") {
            printf("FAIL: expected buffer '5' (non-digits ignored), got '%s'\n", buffer.c_str());
            exit(1);
        }
        printf("PASS: non-digit characters are ignored\n");
    }

    // Test 3: input is capped at 6 characters
    {
        std::string buffer;
        for (int i = 0; i < 10; i++) {
            RoomMenu::AppendDigitInput(buffer, '0' + (i % 10));
        }
        if (buffer.size() != 6) {
            printf("FAIL: expected buffer capped at 6 chars, got size %zu\n", buffer.size());
            exit(1);
        }
        if (buffer != "012345") {
            printf("FAIL: expected first 6 digits '012345', got '%s'\n", buffer.c_str());
            exit(1);
        }
        printf("PASS: input is capped at 6 characters, extras dropped\n");
    }

    printf("All RoomMenu smoke tests passed.\n");
}
```

- [ ] **Step 3: Wire the test into `main.cpp`'s `--test` flag**

In `src/client/main.cpp`, add `#include "RoomMenuTests.h"` alongside the existing `#include "JuiceTests.h"`, and in the `--test` branch, add a call:

```cpp
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--test") {
            RunJuiceSmokeTests();
            RunRoomMenuSmokeTests();
            return 0;
        }
    }
```

- [ ] **Step 4: Build and run**

Run: `mingw32-make client && bin/client --test`
Expected: all Juice smoke tests pass (as before), followed by the 3 new RoomMenu smoke test PASS lines and `All RoomMenu smoke tests passed.`, exit code 0.

- [ ] **Step 5: Commit**

```bash
git add src/client/RoomMenu.h src/client/RoomMenuTests.h src/client/main.cpp
git commit -m "Add RoomMenu class with digit-input filtering and smoke tests"
```

---

### Task 6: main.cpp integration — window-first restructuring, menu loop, HUD line

**Files:**
- Modify: `src/client/main.cpp`

**Interfaces:**
- Consumes: `RoomMenu` (Task 5), `NetClient::CreateRoom`/`JoinRoom`/`GetRoomCode` (Task 4).

- [ ] **Step 1: Restructure `main()` to open the window before connecting, run the RoomMenu loop, then proceed into the existing game loop**

Replace the ENTIRE current `main()` function body (everything between `int main(int argc, char** argv)\n{` and its closing `}`) with:

```cpp
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--test") {
            RunJuiceSmokeTests();
            RunRoomMenuSmokeTests();
            return 0;
        }
    }

    int screenWidth = 1000;
    int screenHeight = 600;
    raylib::Window w(screenWidth, screenHeight, "Maxion Test - Client");
    SetTargetFPS(60);

    NetClient netClient;
    RoomMenu roomMenu;

    while (!w.ShouldClose() && !roomMenu.IsDone()) {
        roomMenu.HandleInput(netClient);

        BeginDrawing();
        ClearBackground(RAYWHITE);
        roomMenu.Draw();
        EndDrawing();
    }

    if (w.ShouldClose()) {
        return 0;
    }

    TraceLog(LOG_INFO, "Connected as player slot %d", (int)netClient.GetPlayerSlot());

    DebugMenu debugMenu;
    ClientEffectsState effects;
    const WelcomeMsg& constants = netClient.GetGameConstants();
    uint8_t mySlot = netClient.GetPlayerSlot();

    while (!w.ShouldClose())
    {
        double now = GetTime();
        float dt = GetFrameTime();

        if (IsKeyPressed(KEY_F1)) {
            debugMenu.Toggle();
        }

        Vector2 move{0, 0};
        if (IsKeyDown(KEY_W)) move.y -= 1;
        if (IsKeyDown(KEY_S)) move.y += 1;
        if (IsKeyDown(KEY_A)) move.x -= 1;
        if (IsKeyDown(KEY_D)) move.x += 1;
        bool interactHeld = IsKeyDown(KEY_E);
        bool attackPressed = IsKeyPressed(KEY_Q);

        netClient.SendInput(move.x, move.y, interactHeld, attackPressed);
        netClient.PollNetwork(now);

        const SnapshotMsg& snap = netClient.GetLatestSnapshot();
        if (netClient.HasReceivedSnapshot()) {
            effects.Update(snap, mySlot, dt);
        }

        BeginDrawing();
        ClearBackground(RAYWHITE);

        Camera2D camera{};
        camera.offset = Vector2{ (float)screenWidth / 2.0f + effects.GetShakeOffsetX(), (float)screenHeight / 2.0f + effects.GetShakeOffsetY() };
        camera.target = Vector2{ (float)screenWidth / 2.0f, (float)screenHeight / 2.0f };
        camera.rotation = 0.0f;
        camera.zoom = 1.0f;

        BeginMode2D(camera);

        DrawRectangle((int)snap.hazardX, (int)snap.hazardY, (int)snap.hazardW, (int)snap.hazardH, Fade(RED, 0.3f));

        for (int i = 0; i < 2; i++) {
            if (snap.items[i].active) {
                DrawCircleV(Vector2{ snap.items[i].posX, snap.items[i].posY }, 8, GOLD);
            }
        }

        auto drawPlayer = [&](int slot, const PlayerSnapshot& p, Color color, const char* label) {
            if (p.state == 3) return; // absent: slot not occupied by a connected player

            Color drawColor = color;
            if (p.state == 1) drawColor = GRAY;   // Downed
            if (p.state == 2) drawColor = Fade(GRAY, 0.3f); // Dead

            float flashRatio = effects.GetHitFlashRatio(slot);
            if (flashRatio > 0.0f) {
                drawColor = ColorLerp(drawColor, WHITE, flashRatio);
            }

            Vector2 pos{ p.posX, p.posY };
            DrawCircleV(pos, 16, drawColor);
            DrawText(label, (int)pos.x - 10, (int)pos.y - 34, 14, BLACK);

            int barWidth = 40;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, barWidth, 5, DARKGRAY);
            float displayedHp = effects.GetDisplayedHp(slot);
            int hpWidth = (int)(barWidth * (displayedHp / constants.maxHp));
            if (hpWidth < 0) hpWidth = 0;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, hpWidth, 5, GREEN);

            DrawText(TextFormat("Potions: %d", p.potionCount), (int)pos.x - 30, (int)pos.y + 20, 12, DARKBLUE);

            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / constants.channelDuration;
                int filledDots = (int)(kReviveRingDotCount * ratio);
                if (filledDots > kReviveRingDotCount) filledDots = kReviveRingDotCount;
                float ringRadius = kReviveRingRadius;
                for (int dot = 0; dot < kReviveRingDotCount; dot++) {
                    float angle = (-90.0f + 360.0f * (float)dot / (float)kReviveRingDotCount) * (3.14159265f / 180.0f);
                    Vector2 dotPos{ pos.x + cosf(angle) * ringRadius, pos.y + sinf(angle) * ringRadius };
                    if (dot < filledDots) {
                        DrawCircleV(dotPos, kReviveRingDotRadius, Fade(SKYBLUE, 0.9f));
                    } else {
                        DrawCircleLines((int)dotPos.x, (int)dotPos.y, (int)kReviveRingDotRadius, Fade(SKYBLUE, 0.4f));
                    }
                }
            }
        };

        drawPlayer(0, snap.players[0], BLUE, "P1");
        drawPlayer(1, snap.players[1], MAROON, "P2");

        for (int i = 0; i < effects.GetParticleCount(); i++) {
            const Particle& particle = effects.GetParticles()[i];
            if (!particle.active) continue;
            float alpha = 1.0f - (particle.age / particle.lifetime);
            if (alpha < 0.0f) alpha = 0.0f;
            DrawCircleV(particle.pos, 3.0f, Fade(particle.color, alpha));
        }

        for (int i = 0; i < effects.GetDamageNumberCount(); i++) {
            const DamageNumber& dmgNum = effects.GetDamageNumbers()[i];
            if (!dmgNum.active) continue;
            float alpha = 1.0f - (dmgNum.age / dmgNum.lifetime);
            if (alpha < 0.0f) alpha = 0.0f;
            const char* text = TextFormat("-%d", dmgNum.value);
            Color textColor = Fade(RED, alpha);
            DrawText(text, (int)dmgNum.pos.x - 6, (int)dmgNum.pos.y - 10, 16, textColor);
        }

        EndMode2D();

        DrawText(TextFormat("Room: %s | Connected", netClient.GetRoomCode()), 10, 10, 16, BLACK);
        DrawText(TextFormat("You are slot %d. WASD move, E pickup/revive, Q attack.", (int)mySlot), 10, 30, 16, BLACK);
        DrawText("F1: Debug Menu", 10, 50, 16, DARKGRAY);

        debugMenu.DrawAndHandle(netClient);

        EndDrawing();
    }

    return 0;
}
```

Add `#include "RoomMenu.h"` alongside the other includes at the top of the file.

Note the changes from the current file: the CLI-arg `sessionName`/`argc`/`argv`-based connection is entirely removed (the RoomMenu replaces it); the two pre-connection HUD text lines shifted down by 20px each (`y=10`→`y=10` for the new Room line, `y=10`→`y=30` for "You are slot N", `y=30`→`y=50` for "F1: Debug Menu") to make room for the new persistent Room line.

- [ ] **Step 2: Build**

Run: `mingw32-make client`
Expected: builds cleanly (this is the first point where `NetClient`'s Task 4 changes and `RoomMenu`'s Task 5 code are actually linked together into the real client binary).

- [ ] **Step 3: Run the smoke test path to confirm no regression**

Run: `bin/client --test`
Expected: all Juice + RoomMenu smoke tests pass, exit 0, no window opens.

- [ ] **Step 4: Manual verification**

Run `bin/server` (stop any already-running instance first — check `tasklist //FI "IMAGENAME eq server.exe"`, ask before killing), then run TWO `bin/client` instances:
- On the first client: click "Create Room", confirm the menu disappears and the game view appears with a HUD line showing a 6-digit room code (e.g. `Room: 482913 | Connected`).
- On the second client: type that exact code into the join box and click "Join Room" (or press Enter once 6 digits are typed), confirm it also connects and shows the SAME room code, and both players can see and move each other.
- On a third client (or restart one), try joining a made-up code (e.g. `000001` if that's not the one in use) and confirm the menu shows "Room not found" and stays on the menu instead of crashing or hanging.

- [ ] **Step 5: Commit**

```bash
git add src/client/main.cpp
git commit -m "Integrate RoomMenu into client startup flow with persistent room-code HUD"
```

---

## Final Verification

After all six tasks:

- [ ] Run `mingw32-make server && mingw32-make client` — both build cleanly with no errors.
- [ ] Run `bin/server --test` — all server smoke tests (including Task 2's new room-code tests) pass.
- [ ] Run `bin/client --test` — all client smoke tests (Juice + RoomMenu) pass.
- [ ] Live 2-client test per Task 6 Step 4: create + join with a valid code succeeds and shows matching room codes in both HUDs; joining an invalid/nonexistent code shows "Room not found" and does not crash.
- [ ] Confirm the F1 debug menu and all previously-built visual polish (shake, flash, particles, damage numbers, revive ring) still work unaffected — this plan only changes the pre-connection flow and adds one HUD line, no gameplay-loop logic changed.
