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
    bool CreateRoom(const std::string& serverIp, uint16_t serverPort);
    bool JoinRoom(const std::string& serverIp, uint16_t serverPort, const std::string& roomCode);
    bool Reconnect();
    void SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed,
                   bool chargingThrow, bool releaseThrow, float aimDirX, float aimDirY);
    void SendDebugAction(DebugAction action, uint8_t targetSlot);
    void PollNetwork(double nowSeconds);

    const SnapshotMsg& GetLatestSnapshot() const { return latestSnapshot; }
    uint8_t GetPlayerSlot() const { return playerSlot; }
    const WelcomeMsg& GetGameConstants() const { return gameConstants; }
    bool IsConnected() const { return connected; }
    bool HasReceivedSnapshot() const { return receivedSnapshot; }
    const char* GetRoomCode() const { return gameConstants.roomCode; }
    RejectReason GetLastRejectReason() const { return lastRejectReason; }

private:
    UdpSocket socket;
    std::string serverIp;
    uint16_t serverPort = 0;
    std::string sessionName;
    uint32_t sessionToken = 0;
    uint8_t playerSlot = 0;
    bool connected = false;
    RejectReason lastRejectReason = RejectReason::SessionFull;

    WelcomeMsg gameConstants{};
    SnapshotMsg latestSnapshot{};
    bool receivedSnapshot = false;
    uint32_t lastAcceptedSnapshotSeq = 0;
    uint32_t nextUnreliableSeq = 1;

    ReliableSender reliableSender;
    ReliableReceiver reliableReceiver;

    bool AttemptConnect(uint32_t reconnectToken);
    void SendAck(uint32_t ackedSeq);
};
