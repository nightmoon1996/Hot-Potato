#pragma once

#include <cstdint>

// Maximum players (and therefore slots) per session. Shared by client and server
// so array sizes and loop bounds on both sides stay in lockstep.
static constexpr int kMaxPlayersPerSession = 4;

// Number of rounds in a normal match before tiebreak resolution kicks in (Phase 3).
constexpr int kRoundsPerMatch = 3;

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
    InvalidToken,
    RoomNotFound
};

enum class GameMode : uint8_t {
    FFA,
    TwoVTwo,
    RevivePotionTest
};

enum class DebugAction : uint8_t {
    Kill,
    Revive,
    HealFull,
    GivePotion,
    ForceDown,
    // Session-scoped, NOT per-player: resets the session's MatchState and respawns the
    // potato for a fresh round. `targetSlot` is ignored. Handled at the server's
    // DebugActionRequest dispatch site in main.cpp, deliberately NOT inside
    // ApplyDebugAction (which has a per-player contract and no session access).
    NewMatch
};

struct PacketHeader {
    uint8_t channel;      // 0 = unreliable-sequenced, 1 = reliable-ordered
    uint32_t seq;
    uint16_t payloadLen;
};

struct ConnectRequestMsg {
    char sessionName[32];
    uint32_t reconnectToken; // 0 = no token
    GameMode requestedMode; // only meaningful when sessionName is empty (a create request);
                            // ignored when joining an existing room, since the room's mode
                            // was already fixed by whoever created it
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
    char roomCode[7]; // 6 digits + null terminator
    GameMode gameMode; // the ACTUAL mode of the room the caller ended up in (create or join)
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

struct HotbarSlotSnapshot {
    uint8_t itemType; // meaningless when count == 0 (empty slot)
    int count;
};

struct PlayerSnapshot {
    float posX;
    float posY;
    int hp;
    uint8_t state; // 0 = Alive, 1 = Downed, 2 = Dead
    HotbarSlotSnapshot slots[4]; // fixed 4-slot hotbar contents, index-for-index with Inventory
    float channelTimer;
};

struct WorldItemSnapshot {
    float posX;
    float posY;
    bool active;
};

struct PotatoSnapshot {
    float posX;
    float posY;
    bool held;
    bool inFlight;
    int holderSlot; // -1 if unheld/mid-flight
    float explodeTimer;
};

struct MatchSnapshot {
    int roundNumber;
    int roundScore[kMaxPlayersPerSession];
    bool matchOver;
    int winnerSlot; // -1 if undecided
    bool inTiebreak;
    int teamScore[2]; // 2v2 only: teamScore[0] = Team A (slots 0-1), teamScore[1] = Team B (slots 2-3)
};

struct SnapshotMsg {
    PlayerSnapshot players[kMaxPlayersPerSession];
    // Intentionally 2, not kMaxPlayersPerSession: the world-item count is unrelated to
    // player count (a later phase reworks item spawning). Do not "fix" this to match above.
    WorldItemSnapshot items[2];
    float hazardX;
    float hazardY;
    float hazardW;
    float hazardH;
    PotatoSnapshot potato;
    MatchSnapshot match;
};
