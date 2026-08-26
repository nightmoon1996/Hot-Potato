#pragma once

#include <string>
#include <cstdint>
#include "../shared/Geometry.h"
#include "Player.h"
#include "../shared/ReliableChannel.h"

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
    ReliableSender reliableSender;
    ReliableReceiver reliableReceiver;

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
