#pragma once

#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
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
    std::string roomCode;
};

class SessionManager {
public:
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

    std::vector<std::string> GetSessionNames() const {
        std::vector<std::string> names;
        for (const auto& entry : sessions) {
            names.push_back(entry.first);
        }
        return names;
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
};
