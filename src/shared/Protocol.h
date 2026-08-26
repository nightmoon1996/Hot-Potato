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
    InvalidToken,
    RoomNotFound
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
    char roomCode[7]; // 6 digits + null terminator
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
