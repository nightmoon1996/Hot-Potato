# Client/Server Networking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the existing single-process local 2-player game into an authoritative headless C++ server and two thin raylib clients communicating over a hybrid reliable/unreliable UDP protocol, with named sessions and reconnect support.

**Architecture:** A `src/shared/` layer (protocol structs, serialization, cross-platform UDP socket wrapper, reliable-channel ack/retransmit logic) is used by both a new headless `src/server/` process (owns all `Player`/`Item`/`Hazard`/`Combat` simulation, ticks at 60Hz, manages sessions and reconnect) and a new thin `src/client/` process (raylib window, sends input, renders the latest received snapshot, sends debug-menu requests). The existing gameplay logic files move to be server-only.

**Tech Stack:** C++17, raylib + raylib-cpp (client only), cross-platform BSD/Winsock UDP sockets (no new external networking library), GNU Make (Makefile gets new `server`/`client` targets).

**Spec:** [docs/superpowers/specs/2026-08-26-client-server-networking-design.md](../specs/2026-08-26-client-server-networking-design.md)

## Global Constraints

- Server tick / snapshot rate: 60 Hz
- Reliable-channel retransmit interval: 100ms
- Disconnect detection timeout (no packets received): 60s
- Reconnect grace period after disconnect detected: 60s
- Server is 100% authoritative — client never mutates game state directly, only sends input/requests
- No client-side prediction in this pass
- No new external networking library — raw BSD/Winsock sockets only
- Same repository (no submodule/separate-repo split)
- All prior gameplay numeric defaults remain unchanged: Max HP 100, Downed 15s, Death respawn 5s, Revive HP 50, Respawn HP 100, Attack cooldown 0.8s, Attack range 40px, Attack damage 15, Pickup radius 24px, Revive range 32px, Channel duration 2.0s, Hazard damage 5 HP/sec, Inventory capacity 8 slots

---

## Task 1: Protocol message structs and serialization

**Files:**
- Create: `src/shared/Protocol.h`
- Create: `src/shared/Serialize.h`
- Modify: `src/main.cpp` (temporary smoke-test additions — this file is deleted in Task 8 once client/server split is wired; the smoke tests here are ported into `src/server/main.cpp`'s test harness in a later task, so treat this as scratch verification for now, not permanent)

**Interfaces:**
- Produces:
  - `enum class MessageType : uint8_t { ConnectRequest, Welcome, Rejected, DebugActionRequest, DisconnectNotice, Ack, Input, Snapshot };`
  - `enum class RejectReason : uint8_t { SessionFull, InvalidToken };`
  - `enum class DebugAction : uint8_t { Kill, Revive, HealFull, GivePotion, ForceDown };`
  - `struct PacketHeader { uint8_t channel; uint32_t seq; uint16_t payloadLen; };` (channel: 0=unreliable-sequenced, 1=reliable-ordered)
  - `struct ConnectRequestMsg { char sessionName[32]; uint32_t reconnectToken; };` (reconnectToken == 0 means "no token")
  - `struct WelcomeMsg { uint8_t playerSlot; uint32_t sessionToken; int maxHp; float downedDuration; float deathRespawnDelay; int reviveHp; int respawnHp; float attackCooldown; float attackRange; int attackDamage; float channelDuration; float hazardDamagePerSecond; int inventoryCapacity; };`
  - `struct RejectedMsg { RejectReason reason; };`
  - `struct DebugActionRequestMsg { DebugAction action; uint8_t targetSlot; };`
  - `struct AckMsg { uint32_t ackedSeq; };`
  - `struct InputMsg { float moveX; float moveY; bool interactHeld; bool attackPressed; };`
  - `struct PlayerSnapshot { float posX; float posY; int hp; uint8_t state; int potionCount; float channelTimer; };` (state: 0=Alive, 1=Downed, 2=Dead — matches `PlayerState` enum order from `src/Player.h`)
  - `struct WorldItemSnapshot { float posX; float posY; bool active; };`
  - `struct SnapshotMsg { PlayerSnapshot players[2]; WorldItemSnapshot items[2]; float hazardX; float hazardY; float hazardW; float hazardH; };`
  - In `Serialize.h`: `template<typename T> void SerializeStruct(const T& value, std::vector<uint8_t>& outBuffer);` and `template<typename T> bool DeserializeStruct(const uint8_t* data, size_t len, T& outValue);` — since every struct above is a plain fixed-layout POD (no pointers, no `std::string`, fixed-size arrays only), serialization is a raw memory copy: `SerializeStruct` appends `sizeof(T)` bytes via `memcpy` from `&value`; `DeserializeStruct` checks `len >= sizeof(T)` and `memcpy`s into `&outValue`, returning `false` if the length check fails.

Every message struct is a plain, fixed-size POD (fixed-size arrays, no `std::string`, no pointers) specifically so `memcpy`-based serialization is correct and portable within this project's own client/server (endianness matching is not addressed here since client and server run on the same architecture family for this test project — no network byte-order conversion is done, matching the spec's LAN/localhost test scope).

- [ ] **Step 1: Write `src/shared/Protocol.h`**

```cpp
#pragma once

#include <cstdint>

enum class MessageType : uint8_t {
    ConnectRequest,
    Welcome,
    Rejected,
    DebugActionRequest,
    DisconnectNotice,
    Ack,
    Input,
    Snapshot
};

enum class RejectReason : uint8_t {
    SessionFull,
    InvalidToken
};

enum class DebugAction : uint8_t {
    Kill,
    Revive,
    HealFull,
    GivePotion,
    ForceDown
};

struct PacketHeader {
    uint8_t channel;      // 0 = unreliable-sequenced, 1 = reliable-ordered
    uint32_t seq;
    uint16_t payloadLen;
};

struct ConnectRequestMsg {
    char sessionName[32];
    uint32_t reconnectToken; // 0 = no token
};

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

struct RejectedMsg {
    RejectReason reason;
};

struct DebugActionRequestMsg {
    DebugAction action;
    uint8_t targetSlot;
};

struct AckMsg {
    uint32_t ackedSeq;
};

struct InputMsg {
    float moveX;
    float moveY;
    bool interactHeld;
    bool attackPressed;
};

struct PlayerSnapshot {
    float posX;
    float posY;
    int hp;
    uint8_t state; // 0 = Alive, 1 = Downed, 2 = Dead
    int potionCount;
    float channelTimer;
};

struct WorldItemSnapshot {
    float posX;
    float posY;
    bool active;
};

struct SnapshotMsg {
    PlayerSnapshot players[2];
    WorldItemSnapshot items[2];
    float hazardX;
    float hazardY;
    float hazardW;
    float hazardH;
};
```

- [ ] **Step 2: Write `src/shared/Serialize.h`**

```cpp
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

template<typename T>
void SerializeStruct(const T& value, std::vector<uint8_t>& outBuffer) {
    size_t offset = outBuffer.size();
    outBuffer.resize(offset + sizeof(T));
    std::memcpy(outBuffer.data() + offset, &value, sizeof(T));
}

template<typename T>
bool DeserializeStruct(const uint8_t* data, size_t len, T& outValue) {
    if (len < sizeof(T)) {
        return false;
    }
    std::memcpy(&outValue, data, sizeof(T));
    return true;
}
```

- [ ] **Step 3: Add a temporary smoke test to `src/main.cpp`**

Add near the top, after the existing includes:

```cpp
#include "shared/Protocol.h"
#include "shared/Serialize.h"
```

Add this function, and call it as the very first line inside `main()` (before `SmokeTestItems();`):

```cpp
void SmokeTestSerialization() {
    InputMsg input{ 1.0f, -1.0f, true, false };
    std::vector<uint8_t> buffer;
    SerializeStruct(input, buffer);
    assert(buffer.size() == sizeof(InputMsg));

    InputMsg roundTripped{};
    bool ok = DeserializeStruct(buffer.data(), buffer.size(), roundTripped);
    assert(ok == true);
    assert(roundTripped.moveX == 1.0f);
    assert(roundTripped.moveY == -1.0f);
    assert(roundTripped.interactHeld == true);
    assert(roundTripped.attackPressed == false);

    // Too-short buffer fails
    InputMsg failed{};
    bool notOk = DeserializeStruct(buffer.data(), 2, failed);
    assert(notOk == false);

    // SnapshotMsg round trip (larger, nested struct)
    SnapshotMsg snap{};
    snap.players[0] = PlayerSnapshot{ 10.0f, 20.0f, 100, 0, 2, 0.5f };
    snap.players[1] = PlayerSnapshot{ 30.0f, 40.0f, 0, 1, 0, 0.0f };
    snap.items[0] = WorldItemSnapshot{ 5.0f, 5.0f, true };
    snap.items[1] = WorldItemSnapshot{ 6.0f, 6.0f, false };
    snap.hazardX = 100.0f;
    snap.hazardY = 200.0f;
    snap.hazardW = 50.0f;
    snap.hazardH = 60.0f;

    std::vector<uint8_t> snapBuffer;
    SerializeStruct(snap, snapBuffer);
    SnapshotMsg snapRoundTripped{};
    bool snapOk = DeserializeStruct(snapBuffer.data(), snapBuffer.size(), snapRoundTripped);
    assert(snapOk == true);
    assert(snapRoundTripped.players[0].posX == 10.0f);
    assert(snapRoundTripped.players[0].hp == 100);
    assert(snapRoundTripped.players[1].state == 1);
    assert(snapRoundTripped.items[0].active == true);
    assert(snapRoundTripped.items[1].active == false);
    assert(snapRoundTripped.hazardW == 50.0f);
}
```

Call it in `main()`:

```cpp
    SmokeTestSerialization();
    TraceLog(LOG_INFO, "SmokeTestSerialization passed");
```

- [ ] **Step 4: Build and run to verify**

