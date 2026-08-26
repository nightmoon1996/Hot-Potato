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