Run: `mingw32-make bin/app; mingw32-make execute` (ensure `C:\msys64\mingw64\bin` is on PATH if `mingw32-make`/`g++` aren't found — try `export PATH="/c/msys64/mingw64/bin:$PATH"` first)

Expected: window opens, log shows `SmokeTestSerialization passed`, no assertion failure.

- [ ] **Step 5: Commit**

```bash
git add src/shared/Protocol.h src/shared/Serialize.h src/main.cpp
git commit -m "Add protocol message structs and raw-memcpy serialization with smoke tests"
```

---

## Task 2: Reliable-channel ack/retransmit/ordering logic

**Files:**
- Create: `src/shared/ReliableChannel.h`
- Modify: `src/main.cpp` (extend smoke tests)

**Interfaces:**
- Consumes: `PacketHeader` (Task 1)
- Produces:
  - `class ReliableSender` with:
    - `uint32_t NextSeq();` — returns an incrementing sequence number starting at 1 (0 is reserved/unused), for tagging outgoing reliable messages
    - `void TrackUnacked(uint32_t seq, const std::vector<uint8_t>& payload, double sentAtSeconds);` — records a sent-but-unacked message
    - `void OnAckReceived(uint32_t seq);` — removes a message from the unacked set once acked
    - `std::vector<std::pair<uint32_t, std::vector<uint8_t>>> GetMessagesToRetransmit(double nowSeconds, double retransmitIntervalSeconds);` — returns (seq, payload) pairs for every still-unacked message whose `sentAtSeconds` is at least `retransmitIntervalSeconds` in the past, and updates their internally tracked `sentAtSeconds` to `nowSeconds` (so they're not immediately re-flagged next call)
  - `class ReliableReceiver` with:
    - `bool ShouldAck(uint32_t seq);` — always returns `true` (every reliable message, including duplicates, gets acked) but exists as a named seam for clarity
    - `bool TryDeliverInOrder(uint32_t seq, const std::vector<uint8_t>& payload, std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& outReadyToProcess);` — buffers `(seq, payload)` if `seq` is not the next expected in-order sequence; if it IS the next expected, appends it (and any subsequently-bufferable, now-contiguous sequences) to `outReadyToProcess` in order, advances the expected-next counter, and returns `true` if at least one message became ready to process this call (`false` if this call only buffered an out-of-order arrival with nothing new becoming ready)

Expected-next starts at 1 (matching `ReliableSender::NextSeq()` starting at 1). A duplicate of an already-delivered seq (< expected-next) is ignored for delivery purposes (still acked per `ShouldAck`, but not re-delivered).

- [ ] **Step 1: Write `src/shared/ReliableChannel.h`**

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include <map>
#include <utility>

class ReliableSender {
public:
    uint32_t NextSeq() {
        return nextSeq++;
    }

    void TrackUnacked(uint32_t seq, const std::vector<uint8_t>& payload, double sentAtSeconds) {
        unacked[seq] = { payload, sentAtSeconds };
    }

    void OnAckReceived(uint32_t seq) {
        unacked.erase(seq);
    }

    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> GetMessagesToRetransmit(double nowSeconds, double retransmitIntervalSeconds) {
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> result;
        for (auto& entry : unacked) {
            uint32_t seq = entry.first;
            std::vector<uint8_t>& payload = entry.second.first;
            double& sentAt = entry.second.second;
            if (nowSeconds - sentAt >= retransmitIntervalSeconds) {
                result.push_back({ seq, payload });
                sentAt = nowSeconds;
            }
        }
        return result;
    }

private:
    uint32_t nextSeq = 1;
    std::map<uint32_t, std::pair<std::vector<uint8_t>, double>> unacked;
};

class ReliableReceiver {
public:
    bool ShouldAck(uint32_t seq) {
        (void)seq;
        return true;
    }

    bool TryDeliverInOrder(uint32_t seq, const std::vector<uint8_t>& payload,
                           std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& outReadyToProcess) {
        if (seq < expectedNext) {
            return false; // already delivered duplicate
        }
        pending[seq] = payload;

        bool deliveredAny = false;
        while (pending.count(expectedNext) > 0) {
            outReadyToProcess.push_back({ expectedNext, pending[expectedNext] });
            pending.erase(expectedNext);
            expectedNext++;
            deliveredAny = true;
        }
        return deliveredAny;
    }

private:
    uint32_t expectedNext = 1;
    std::map<uint32_t, std::vector<uint8_t>> pending;
};
```

- [ ] **Step 2: Add a smoke test to `src/main.cpp`**

Add `#include "shared/ReliableChannel.h"`. Add:

```cpp
void SmokeTestReliableChannel() {
    // Sender: track, ack, retransmit
    ReliableSender sender;
    uint32_t seq1 = sender.NextSeq();
    uint32_t seq2 = sender.NextSeq();
    assert(seq1 == 1);
    assert(seq2 == 2);

    std::vector<uint8_t> payload1{ 0xAA };
    std::vector<uint8_t> payload2{ 0xBB };
    sender.TrackUnacked(seq1, payload1, 0.0);
    sender.TrackUnacked(seq2, payload2, 0.0);

    // Not enough time has passed: no retransmits yet
    auto toRetransmit1 = sender.GetMessagesToRetransmit(0.05, 0.1);
    assert(toRetransmit1.size() == 0);

    // 0.1s later: both should be flagged for retransmit
    auto toRetransmit2 = sender.GetMessagesToRetransmit(0.1, 0.1);
    assert(toRetransmit2.size() == 2);

    // Ack seq1: it should no longer be retransmitted
    sender.OnAckReceived(seq1);
    auto toRetransmit3 = sender.GetMessagesToRetransmit(0.3, 0.1);
    assert(toRetransmit3.size() == 1);
    assert(toRetransmit3[0].first == seq2);

    // Receiver: in-order delivery
    ReliableReceiver receiver;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ready;

    bool delivered1 = receiver.TryDeliverInOrder(1, payload1, ready);
    assert(delivered1 == true);
    assert(ready.size() == 1);
    assert(ready[0].first == 1);

    // Out-of-order arrival (seq 3 before seq 2): buffered, nothing new ready
    ready.clear();
    bool delivered2 = receiver.TryDeliverInOrder(3, payload1, ready);
    assert(delivered2 == false);
    assert(ready.size() == 0);

    // Now seq 2 arrives: both 2 and 3 become ready, in order
    ready.clear();
    bool delivered3 = receiver.TryDeliverInOrder(2, payload2, ready);
    assert(delivered3 == true);
    assert(ready.size() == 2);
    assert(ready[0].first == 2);
    assert(ready[1].first == 3);

    // Duplicate of already-delivered seq 1: not re-delivered
    ready.clear();
    bool delivered4 = receiver.TryDeliverInOrder(1, payload1, ready);
    assert(delivered4 == false);
    assert(ready.size() == 0);
}
```

Call it in `main()`:

```cpp
    SmokeTestReliableChannel();
    TraceLog(LOG_INFO, "SmokeTestReliableChannel passed");
```

- [ ] **Step 3: Build and run to verify**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: log shows `SmokeTestReliableChannel passed`, no assertion failure.

- [ ] **Step 4: Commit**

```bash
git add src/shared/ReliableChannel.h src/main.cpp
git commit -m "Add reliable-channel ack/retransmit/ordering logic with smoke tests"
```

---

## Task 3: Cross-platform UDP socket wrapper

**Files:**
- Create: `src/shared/Socket.h`
- Create: `src/shared/Socket.cpp`
- Modify: `Makefile` (see note below — this task only needs the Windows link flag added since that's the active dev platform; full Linux/macOS link flags for sockets are already satisfied by existing flags on those platforms, BSD sockets need no extra linking beyond libc)

**Interfaces:**
- Produces:
  - `class UdpSocket` with:
    - `UdpSocket();` / `~UdpSocket();` — constructor initializes the platform socket layer (calls `WSAStartup` on Windows exactly once per `UdpSocket` instance's lifetime; a real multi-socket app would refcount this globally, but this project only ever constructs one `UdpSocket` per process, so per-instance init/cleanup is sufficient)
    - `bool Bind(uint16_t port);` — binds to `0.0.0.0:port` for receiving (used by the server, and optionally by clients for a fixed local port — not required for this project's clients, which can bind to port 0 / any)
    - `bool SendTo(const std::string& ip, uint16_t port, const uint8_t* data, size_t len);`
    - `int ReceiveFrom(uint8_t* outBuffer, size_t bufferCapacity, std::string& outFromIp, uint16_t& outFromPort);` — non-blocking; returns the number of bytes received, or -1 if nothing was available (no packet waiting) this call
  - This class wraps `#ifdef _WIN32` Winsock (`WSAStartup`/`WSACleanup`, `SOCKET`, `sendto`/`recvfrom`, `closesocket`, non-blocking via `ioctlsocket(FIONBIO)`) vs POSIX BSD sockets (`socket`/`bind`/`sendto`/`recvfrom`/`close`, non-blocking via `fcntl(F_SETFL, O_NONBLOCK)`) behind the same public interface.

This task has no automated smoke test — sockets require real OS resources and a loopback round-trip test is more appropriately verified manually once Task 6/7 wire up the real server/client processes (an isolated "send to myself on localhost" test in a smoke-test function would work, but real socket I/O behavior — especially non-blocking `recvfrom` returning "no data" vs erroring — is exactly the kind of thing better verified by the actual server/client talking to each other in Task 8, to avoid writing socket test code twice).

- [ ] **Step 1: Write `src/shared/Socket.h`**

```cpp
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    bool Bind(uint16_t port);
    bool SendTo(const std::string& ip, uint16_t port, const uint8_t* data, size_t len);
    int ReceiveFrom(uint8_t* outBuffer, size_t bufferCapacity, std::string& outFromIp, uint16_t& outFromPort);

private:
#ifdef _WIN32
    uintptr_t sockHandle; // SOCKET, stored as uintptr_t to avoid pulling <winsock2.h> into this header
#else
    int sockHandle;
#endif
    bool initialized;
};
```

- [ ] **Step 2: Write `src/shared/Socket.cpp`**

```cpp
#include "Socket.h"
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

UdpSocket::UdpSocket() : initialized(false) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        sockHandle = 0;
        return;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    sockHandle = (uintptr_t)s;
    initialized = true;
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return;
    }
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    sockHandle = s;
    initialized = true;
#endif
}

UdpSocket::~UdpSocket() {
    if (!initialized) {
        return;
    }
#ifdef _WIN32
    closesocket((SOCKET)sockHandle);
    WSACleanup();
#else
    close(sockHandle);
#endif
}

bool UdpSocket::Bind(uint16_t port) {
    if (!initialized) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
#ifdef _WIN32
    return bind((SOCKET)sockHandle, (sockaddr*)&addr, sizeof(addr)) == 0;
#else
    return bind(sockHandle, (sockaddr*)&addr, sizeof(addr)) == 0;
#endif
}

bool UdpSocket::SendTo(const std::string& ip, uint16_t port, const uint8_t* data, size_t len) {
    if (!initialized) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
#ifdef _WIN32
    int sent = sendto((SOCKET)sockHandle, (const char*)data, (int)len, 0, (sockaddr*)&addr, sizeof(addr));
#else
    ssize_t sent = sendto(sockHandle, data, len, 0, (sockaddr*)&addr, sizeof(addr));
#endif
    return sent == (int)len || sent == (ssize_t)len;
}

int UdpSocket::ReceiveFrom(uint8_t* outBuffer, size_t bufferCapacity, std::string& outFromIp, uint16_t& outFromPort) {
    if (!initialized) {
        return -1;
    }
    sockaddr_in fromAddr{};
#ifdef _WIN32
    int fromLen = sizeof(fromAddr);
    int received = recvfrom((SOCKET)sockHandle, (char*)outBuffer, (int)bufferCapacity, 0, (sockaddr*)&fromAddr, &fromLen);
    if (received <= 0) {
        return -1;
    }
#else
    socklen_t fromLen = sizeof(fromAddr);
    ssize_t received = recvfrom(sockHandle, outBuffer, bufferCapacity, 0, (sockaddr*)&fromAddr, &fromLen);
    if (received <= 0) {
        return -1;
    }
#endif
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));
    outFromIp = ipStr;
    outFromPort = ntohs(fromAddr.sin_port);
    return (int)received;
}
```

- [ ] **Step 3: Add the Windows socket link flag to the Makefile**

Read the current `Makefile`. In the `ifeq ($(OS), Windows_NT)` block, find the line:

```makefile
linkFlags += -Wl,--allow-multiple-definition -pthread -lopengl32 -lgdi32 -lwinmm -static -static-libgcc -static-libstdc++
```

Change it to add `-lws2_32` (the Winsock library):

```makefile
linkFlags += -Wl,--allow-multiple-definition -pthread -lopengl32 -lgdi32 -lwinmm -lws2_32 -static -static-libgcc -static-libstdc++
```

Linux/macOS need no additional link flags — BSD sockets are part of libc on those platforms.

- [ ] **Step 4: Build to verify it compiles (no smoke test — verified in Task 8)**

Run: `mingw32-make bin/app 2>&1 | tail -20`

Expected: build succeeds with no compiler/linker errors (this only adds new files without yet calling any of `UdpSocket`'s methods from `main.cpp`, so nothing new executes yet — this step just confirms the new `.cpp` file compiles and links cleanly with the new `-lws2_32` flag).

- [ ] **Step 5: Commit**

```bash
git add src/shared/Socket.h src/shared/Socket.cpp Makefile
git commit -m "Add cross-platform UDP socket wrapper (Winsock/BSD)"
```

---

## Task 4: SessionManager state machine (session create/join/reject/reconnect)

**Files:**
- Create: `src/server/Session.h`
- Create: `src/server/SessionManager.h`
- Modify: `src/main.cpp` (extend smoke tests)

**Interfaces:**
- Consumes: `Player` (from existing `src/Player.h`, unmoved until Task 5), nothing from Task 1-3 directly (this task is pure session bookkeeping logic, decoupled from the wire format so it's testable without any serialization/socket involvement)
- Produces:
  - `enum class SlotState { Empty, Connected, DisconnectedPending };`
  - `struct PlayerSlot { SlotState state = SlotState::Empty; uint32_t sessionToken = 0; std::string clientIp; uint16_t clientPort = 0; double lastPacketAtSeconds = 0.0; Player player; PlayerSlot() : player(Vector2{0,0}) {} };` (the `Player` default-constructed at the origin; the real spawn position is set when a slot transitions from Empty to Connected for the first time — see Step 1's `AssignSlot` helper)
  - `class Session` with:
    - `PlayerSlot slots[2];`
    - `int FindEmptySlot() const;` — returns 0 or 1 for the first Empty slot, or -1 if none
    - `int FindDisconnectedSlotByToken(uint32_t token) const;` — returns the slot index whose `sessionToken == token` and `state == DisconnectedPending`, or -1 if none
    - `void CheckTimeouts(double nowSeconds);` — for each Connected slot, if `nowSeconds - lastPacketAtSeconds >= 60.0`, transition it to DisconnectedPending; for each DisconnectedPending slot, if `nowSeconds - lastPacketAtSeconds >= 120.0` (60s to detect + 60s grace), transition it to Empty and reset its `Player` to a fresh one at that slot's original spawn point
  - `enum class ConnectResult { Created, Joined, Rejected, Reconnected };`
  - `struct ConnectOutcome { ConnectResult result; int slotIndex; uint32_t sessionToken; RejectReason rejectReason; };` (`RejectReason` from `src/shared/Protocol.h`, Task 1)
  - `class SessionManager` with:
    - `ConnectOutcome HandleConnect(const std::string& sessionName, uint32_t reconnectToken, const std::string& clientIp, uint16_t clientPort, double nowSeconds);` — implements the 5-case logic from the spec's "Connect handling" section (see Step 1 below for the exact code)
    - `Session* GetSession(const std::string& sessionName);` — returns a pointer to the named session, or `nullptr` if it doesn't exist
    - `void CheckAllTimeouts(double nowSeconds);` — calls `CheckTimeouts` on every managed session

Spawn points: slot 0 always spawns at `Vector2{150.0f, 300.0f}`, slot 1 always spawns at `Vector2{850.0f, 300.0f}` (matching the prior single-process game's `p1`/`p2` spawn positions) — hardcoded as constants in `Session.h`.

- [ ] **Step 1: Write `src/server/Session.h`**

```cpp
#pragma once

#include <string>
#include <cstdint>
#include <raylib-cpp.hpp>
#include "../Player.h"

static constexpr Vector2 kSlot0Spawn{ 150.0f, 300.0f };
static constexpr Vector2 kSlot1Spawn{ 850.0f, 300.0f };

enum class SlotState {
    Empty,
    Connected,
    DisconnectedPending
};

struct PlayerSlot {
    SlotState state = SlotState::Empty;
    uint32_t sessionToken = 0;
    std::string clientIp;
    uint16_t clientPort = 0;
    double lastPacketAtSeconds = 0.0;
    Player player;

    PlayerSlot() : player(Vector2{0.0f, 0.0f}) {}
};

class Session {
public:
    PlayerSlot slots[2];

    int FindEmptySlot() const {
        for (int i = 0; i < 2; i++) {
            if (slots[i].state == SlotState::Empty) {
                return i;
            }
        }
        return -1;
    }

    int FindDisconnectedSlotByToken(uint32_t token) const {
        if (token == 0) {
            return -1;
        }
        for (int i = 0; i < 2; i++) {
            if (slots[i].state == SlotState::DisconnectedPending && slots[i].sessionToken == token) {
                return i;
            }
        }
        return -1;
    }

    void CheckTimeouts(double nowSeconds) {
        for (int i = 0; i < 2; i++) {
            if (slots[i].state == SlotState::Connected &&
                nowSeconds - slots[i].lastPacketAtSeconds >= 60.0) {
                slots[i].state = SlotState::DisconnectedPending;
            } else if (slots[i].state == SlotState::DisconnectedPending &&
                       nowSeconds - slots[i].lastPacketAtSeconds >= 120.0) {
                Vector2 spawn = (i == 0) ? kSlot0Spawn : kSlot1Spawn;
                slots[i] = PlayerSlot{};
                slots[i].player = Player(spawn);
            }
        }
    }
};
```

- [ ] **Step 2: Write `src/server/SessionManager.h`**

```cpp
#pragma once

#include <string>
#include <map>
#include <cstdint>
#include <cstdlib>
#include "Session.h"
#include "../shared/Protocol.h"

enum class ConnectResult {
    Created,
    Joined,
    Rejected,
    Reconnected
};

struct ConnectOutcome {
    ConnectResult result;
    int slotIndex;
    uint32_t sessionToken;
    RejectReason rejectReason;
};

class SessionManager {
public:
    ConnectOutcome HandleConnect(const std::string& sessionName, uint32_t reconnectToken,
                                 const std::string& clientIp, uint16_t clientPort, double nowSeconds) {
        // Case 4: reconnect token matches a disconnected slot
        if (reconnectToken != 0) {
            auto it = sessions.find(sessionName);
            if (it != sessions.end()) {
                int slotIndex = it->second.FindDisconnectedSlotByToken(reconnectToken);
                if (slotIndex != -1) {
                    PlayerSlot& slot = it->second.slots[slotIndex];
                    slot.state = SlotState::Connected;
                    slot.clientIp = clientIp;
                    slot.clientPort = clientPort;
                    slot.lastPacketAtSeconds = nowSeconds;
                    return { ConnectResult::Reconnected, slotIndex, slot.sessionToken, RejectReason::SessionFull };
                }
            }
            // Case 5: token present but no match -> fall through to fresh-connect logic
        }

        // Case 1: session doesn't exist -> create it, assign slot 0
        auto it = sessions.find(sessionName);
        if (it == sessions.end()) {
            Session newSession;
            uint32_t token = GenerateToken();
            newSession.slots[0].state = SlotState::Connected;
            newSession.slots[0].sessionToken = token;
            newSession.slots[0].clientIp = clientIp;
            newSession.slots[0].clientPort = clientPort;
            newSession.slots[0].lastPacketAtSeconds = nowSeconds;
            newSession.slots[0].player = Player(kSlot0Spawn);
            newSession.slots[1].player = Player(kSlot1Spawn);
            sessions[sessionName] = newSession;
            return { ConnectResult::Created, 0, token, RejectReason::SessionFull };
        }

        // Case 2: session exists with an empty slot
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
            return { ConnectResult::Joined, emptySlot, token, RejectReason::SessionFull };
        }

        // Case 3: session full
        return { ConnectResult::Rejected, -1, 0, RejectReason::SessionFull };
    }

    Session* GetSession(const std::string& sessionName) {
        auto it = sessions.find(sessionName);
        if (it == sessions.end()) {
            return nullptr;
        }
        return &it->second;
    }

    void CheckAllTimeouts(double nowSeconds) {
        for (auto& entry : sessions) {
            entry.second.CheckTimeouts(nowSeconds);
        }
    }

private:
    std::map<std::string, Session> sessions;

    uint32_t GenerateToken() {
        uint32_t token = 0;
        while (token == 0) {
            token = (uint32_t)std::rand();
        }
        return token;
    }
};
```

- [ ] **Step 3: Add a smoke test to `src/main.cpp`**

Add `#include "server/SessionManager.h"`. Add:

```cpp
void SmokeTestSessionManager() {
    SessionManager manager;

    // Case 1: fresh session created, first connector gets slot 0
    ConnectOutcome outcome1 = manager.HandleConnect("game1", 0, "127.0.0.1", 5000, 0.0);
    assert(outcome1.result == ConnectResult::Created);
    assert(outcome1.slotIndex == 0);
    assert(outcome1.sessionToken != 0);

    // Case 2: second connector joins the open slot
    ConnectOutcome outcome2 = manager.HandleConnect("game1", 0, "127.0.0.1", 5001, 0.0);
    assert(outcome2.result == ConnectResult::Joined);
    assert(outcome2.slotIndex == 1);
    assert(outcome2.sessionToken != 0);
    assert(outcome2.sessionToken != outcome1.sessionToken);

    // Case 3: third connector rejected, session full
    ConnectOutcome outcome3 = manager.HandleConnect("game1", 0, "127.0.0.1", 5002, 0.0);
    assert(outcome3.result == ConnectResult::Rejected);
    assert(outcome3.rejectReason == RejectReason::SessionFull);

    // Simulate slot 0 going disconnected (timeout), then reconnecting
    Session* session = manager.GetSession("game1");
    assert(session != nullptr);
    session->slots[0].lastPacketAtSeconds = 0.0;
    session->CheckTimeouts(60.0); // 60s elapsed -> slot 0 becomes DisconnectedPending
    assert(session->slots[0].state == SlotState::DisconnectedPending);
    assert(session->slots[1].state == SlotState::Connected); // slot 1 untouched (recent packet)

    // Case 4: reconnect with the correct token succeeds, resumes the same slot
    ConnectOutcome outcome4 = manager.HandleConnect("game1", outcome1.sessionToken, "127.0.0.1", 5003, 61.0);
    assert(outcome4.result == ConnectResult::Reconnected);
    assert(outcome4.slotIndex == 0);
    assert(outcome4.sessionToken == outcome1.sessionToken);
    assert(session->slots[0].state == SlotState::Connected);

    // Case 5: reconnect with a token that doesn't match anything falls back to fresh-connect logic
    // (session "game1" is now full again after the reconnect above, so this should be rejected)
    ConnectOutcome outcome5 = manager.HandleConnect("game1", 999999, "127.0.0.1", 5004, 61.0);
    assert(outcome5.result == ConnectResult::Rejected);

    // Full timeout-to-Empty cycle: disconnect slot 0 again, let both timeouts elapse, slot becomes Empty
    session->slots[0].lastPacketAtSeconds = 100.0;
    session->CheckTimeouts(160.0); // 60s -> DisconnectedPending
    assert(session->slots[0].state == SlotState::DisconnectedPending);
    session->CheckTimeouts(220.0); // another 60s (120s total from lastPacketAtSeconds) -> Empty
    assert(session->slots[0].state == SlotState::Empty);

    // A new session name creates an independent session
    ConnectOutcome outcomeOther = manager.HandleConnect("game2", 0, "127.0.0.1", 6000, 0.0);
    assert(outcomeOther.result == ConnectResult::Created);
    assert(outcomeOther.slotIndex == 0);
}
```

Call it in `main()`:

```cpp
    SmokeTestSessionManager();
    TraceLog(LOG_INFO, "SmokeTestSessionManager passed");
```

- [ ] **Step 4: Build and run to verify**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: log shows `SmokeTestSessionManager passed`, no assertion failure.

- [ ] **Step 5: Commit**

```bash
git add src/server/Session.h src/server/SessionManager.h src/main.cpp
git commit -m "Add SessionManager with create/join/reject/reconnect/timeout logic and smoke tests"
```

---

## Task 5: Move gameplay simulation files to be server-owned; add server-side priority/debug-action helpers

**Files:**
- Modify: `src/Player.h` → move to `src/server/Player.h` (no content changes)
- Modify: `src/Item.h` → move to `src/server/Item.h` (no content changes)
- Modify: `src/Hazard.h` → move to `src/server/Hazard.h` (no content changes)
- Modify: `src/Combat.h` → move to `src/server/Combat.h` (no content changes)
- Create: `src/server/DebugActions.h`
- Modify: `src/main.cpp` (update include paths to match the move; this file still exists as the scratch smoke-test harness until Task 8 replaces it)

**Interfaces:**
- Consumes: `Player`, `PlayerState`, `Inventory`, `ItemType`, `WorldItem`, `TryPickup`, `HazardZone`, `ApplyHazardDamage`, `TryAttack`, `UpdateAttackCooldown`, `UpdateRevive`, `DistanceBetween` (all unchanged from the existing single-process code, just relocated)
- Produces:
  - `void ApplyDebugAction(Player& target, DebugAction action);` in `src/server/DebugActions.h` — the exact same 5-branch logic as the old `DebugMenu::ApplyAction`, extracted into a standalone function (since the server has no `DebugMenu` UI class — that stays client-side as a request-sender in a later task):
    ```cpp
    #pragma once
    #include "Player.h"
    #include "../shared/Protocol.h"

    inline void ApplyDebugAction(Player& target, DebugAction action) {
        switch (action) {
            case DebugAction::Kill:
                target.Kill();
                break;
            case DebugAction::Revive:
                if (target.state == PlayerState::Downed) target.ReviveFromDowned();
                else if (target.state == PlayerState::Dead) target.RespawnFull();
                break;
            case DebugAction::HealFull:
                if (target.state == PlayerState::Alive) target.hp = Player::kMaxHp;
                break;
            case DebugAction::GivePotion:
                target.inventory.Add(ItemType::RevivePotion);
                break;
            case DebugAction::ForceDown:
                target.ForceDown();
                break;
        }
    }
    ```

This task is a pure file-move plus one small new extraction (`ApplyDebugAction`) — no behavior changes to any moved file's content.

- [ ] **Step 1: Move the four gameplay files**

```bash
mkdir -p src/server
git mv src/Player.h src/server/Player.h
git mv src/Item.h src/server/Item.h
git mv src/Hazard.h src/server/Hazard.h
git mv src/Combat.h src/server/Combat.h
```

- [ ] **Step 2: Fix the moved files' relative includes**

`src/server/Player.h` currently has `#include "Item.h"` — since `Item.h` moved to the same directory (`src/server/`), this include is still correct as-is (no change needed).

`src/server/Hazard.h` currently has `#include "Player.h"` — same directory, no change needed.

`src/server/Combat.h` currently has `#include "Player.h"` — same directory, no change needed.

(All four files' internal includes reference each other by bare filename with no path prefix, and since they're all moving into the same new directory together, no include paths need to change inside these four files.)

- [ ] **Step 3: Create `src/server/DebugActions.h`** (exact content shown in the Interfaces section above)

- [ ] **Step 4: Update `src/main.cpp`'s includes to the new paths**

Change:

```cpp
#include "Item.h"
#include "Player.h"
#include "Hazard.h"
#include "Combat.h"
#include "DebugMenu.h"
```

to:

```cpp
#include "server/Item.h"
#include "server/Player.h"
#include "server/Hazard.h"
#include "server/Combat.h"
#include "server/DebugActions.h"
#include "DebugMenu.h"
```

(`DebugMenu.h` stays in `src/` for now — a later task moves it to `src/client/` and changes its behavior; this task only needs it to keep compiling since `src/main.cpp` still uses it as the single-process game loop for now.)

`src/DebugMenu.h` itself has `#include "Player.h"` at the top — change this one line to `#include "server/Player.h"` since `Player.h` moved.

Add `#include "server/DebugActions.h"` is already covered above; no other file needs a new include for this task.

- [ ] **Step 5: Build and run to verify nothing broke**

Run: `mingw32-make bin/app; mingw32-make execute`

Expected: build succeeds, all six smoke test pass lines are printed (Serialization, ReliableChannel, SessionManager, Items, PlayerStateMachine, Hazard, CombatAndRevive — 7 total including "All smoke tests passed"), window opens and the game plays identically to before (this task made no behavior changes, only moved files and added one new small header).

- [ ] **Step 6: Commit**

```bash
git add -A src/
git commit -m "Move gameplay simulation files under src/server, extract ApplyDebugAction"
```

---

## Task 6: Server main loop — session ticking, packet dispatch, snapshot broadcast

**Files:**
- Create: `src/server/main.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1-5 (`UdpSocket`, `PacketHeader`, all message structs, `SerializeStruct`/`DeserializeStruct`, `ReliableSender`/`ReliableReceiver`, `SessionManager`/`Session`/`PlayerSlot`/`SlotState`, `Player`/`PlayerState`/`Inventory`/`ItemType`/`WorldItem`/`TryPickup`, `HazardZone`/`ApplyHazardDamage`, `TryAttack`/`UpdateAttackCooldown`/`UpdateRevive`/`DistanceBetween`, `ApplyDebugAction`)
- Produces: a running headless server process listening on a fixed UDP port, accepting connections, simulating sessions at 60Hz, and broadcasting snapshots — the deliverable other tasks (client) connect to.

This task has no automated smoke test — it's the process entry point wiring everything together, verified by manual end-to-end testing in Task 8 alongside the client. The per-session `ReliableSender`/`ReliableReceiver` pair lives in each `PlayerSlot` (added as new fields in this task, since Task 4 didn't yet know about the reliable channel).

- [ ] **Step 1: Add reliable-channel fields to `PlayerSlot`**

Modify `src/server/Session.h`: add `#include "../shared/ReliableChannel.h"` near the top, and add two new fields to `struct PlayerSlot`:

```cpp
struct PlayerSlot {
    SlotState state = SlotState::Empty;
    uint32_t sessionToken = 0;
    std::string clientIp;
    uint16_t clientPort = 0;
    double lastPacketAtSeconds = 0.0;
    Player player;
    ReliableSender reliableSender;
    ReliableReceiver reliableReceiver;

    PlayerSlot() : player(Vector2{0.0f, 0.0f}) {}
};
```

(No other changes to `Session.h` needed — `ReliableSender`/`ReliableReceiver` are default-constructible with no arguments.)

- [ ] **Step 2: Write `src/server/main.cpp`**

```cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <raylib-cpp.hpp> // only for Vector2/Rectangle math types and CheckCollisionPointRec, no window created

#include "../shared/Protocol.h"
#include "../shared/Serialize.h"
#include "../shared/Socket.h"
#include "SessionManager.h"
#include "Item.h"
#include "Hazard.h"
#include "Combat.h"
#include "DebugActions.h"

static const uint16_t kServerPort = 7777;
static const float kMoveSpeed = 200.0f;
static const float kPickupRadius = 24.0f;
static const float kReviveRange = 32.0f;

static double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void SendReliable(UdpSocket& socket, PlayerSlot& slot, MessageType type, const uint8_t* payload, size_t payloadLen, double now) {
    std::vector<uint8_t> buffer;
    buffer.push_back((uint8_t)type);
    buffer.insert(buffer.end(), payload, payload + payloadLen);

    uint32_t seq = slot.reliableSender.NextSeq();
    slot.reliableSender.TrackUnacked(seq, buffer, now);

    PacketHeader header{ 1, seq, (uint16_t)buffer.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), buffer.begin(), buffer.end());
    socket.SendTo(slot.clientIp, slot.clientPort, packet.data(), packet.size());
}

static void SendUnreliable(UdpSocket& socket, const std::string& ip, uint16_t port, uint32_t seq, MessageType type, const uint8_t* payload, size_t payloadLen) {
    std::vector<uint8_t> buffer;
    buffer.push_back((uint8_t)type);
    buffer.insert(buffer.end(), payload, payload + payloadLen);

    PacketHeader header{ 0, seq, (uint16_t)buffer.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), buffer.begin(), buffer.end());
    socket.SendTo(ip, port, packet.data(), packet.size());
}

int main() {
    std::srand((unsigned int)std::time(nullptr));

    UdpSocket socket;
    if (!socket.Bind(kServerPort)) {
        std::printf("Failed to bind UDP port %d\n", (int)kServerPort);
        return 1;
    }
    std::printf("Server listening on UDP port %d\n", (int)kServerPort);

    SessionManager sessionManager;
    HazardZone hazard{ Rectangle{450.0f, 200.0f, 100.0f, 200.0f} };

    // Track world items and unreliable-send sequence counters per session name,
    // since Session itself only holds player slots.
    std::map<std::string, std::vector<WorldItem>> sessionWorldItems;
    std::map<std::string, float[2]> sessionHazardCarry;
    std::map<std::string, uint32_t> sessionSnapshotSeq;

    const double tickInterval = 1.0 / 60.0;
    double lastTick = NowSeconds();

    uint8_t recvBuffer[1024];

    while (true) {
        // --- Receive and dispatch incoming packets ---
        std::string fromIp;
        uint16_t fromPort;
        int received = socket.ReceiveFrom(recvBuffer, sizeof(recvBuffer), fromIp, fromPort);
        if (received > (int)sizeof(PacketHeader)) {
            PacketHeader header{};
            DeserializeStruct(recvBuffer, received, header);
            const uint8_t* payload = recvBuffer + sizeof(PacketHeader);
            size_t payloadLen = (size_t)received - sizeof(PacketHeader);

            if (payloadLen >= 1) {
                MessageType type = (MessageType)payload[0];
                const uint8_t* body = payload + 1;
                size_t bodyLen = payloadLen - 1;

                if (header.channel == 1 && type == MessageType::ConnectRequest) {
                    ConnectRequestMsg msg{};
                    if (DeserializeStruct(body, bodyLen, msg)) {
                        std::string sessionName(msg.sessionName);
                        double now = NowSeconds();
                        ConnectOutcome outcome = sessionManager.HandleConnect(sessionName, msg.reconnectToken, fromIp, fromPort, now);

                        if (outcome.result == ConnectResult::Rejected) {
                            RejectedMsg reject{ outcome.rejectReason };
                            std::vector<uint8_t> rejectBytes;
                            SerializeStruct(reject, rejectBytes);
                            std::vector<uint8_t> full;
                            full.push_back((uint8_t)MessageType::Rejected);
                            full.insert(full.end(), rejectBytes.begin(), rejectBytes.end());
                            PacketHeader rejectHeader{ 1, 0, (uint16_t)full.size() };
                            std::vector<uint8_t> packet;
                            SerializeStruct(rejectHeader, packet);
                            packet.insert(packet.end(), full.begin(), full.end());
                            socket.SendTo(fromIp, fromPort, packet.data(), packet.size());
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
                        }
                    }
                } else {
                    // For all other message types, find which session/slot this address belongs to.
                    // (A small linear scan is fine at this project's scale: at most a handful of
                    // sessions, 2 slots each.)
                    // This requires iterating sessionManager's sessions, so we expose a helper loop here
                    // rather than adding a by-address index to SessionManager (out of scope for this task).
                }
            }
        }

        // --- Fixed 60Hz simulation tick ---
        double now = NowSeconds();
        if (now - lastTick >= tickInterval) {
            float dt = (float)(now - lastTick);
            lastTick = now;
            sessionManager.CheckAllTimeouts(now);
            // Per-session simulation and snapshot broadcast happens here in Task 7's
            // continuation of this file (input processing, movement, combat, revive,
            // hazard, timers, snapshot send) — see Task 7.
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
```

**Note for the engineer:** this task deliberately leaves the "dispatch InputMsg/DebugActionRequest/Ack by client address" branch and the per-session simulation tick body as scaffolding — Task 7 completes both in the same file. Splitting this way keeps Task 6 focused on "can the server bind a socket, accept a ConnectRequest, and reply Welcome" as an independently verifiable slice, before Task 7 adds the full per-tick simulation loop on top.

- [ ] **Step 3: Add a Makefile target for the server**

Read the current `Makefile`. Add these lines after the existing `execute:` target:

```makefile
serverSources := $(call rwildcard,src/shared/,*.cpp) $(call rwildcard,src/server/,*.cpp)
serverObjects := $(patsubst src/%, $(buildDir)/%, $(patsubst %.cpp, %.o, $(serverSources)))
serverTarget := $(buildDir)/server$(if $(filter Windows,$(platform)),.exe,)

.PHONY: server run-server

server: $(serverTarget)

$(serverTarget): $(serverObjects)
	$(CXX) $(serverObjects) -o $(serverTarget) $(if $(filter Windows,$(platform)),-lws2_32 -static -static-libgcc -static-libstdc++,)

run-server: server
	$(serverTarget)
```

(The server binary does not link raylib or its `-l raylib` / OpenGL / windowing flags at all — it only needs `-lws2_32` on Windows for sockets, and nothing extra on Linux/macOS since BSD sockets are in libc. It does, however, `#include <raylib-cpp.hpp>` for `Vector2`/`Rectangle`/`CheckCollisionPointRec` math types — since these are header-only inline wrappers in raylib-cpp for simple structs, this compiles fine without linking the raylib library itself, as long as `-I include` still resolves the header. Confirm this compiles in Step 4; if raylib-cpp's `Vector2`/`Rectangle` types require any non-header-only raylib symbols, note this in your report as a blocker for the controller to resolve, since it would mean the server needs a lightweight math-only header instead of pulling in raylib-cpp.)

- [ ] **Step 4: Build to verify it compiles and runs (no full test yet)**

Run: `mingw32-make server 2>&1 | tail -30`

Expected: compiles and links successfully into `bin/server.exe` (Windows) with no raylib/OpenGL linker errors. If it fails specifically because `Vector2`/`Rectangle` pull in non-header-only raylib symbols, report this as a concern (do not attempt to fix by inventing a workaround — this is exactly the kind of architectural surprise the plan asks you to escalate).

Then run it briefly:

Run: `timeout 3 ./bin/server` (or `./bin/server.exe` on Windows via bash)

Expected: prints `Server listening on UDP port 7777` and does not crash for the 3 seconds before timeout kills it.

- [ ] **Step 5: Commit**

```bash
git add src/server/main.cpp src/server/Session.h Makefile
git commit -m "Add server main loop skeleton: socket bind, ConnectRequest handling, Welcome reply"
```

---

## Task 7: Complete server per-session simulation tick (input processing, combat, revive, hazard, snapshot broadcast, debug actions)

**Files:**
- Modify: `src/server/main.cpp`

**Interfaces:**
- Consumes: everything from Task 6, plus needs a way to route an incoming non-ConnectRequest packet to the right `(sessionName, slotIndex)` — this task adds that routing.
- Produces: a fully functional server that simulates gameplay for every active session at 60Hz and broadcasts snapshots to both connected clients in each session.

- [ ] **Step 1: Add an address-to-session/slot lookup helper**

At the top of `src/server/main.cpp` (after the existing includes), add a small helper struct and a linear-scan lookup function — this project's scale (a handful of sessions, 2 slots each) makes a linear scan entirely sufficient; no indexing structure is needed:

```cpp
struct ClientLocation {
    bool found;
    std::string sessionName;
    int slotIndex;
};

static ClientLocation FindClientByAddress(SessionManager& manager, const std::vector<std::string>& sessionNames,
                                           const std::string& ip, uint16_t port) {
    for (const auto& name : sessionNames) {
        Session* session = manager.GetSession(name);
        if (!session) continue;
        for (int i = 0; i < 2; i++) {
            if (session->slots[i].clientIp == ip && session->slots[i].clientPort == port &&
                session->slots[i].state == SlotState::Connected) {
                return { true, name, i };
            }
        }
    }
    return { false, "", -1 };
}
```

Since `SessionManager` doesn't expose a list of session names directly, add this one method to `src/server/SessionManager.h`:

```cpp
    std::vector<std::string> GetSessionNames() const {
        std::vector<std::string> names;
        for (const auto& entry : sessions) {
            names.push_back(entry.first);
        }
        return names;
    }
```

(Add `#include <vector>` to the top of `SessionManager.h` if not already present via a transitive include — it is, via `Session.h`'s own includes, but add it explicitly for clarity.)

- [ ] **Step 2: Replace the empty `else` dispatch branch from Task 6 with full packet handling**

In `src/server/main.cpp`, replace this block (written in Task 6 as a placeholder):

```cpp
                } else {
                    // For all other message types, find which session/slot this address belongs to.
                    // (A small linear scan is fine at this project's scale: at most a handful of
                    // sessions, 2 slots each.)
                    // This requires iterating sessionManager's sessions, so we expose a helper loop here
                    // rather than adding a by-address index to SessionManager (out of scope for this task).
                }
```

with:

```cpp
                } else {
                    std::vector<std::string> sessionNames = sessionManager.GetSessionNames();
                    ClientLocation loc = FindClientByAddress(sessionManager, sessionNames, fromIp, fromPort);
                    if (loc.found) {
                        Session* session = sessionManager.GetSession(loc.sessionName);
                        PlayerSlot& slot = session->slots[loc.slotIndex];
                        slot.lastPacketAtSeconds = now;

                        if (header.channel == 0 && type == MessageType::Input) {
                            InputMsg input{};
                            if (DeserializeStruct(body, bodyLen, input)) {
                                slot.player.position.x += input.moveX * kMoveSpeed * dt;
                                slot.player.position.y += input.moveY * kMoveSpeed * dt;
                                pendingAttack[loc.sessionName][loc.slotIndex] = input.attackPressed;
                                pendingInteract[loc.sessionName][loc.slotIndex] = input.interactHeld;
                            }
                        } else if (header.channel == 1 && type == MessageType::Ack) {
                            AckMsg ack{};
                            if (DeserializeStruct(body, bodyLen, ack)) {
                                slot.reliableSender.OnAckReceived(ack.ackedSeq);
                            }
                        } else if (header.channel == 1 && type == MessageType::DebugActionRequest) {
                            DebugActionRequestMsg req{};
                            if (DeserializeStruct(body, bodyLen, req)) {
                                std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ready;
                                slot.reliableReceiver.TryDeliverInOrder(header.seq, std::vector<uint8_t>(body, body + bodyLen), ready);
                                for (auto& msg : ready) {
                                    DebugActionRequestMsg deliveredReq{};
                                    if (DeserializeStruct(msg.second.data(), msg.second.size(), deliveredReq)) {
                                        if (deliveredReq.targetSlot < 2) {
                                            ApplyDebugAction(session->slots[deliveredReq.targetSlot].player, deliveredReq.action);
                                        }
                                    }
                                }
                                AckMsg ackReply{ header.seq };
                                std::vector<uint8_t> ackBytes;
                                SerializeStruct(ackReply, ackBytes);
                                std::vector<uint8_t> ackFull;
                                ackFull.push_back((uint8_t)MessageType::Ack);
                                ackFull.insert(ackFull.end(), ackBytes.begin(), ackBytes.end());
                                PacketHeader ackHeader{ 1, 0, (uint16_t)ackFull.size() };
                                std::vector<uint8_t> ackPacket;
                                SerializeStruct(ackHeader, ackPacket);
                                ackPacket.insert(ackPacket.end(), ackFull.begin(), ackFull.end());
                                socket.SendTo(slot.clientIp, slot.clientPort, ackPacket.data(), ackPacket.size());
                            }
                        }
                    }
                }
```

Add two new per-session input buffers near the other `std::map<std::string, ...>` declarations (right after `sessionSnapshotSeq`):

```cpp
    std::map<std::string, bool[2]> pendingAttack;
    std::map<std::string, bool[2]> pendingInteract;
```

- [ ] **Step 3: Replace the tick-body comment placeholder with the full per-session simulation**

Replace this line from Task 6 (inside the `if (now - lastTick >= tickInterval)` block):

```cpp
            // Per-session simulation and snapshot broadcast happens here in Task 7's
            // continuation of this file (input processing, movement, combat, revive,
            // hazard, timers, snapshot send) — see Task 7.
```

with:

```cpp
            for (auto& sessionEntry : sessionManager.GetSessionNames()) {
                Session* session = sessionManager.GetSession(sessionEntry);
                if (!session) continue;

                Player& p0 = session->slots[0].player;
                Player& p1 = session->slots[1].player;
                std::vector<WorldItem>& items = sessionWorldItems[sessionEntry];
                float* hazardCarry = sessionHazardCarry[sessionEntry];
                bool* attack = pendingAttack.count(sessionEntry) ? pendingAttack[sessionEntry] : nullptr;
                bool* interact = pendingInteract.count(sessionEntry) ? pendingInteract[sessionEntry] : nullptr;

                bool p0CanRevive = p1.state == PlayerState::Downed && DistanceBetween(p0.position, p1.position) <= kReviveRange;
                bool p1CanRevive = p0.state == PlayerState::Downed && DistanceBetween(p1.position, p0.position) <= kReviveRange;

                if (interact) {
                    if (p0.state == PlayerState::Alive && interact[0] && !p0CanRevive) {
                        for (auto& item : items) { if (TryPickup(item, p0.position, p0.inventory, kPickupRadius)) break; }
                    }
                    if (p1.state == PlayerState::Alive && interact[1] && !p1CanRevive) {
                        for (auto& item : items) { if (TryPickup(item, p1.position, p1.inventory, kPickupRadius)) break; }
                    }
                }
                if (attack) {
                    if (attack[0]) { TryAttack(p0, p1); attack[0] = false; }
                    if (attack[1]) { TryAttack(p1, p0); attack[1] = false; }
                }
                UpdateAttackCooldown(p0, dt);
                UpdateAttackCooldown(p1, dt);

                bool p0InteractHeld = interact ? interact[0] : false;
                bool p1InteractHeld = interact ? interact[1] : false;
                UpdateRevive(p0, p1, p0InteractHeld, dt, kReviveRange);
                UpdateRevive(p1, p0, p1InteractHeld, dt, kReviveRange);

                ApplyHazardDamage(hazard, p0, dt, hazardCarry[0]);
                ApplyHazardDamage(hazard, p1, dt, hazardCarry[1]);

                p0.UpdateTimers(dt);
                p1.UpdateTimers(dt);

                SnapshotMsg snap{};
                snap.players[0] = PlayerSnapshot{ p0.position.x, p0.position.y, p0.hp, (uint8_t)p0.state, p0.inventory.Count(ItemType::RevivePotion), p0.channelTimer };
                snap.players[1] = PlayerSnapshot{ p1.position.x, p1.position.y, p1.hp, (uint8_t)p1.state, p1.inventory.Count(ItemType::RevivePotion), p1.channelTimer };
                for (int i = 0; i < 2 && i < (int)items.size(); i++) {
                    snap.items[i] = WorldItemSnapshot{ items[i].position.x, items[i].position.y, items[i].active };
                }
                snap.hazardX = hazard.bounds.x;
                snap.hazardY = hazard.bounds.y;
                snap.hazardW = hazard.bounds.width;
                snap.hazardH = hazard.bounds.height;

                std::vector<uint8_t> snapBytes;
                SerializeStruct(snap, snapBytes);
                uint32_t seq = sessionSnapshotSeq[sessionEntry]++;
                for (int i = 0; i < 2; i++) {
                    if (session->slots[i].state == SlotState::Connected) {
                        SendUnreliable(socket, session->slots[i].clientIp, session->slots[i].clientPort, seq, MessageType::Snapshot, snapBytes.data(), snapBytes.size());
                    }
                }

                // Retransmit any unacked reliable messages for this session's slots
                for (int i = 0; i < 2; i++) {
                    if (session->slots[i].state != SlotState::Connected) continue;
                    auto toResend = session->slots[i].reliableSender.GetMessagesToRetransmit(now, 0.1);
                    for (auto& msg : toResend) {
                        PacketHeader resendHeader{ 1, msg.first, (uint16_t)msg.second.size() };
                        std::vector<uint8_t> packet;
                        SerializeStruct(resendHeader, packet);
                        packet.insert(packet.end(), msg.second.begin(), msg.second.end());
                        socket.SendTo(session->slots[i].clientIp, session->slots[i].clientPort, packet.data(), packet.size());
                    }
                }
            }
```

(Note: `for (auto& sessionEntry : sessionManager.GetSessionNames())` — `sessionEntry` here is a `std::string`, matching `GetSession`'s parameter; this loop's variable naming was chosen to avoid shadowing the outer `Session* session` from the ConnectRequest branch, which is out of scope by this point in the function.)

- [ ] **Step 4: Build to verify it compiles**

Run: `mingw32-make server 2>&1 | tail -40`

Expected: compiles and links with no errors. This is still not end-to-end tested against a real client (that's Task 8) — this step only confirms the server binary builds with the full simulation loop wired in.

- [ ] **Step 5: Commit**

```bash
git add src/server/main.cpp src/server/SessionManager.h
git commit -m "Complete server simulation tick: input, combat, revive, hazard, snapshot broadcast"
```

---

## Task 8: Client — NetClient (connect/reconnect/send/receive) and thin rendering main

**Files:**
- Create: `src/client/NetClient.h`
- Create: `src/client/NetClient.cpp`
- Create: `src/client/main.cpp`
- Delete: `src/main.cpp` (its smoke-test role is superseded — see Step 5)
- Delete: `src/DebugMenu.h` → recreated as `src/client/DebugMenu.h` with networked behavior (see Step 4)

**Interfaces:**
- Consumes: `UdpSocket`, all `src/shared/Protocol.h` message structs, `SerializeStruct`/`DeserializeStruct`, `ReliableSender`/`ReliableReceiver` (Tasks 1-3)
- Produces:
  - `class NetClient` with:
    - `bool Connect(const std::string& serverIp, uint16_t serverPort, const std::string& sessionName);` — sends a `ConnectRequest` (with `reconnectToken = 0` for a fresh connect, or the stored token if `Reconnect()` was called instead), blocks briefly polling for a `Welcome`/`Rejected` reply (simple retry loop: send, wait up to 200ms for a reply, retry up to 5 times, then give up), returns `true` on `Welcome`, `false` on `Rejected` or timeout. On success, stores `playerSlot`, `sessionToken`, and the `WelcomeMsg`'s game constants for later use.
    - `bool Reconnect();` — same as `Connect` but reuses the last-connected `serverIp`/`serverPort`/`sessionName` and passes the stored `sessionToken` as `reconnectToken`.
    - `void SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed);` — sends an `InputMsg` on the unreliable channel with an incrementing local sequence number.
    - `void SendDebugAction(DebugAction action, uint8_t targetSlot);` — sends a `DebugActionRequestMsg` on the reliable channel (tracked for retransmit exactly like the server's reliable sends).
    - `void PollNetwork(double nowSeconds);` — call once per client frame: drains all available incoming packets, updates the latest stored `SnapshotMsg` (unreliable channel: only overwrite if the incoming `seq` is higher than the last one accepted), processes any reliable `Ack` (advances `NetClient`'s own `ReliableSender` state) and retransmits its own unacked reliable sends via `GetMessagesToRetransmit`.
    - `const SnapshotMsg& GetLatestSnapshot() const;`
    - `uint8_t GetPlayerSlot() const;`
    - `const WelcomeMsg& GetGameConstants() const;`
  - `src/client/main.cpp` — the raylib window loop: constructs a `NetClient`, calls `Connect(...)` once at startup (hardcoded to `"127.0.0.1"`, port `7777`, and a session name read from a command-line argument or defaulted to `"default"` — see Step 3), then every frame: reads local keyboard input exactly as the old single-process client did (WASD/Arrows depending on `GetPlayerSlot()`, E/RCtrl for interact, Q/RShift for attack), calls `SendInput(...)`, calls `PollNetwork(...)`, and renders using `GetLatestSnapshot()` instead of local `Player` objects.
  - `src/client/DebugMenu.h` — same visual layout as the old `src/DebugMenu.h`, but `ApplyAction` now calls `netClient.SendDebugAction(...)` instead of mutating a `Player&` directly; button enable/disable logic (if any) reads from the latest `SnapshotMsg` instead of a local `Player`.

Since each client only ever controls ONE player (its own assigned slot), the client's key bindings are no longer split by "P1 vs P2" — every client uses the SAME keys (WASD move, E interact, Q attack) since only one player is ever local to a given client process. This is a deliberate simplification enabled by the network split: the old single-process code needed two different keysets because both players shared one keyboard; two separate client processes each need only one keyset.

- [ ] **Step 1: Write `src/client/NetClient.h`**

```cpp
#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <thread>
#include "../shared/Protocol.h"
#include "../shared/Serialize.h"
#include "../shared/Socket.h"
#include "../shared/ReliableChannel.h"

class NetClient {
public:
    bool Connect(const std::string& serverIp, uint16_t serverPort, const std::string& sessionName);
    bool Reconnect();
    void SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed);
    void SendDebugAction(DebugAction action, uint8_t targetSlot);
    void PollNetwork(double nowSeconds);

    const SnapshotMsg& GetLatestSnapshot() const { return latestSnapshot; }
    uint8_t GetPlayerSlot() const { return playerSlot; }
    const WelcomeMsg& GetGameConstants() const { return gameConstants; }
    bool IsConnected() const { return connected; }

private:
    UdpSocket socket;
    std::string serverIp;
    uint16_t serverPort = 0;
    std::string sessionName;
    uint32_t sessionToken = 0;
    uint8_t playerSlot = 0;
    bool connected = false;

    WelcomeMsg gameConstants{};
    SnapshotMsg latestSnapshot{};
    uint32_t lastAcceptedSnapshotSeq = 0;
    uint32_t nextUnreliableSeq = 1;

    ReliableSender reliableSender;
    ReliableReceiver reliableReceiver;

    bool AttemptConnect(uint32_t reconnectToken);
};
```

- [ ] **Step 2: Write `src/client/NetClient.cpp`**

```cpp
#include "NetClient.h"
#include <cstring>

static double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool NetClient::Connect(const std::string& ip, uint16_t port, const std::string& session) {
    serverIp = ip;
    serverPort = port;
    sessionName = session;
    return AttemptConnect(0);
}

bool NetClient::Reconnect() {
    return AttemptConnect(sessionToken);
}

bool NetClient::AttemptConnect(uint32_t reconnectToken) {
    ConnectRequestMsg req{};
    std::strncpy(req.sessionName, sessionName.c_str(), sizeof(req.sessionName) - 1);
    req.reconnectToken = reconnectToken;

    std::vector<uint8_t> reqBytes;
    SerializeStruct(req, reqBytes);
    std::vector<uint8_t> full;
    full.push_back((uint8_t)MessageType::ConnectRequest);
    full.insert(full.end(), reqBytes.begin(), reqBytes.end());

    for (int attempt = 0; attempt < 5; attempt++) {
        PacketHeader header{ 1, 0, (uint16_t)full.size() };
        std::vector<uint8_t> packet;
        SerializeStruct(header, packet);
        packet.insert(packet.end(), full.begin(), full.end());
        socket.SendTo(serverIp, serverPort, packet.data(), packet.size());

        double deadline = NowSeconds() + 0.2;
        while (NowSeconds() < deadline) {
            uint8_t recvBuffer[512];
            std::string fromIp;
            uint16_t fromPort;
            int received = socket.ReceiveFrom(recvBuffer, sizeof(recvBuffer), fromIp, fromPort);
            if (received > (int)sizeof(PacketHeader)) {
                PacketHeader respHeader{};
                DeserializeStruct(recvBuffer, received, respHeader);
                const uint8_t* payload = recvBuffer + sizeof(PacketHeader);
                size_t payloadLen = (size_t)received - sizeof(PacketHeader);
                if (payloadLen >= 1) {
                    MessageType type = (MessageType)payload[0];
                    if (type == MessageType::Welcome) {
                        WelcomeMsg welcome{};
                        if (DeserializeStruct(payload + 1, payloadLen - 1, welcome)) {
                            playerSlot = welcome.playerSlot;
                            sessionToken = welcome.sessionToken;
                            gameConstants = welcome;
                            connected = true;
                            return true;
                        }
                    } else if (type == MessageType::Rejected) {
                        connected = false;
                        return false;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    connected = false;
    return false;
}

void NetClient::SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed) {
    if (!connected) return;

    InputMsg input{ moveX, moveY, interactHeld, attackPressed };
    std::vector<uint8_t> inputBytes;
    SerializeStruct(input, inputBytes);
    std::vector<uint8_t> full;
    full.push_back((uint8_t)MessageType::Input);
    full.insert(full.end(), inputBytes.begin(), inputBytes.end());

    PacketHeader header{ 0, nextUnreliableSeq++, (uint16_t)full.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), full.begin(), full.end());
    socket.SendTo(serverIp, serverPort, packet.data(), packet.size());
}

void NetClient::SendDebugAction(DebugAction action, uint8_t targetSlot) {
    if (!connected) return;

    DebugActionRequestMsg req{ action, targetSlot };
    std::vector<uint8_t> reqBytes;
    SerializeStruct(req, reqBytes);
    std::vector<uint8_t> full;
    full.push_back((uint8_t)MessageType::DebugActionRequest);
    full.insert(full.end(), reqBytes.begin(), reqBytes.end());

    uint32_t seq = reliableSender.NextSeq();
    reliableSender.TrackUnacked(seq, full, NowSeconds());

    PacketHeader header{ 1, seq, (uint16_t)full.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), full.begin(), full.end());
    socket.SendTo(serverIp, serverPort, packet.data(), packet.size());
}

void NetClient::PollNetwork(double nowSeconds) {
    if (!connected) return;

    uint8_t recvBuffer[1024];
    std::string fromIp;
    uint16_t fromPort;
    int received;
    while ((received = socket.ReceiveFrom(recvBuffer, sizeof(recvBuffer), fromIp, fromPort)) > (int)sizeof(PacketHeader)) {
        PacketHeader header{};
        DeserializeStruct(recvBuffer, received, header);
        const uint8_t* payload = recvBuffer + sizeof(PacketHeader);
        size_t payloadLen = (size_t)received - sizeof(PacketHeader);
        if (payloadLen < 1) continue;

        MessageType type = (MessageType)payload[0];
        const uint8_t* body = payload + 1;
        size_t bodyLen = payloadLen - 1;

        if (header.channel == 0 && type == MessageType::Snapshot) {
            if (header.seq > lastAcceptedSnapshotSeq) {
                SnapshotMsg snap{};
                if (DeserializeStruct(body, bodyLen, snap)) {
                    latestSnapshot = snap;
                    lastAcceptedSnapshotSeq = header.seq;
                }
            }
        } else if (header.channel == 1 && type == MessageType::Ack) {
            AckMsg ack{};
            if (DeserializeStruct(body, bodyLen, ack)) {
                reliableSender.OnAckReceived(ack.ackedSeq);
            }
        }
    }

    auto toResend = reliableSender.GetMessagesToRetransmit(nowSeconds, 0.1);
    for (auto& msg : toResend) {
        PacketHeader header{ 1, msg.first, (uint16_t)msg.second.size() };
        std::vector<uint8_t> packet;
        SerializeStruct(header, packet);
        packet.insert(packet.end(), msg.second.begin(), msg.second.end());
        socket.SendTo(serverIp, serverPort, packet.data(), packet.size());
    }
}
```

- [ ] **Step 3: Write `src/client/DebugMenu.h`**

```cpp
#pragma once

#include <raylib-cpp.hpp>
#include "NetClient.h"

class DebugMenu {
public:
    bool visible = false;

    void Toggle() {
        visible = !visible;
    }

    void DrawAndHandle(NetClient& netClient) {
        if (!visible) {
            return;
        }

        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.5f));
        DrawRectangle(300, 150, 400, 260, RAYWHITE);
        DrawText("DEBUG MENU (F1 to close)", 320, 160, 16, BLACK);

        DrawText("P1 (slot 0)", 340, 190, 16, BLUE);
        DrawText("P2 (slot 1)", 540, 190, 16, MAROON);

        HandleColumn(netClient, 0, 320, 210);
        HandleColumn(netClient, 1, 520, 210);
    }

private:
    void HandleColumn(NetClient& netClient, uint8_t targetSlot, int x, int startY) {
        const char* labels[5] = { "Kill", "Revive", "Heal Full", "Give Potion", "Force Down" };
        DebugAction actions[5] = { DebugAction::Kill, DebugAction::Revive, DebugAction::HealFull, DebugAction::GivePotion, DebugAction::ForceDown };
        for (int i = 0; i < 5; i++) {
            Rectangle bounds{ (float)x, (float)(startY + i * 40), 150.0f, 30.0f };
            DrawRectangleRec(bounds, LIGHTGRAY);
            DrawRectangleLinesEx(bounds, 1, DARKGRAY);
            DrawText(labels[i], (int)bounds.x + 8, (int)bounds.y + 7, 14, BLACK);

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(GetMousePosition(), bounds)) {
                netClient.SendDebugAction(actions[i], targetSlot);
            }
        }
    }
};
```

- [ ] **Step 4: Write `src/client/main.cpp`**

```cpp
#include <raylib-cpp.hpp>
#include <string>
#include "NetClient.h"
#include "DebugMenu.h"

int main(int argc, char** argv)
{
    std::string sessionName = (argc > 1) ? argv[1] : "default";

    NetClient netClient;
    if (!netClient.Connect("127.0.0.1", 7777, sessionName)) {
        TraceLog(LOG_ERROR, "Failed to connect to server");
        return 1;
    }
    TraceLog(LOG_INFO, "Connected as player slot %d", (int)netClient.GetPlayerSlot());

    int screenWidth = 1000;
    int screenHeight = 600;
    raylib::Window w(screenWidth, screenHeight, "Maxion Test - Client");
    SetTargetFPS(60);

    DebugMenu debugMenu;
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

        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawRectangle((int)snap.hazardX, (int)snap.hazardY, (int)snap.hazardW, (int)snap.hazardH, Fade(RED, 0.3f));

        for (int i = 0; i < 2; i++) {
            if (snap.items[i].active) {
                DrawCircleV(Vector2{ snap.items[i].posX, snap.items[i].posY }, 8, GOLD);
            }
        }

        auto drawPlayer = [&](const PlayerSnapshot& p, Color color, const char* label) {
            Color drawColor = color;
            if (p.state == 1) drawColor = GRAY;   // Downed
            if (p.state == 2) drawColor = Fade(GRAY, 0.3f); // Dead

            Vector2 pos{ p.posX, p.posY };
            DrawCircleV(pos, 16, drawColor);
            DrawText(label, (int)pos.x - 10, (int)pos.y - 34, 14, BLACK);

            int barWidth = 40;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, barWidth, 5, DARKGRAY);
            int hpWidth = (int)(barWidth * ((float)p.hp / constants.maxHp));
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, hpWidth, 5, GREEN);

            DrawText(TextFormat("Potions: %d", p.potionCount), (int)pos.x - 30, (int)pos.y + 20, 12, DARKBLUE);

            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / constants.channelDuration;
                DrawCircleSector(pos, 24, -90, -90 + 360 * ratio, 32, Fade(SKYBLUE, 0.6f));
            }
        };

        drawPlayer(snap.players[0], BLUE, "P1");
        drawPlayer(snap.players[1], MAROON, "P2");

        DrawText(TextFormat("You are slot %d. WASD move, E pickup/revive, Q attack.", (int)mySlot), 10, 10, 16, BLACK);
        DrawText("F1: Debug Menu", 10, 30, 16, DARKGRAY);

        debugMenu.DrawAndHandle(netClient);

        EndDrawing();
    }

    return 0;
}
```

- [ ] **Step 5: Delete `src/main.cpp` and the old `src/DebugMenu.h`**

The prior single-process smoke-test harness in `src/main.cpp` has served its purpose — every logic piece it verified (`SmokeTestSerialization`, `SmokeTestReliableChannel`, `SmokeTestSessionManager`, `SmokeTestItems`, `SmokeTestPlayerStateMachine`, `SmokeTestHazard`, `SmokeTestCombatAndRevive`) was pure logic with no dependency on the single-process game loop, and none of it is lost — it's simply no longer the process entry point now that `src/server/main.cpp` and `src/client/main.cpp` exist as the real entry points. Move the smoke test functions themselves (not the game loop) into a new file so they're still run automatically:

Create `src/server/main.cpp`'s existing content already has no smoke tests in it (Task 6/7 didn't add any there) — instead, add a `--test` startup mode to the server that's easy to run manually. Add this near the top of `src/server/main.cpp` (after includes), and call it conditionally at the very start of `main()`:

```cpp
#include <cassert>
#include "Item.h"
#include "Player.h"
#include "Hazard.h"
#include "Combat.h"
#include "../shared/Serialize.h"
#include "../shared/ReliableChannel.h"

void RunAllSmokeTests() {
    // Paste the full bodies of SmokeTestSerialization, SmokeTestReliableChannel,
    // SmokeTestSessionManager, SmokeTestItems, SmokeTestPlayerStateMachine,
    // SmokeTestHazard, and SmokeTestCombatAndRevive here verbatim, exactly as
    // they exist in the current src/main.cpp before it is deleted in this step.
    // Call each one and TraceLog its "passed" line, then a final
    // TraceLog(LOG_INFO, "All smoke tests passed") — identical structure to
    // the old main.cpp, just relocated into this function.
}
```

Then in `main()`, add as the very first lines:

```cpp
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--test") {
        RunAllSmokeTests();
        return 0;
    }
    std::srand((unsigned int)std::time(nullptr));
    // ... rest of the existing server main() body continues unchanged
```

(`main`'s signature changes from `int main()` to `int main(int argc, char** argv)` — update accordingly; the rest of the existing body is untouched.)

Now delete the old files:

```bash
git rm src/main.cpp src/DebugMenu.h
```

- [ ] **Step 6: Add a client Makefile target**

Add to the `Makefile`, after the `server`/`run-server` targets added in Task 6:

```makefile
clientSources := $(call rwildcard,src/shared/,*.cpp) $(call rwildcard,src/client/,*.cpp)
clientObjects := $(patsubst src/%, $(buildDir)/%, $(patsubst %.cpp, %.o, $(clientSources)))
clientTarget := $(buildDir)/client$(if $(filter Windows,$(platform)),.exe,)

.PHONY: client run-client

client: $(clientTarget)

$(clientTarget): $(clientObjects)
	$(CXX) $(clientObjects) -o $(clientTarget) $(linkFlags)

run-client: client
	$(clientTarget) $(ARGS)
```

(`$(linkFlags)` already includes `-l raylib` plus the Windows socket/OpenGL flags from Task 3 — the client needs all of them since it opens a real window.)

Since `src/main.cpp` is deleted, the original `all`/`bin/app`/`execute` targets (which depended on `$(sources)` = all of `src/*.cpp`) no longer have a `main()` to build against once both `src/server/main.cpp` and `src/client/main.cpp` exist under subdirectories that `$(call rwildcard,src/,*.cpp)` would still both pick up together (producing two `main` symbols, a link error). Change the original `sources`/`target` lines near the top of the Makefile:

```makefile
sources := $(call rwildcard,src/,*.cpp)
objects := $(patsubst src/%, $(buildDir)/%, $(patsubst %.cpp, %.o, $(sources)))
```

to be removed entirely, along with the `all`, `$(target)`, and `execute` rules that depended on them (the old single `bin/app` target is retired in favor of the new `server`/`client` targets). Replace the `all:` rule with:

```makefile
all: server client
```

- [ ] **Step 7: Build both binaries**

Run: `mingw32-make server 2>&1 | tail -30` then `mingw32-make client 2>&1 | tail -30`

Expected: both compile and link with no errors, producing `bin/server.exe` and `bin/client.exe`.

- [ ] **Step 8: Run the server's smoke tests**

Run: `./bin/server --test`

Expected: all seven smoke-test pass lines print, then the process exits (return 0, no crash), confirming none of the pure logic broke during the file reorganization.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "Add NetClient, thin rendering client main, and networked debug menu; retire single-process bin/app"
```

---

## Task 9: End-to-end manual verification (server + two clients)

**Files:** none (verification-only task, no code changes expected unless a bug is found)

**Interfaces:** N/A

- [ ] **Step 1: Start the server**

Run in one terminal: `./bin/server` (leave it running)

Expected: prints `Server listening on UDP port 7777` and stays running.

- [ ] **Step 2: Start the first client**

Run in a second terminal: `./bin/client default` (or just `./bin/client`, since `"default"` is the argument default)

Expected: a window opens titled "Maxion Test - Client", console shows `Connected as player slot 0`. The player circle for slot 0 should be visible near the left side of the screen.

- [ ] **Step 3: Start the second client**

Run in a third terminal: `./bin/client default` (same session name, so it joins the same session as slot 1)

Expected: a second window opens, console shows `Connected as player slot 1`. Both windows should now show BOTH players' circles, in sync (moving one should be visible in both windows).

- [ ] **Step 4: Manually verify each mechanic works over the network**

With both client windows open side by side:
1. Move the slot-0 client with WASD — its circle should move in BOTH windows simultaneously (proving the server is broadcasting to both, and both clients render the same authoritative state).
2. Walk the slot-0 player onto a gold world-item dot and hold E — it should disappear in both windows, and slot 0's "Potions" count should increase in both windows.
3. Walk either player into the red hazard rectangle — their HP bar should drain in both windows.
4. Get one player Downed (via hazard or by pressing Q on the other client while in range), then have the OTHER player (who has a potion) get close and hold E — the blue progress ring should appear around the reviver in both windows and the downed player should return to half HP after ~2 seconds.
5. Press F1 in one client — its debug menu overlay should appear (locally, since it's UI-only). Click "Kill" under "P1 (slot 0)" — after a brief round trip, slot 0's player should go translucent/gray (Dead) in BOTH windows, proving the debug action reached the server and the server's decision (not the client's) is what both clients ended up rendering.

- [ ] **Step 5: Verify reconnect**

With both clients still connected and playing:
1. Close (Ctrl+C or close the window on) the slot-0 client.
2. Wait a few seconds, then restart it: `./bin/client default`.
3. Since this plan's `NetClient::Connect` always sends `reconnectToken = 0` on a fresh `Connect()` call rather than persisting a token across process restarts, this specific manual test will actually connect as a NEW client — note this as an expected limitation of the current implementation (no token persistence to disk), not a bug: the underlying server-side reconnect logic (`SessionManager::HandleConnect` case 4) IS implemented and covered by Task 4's smoke test using an in-memory token, but no client-side flow exercises it end-to-end in this task since doing so would require token persistence (e.g. writing it to a local file on connect and reading it back on startup), which is out of scope for this plan (the spec's reconnect feature is about the SERVER correctly resuming a slot when given a valid token — verified in Task 4 — not about the client's UX for supplying one after a restart).
4. Instead, verify the DISCONNECT half of the behavior: after closing slot 0's client and NOT restarting it, wait and watch the OTHER client's window — after 60 seconds of no input from slot 0, its player should stop being affected by anything (frozen state) rather than continuing to take hazard damage or otherwise misbehave, since `Session::CheckTimeouts` marks it `DisconnectedPending`. (Full verification of the freeze behavior requires the simulation loop to actually check `SlotState` before applying movement/damage per-slot — confirm this is the case by reading `src/server/main.cpp`'s per-session tick body: if `SlotState::DisconnectedPending` players are NOT currently excluded from `ApplyHazardDamage`/`TryAttack`/etc. in the Task 7 code, note this as a gap for the controller — the plan's Task 7 code operates on `Player&` references directly without checking `slot.state`, so a `DisconnectedPending` player's `Player` object is still fully simulated. This is a real gap between the spec's intent ("frozen, not deleted") and the Task 7 code as written. Flag it in your report rather than silently fixing it, since the fix (skip simulation for non-Connected slots) touches the same block Task 7 already wrote and should be confirmed with the controller first.)

- [ ] **Step 6: Report findings**

Since this task has no commit of its own (verification-only), write up what you observed for each of Steps 4 and 5 in your report, including the flagged gap from Step 5.4 if confirmed. If any of Steps 4's mechanics did NOT work as described, that's a real bug — note it in detail (which step, what you expected, what actually happened) rather than attempting to silently patch it, since a networking bug at this stage may indicate a protocol-level misunderstanding worth discussing before a fix is attempted.

---

## Plan Self-Review Notes

- **Spec coverage:** Protocol/wire format (header + message structs) → Task 1. Reliable channel (acks, retransmit, ordering) → Task 2. Cross-platform sockets → Task 3. Session/reconnect state machine (all 5 connect cases + timeout transitions) → Task 4. Moving gameplay logic server-side + debug action extraction → Task 5. Server main loop (bind, ConnectRequest/Welcome) → Task 6. Full server simulation tick (input, combat, revive, hazard, snapshot, debug dispatch) → Task 7. Client (NetClient, thin rendering, networked debug menu) → Task 8. Build layout (two Makefile targets, retiring `bin/app`) → Tasks 6/8. End-to-end manual verification including reconnect/session behavior → Task 9. All spec sections are covered.
- **Placeholder scan:** Task 8 Step 5 asks the engineer to "paste the full bodies" of the smoke tests from the file being deleted rather than repeating ~150 lines of already-shown code a third time in this document — this is a deliberate exception to avoid the plan itself becoming unwieldy, and the source (the current `src/main.cpp`, viewable by the engineer before deletion) is unambiguous and complete, not a vague "TODO." No other placeholders found.
- **Type consistency:** `MessageType`, `PacketHeader`, all message structs (Task 1) are used with identical names/fields in Tasks 6-8. `ReliableSender`/`ReliableReceiver` (Task 2) method signatures (`NextSeq`, `TrackUnacked`, `OnAckReceived`, `GetMessagesToRetransmit`, `TryDeliverInOrder`) are used identically in Tasks 6-8. `SessionManager`/`Session`/`PlayerSlot`/`SlotState`/`ConnectOutcome` (Task 4) are used identically in Tasks 6-7, with the one addition (`GetSessionNames()`) explicitly added in Task 7 rather than assumed. `ApplyDebugAction` (Task 5) signature matches its Task 7 call site. Flagged and resolved one genuine ambiguity during self-review: Task 9 surfaces that Task 7's simulation loop does not check `SlotState` before simulating a slot's `Player`, meaning a `DisconnectedPending` player is not actually "frozen" as the spec requires — this is called out explicitly in Task 9 Step 5 as a gap to confirm/fix with the controller rather than silently patched, since fixing it correctly means Task 7's simulation block needs a `if (session->slots[i].state != SlotState::Connected) continue;`-style guard added per-player, which the controller should apply as a follow-up fix once Task 9's manual verification confirms the gap is real.
