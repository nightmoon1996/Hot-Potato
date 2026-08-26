#include "NetClient.h"
#include <cstring>

static double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void NetClient::SendAck(uint32_t ackedSeq) {
    AckMsg ack{ ackedSeq };
    std::vector<uint8_t> ackBytes;
    SerializeStruct(ack, ackBytes);
    std::vector<uint8_t> full;
    full.push_back((uint8_t)MessageType::Ack);
    full.insert(full.end(), ackBytes.begin(), ackBytes.end());

    PacketHeader header{ 1, 0, (uint16_t)full.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), full.begin(), full.end());
    socket.SendTo(serverIp, serverPort, packet.data(), packet.size());
}

bool NetClient::CreateRoom(const std::string& ip, uint16_t port, GameMode mode) {
    serverIp = ip;
    serverPort = port;
    sessionName = "";
    requestedMode = mode;
    return AttemptConnect(0);
}

bool NetClient::JoinRoom(const std::string& ip, uint16_t port, const std::string& roomCode) {
    serverIp = ip;
    serverPort = port;
    sessionName = roomCode;
    requestedMode = GameMode::FFA; // ignored by the server when sessionName is non-empty (a join)
    return AttemptConnect(0);
}

bool NetClient::Reconnect() {
    return AttemptConnect(sessionToken);
}

bool NetClient::AttemptConnect(uint32_t reconnectToken) {
    ConnectRequestMsg req{};
    std::strncpy(req.sessionName, sessionName.c_str(), sizeof(req.sessionName) - 1);
    req.reconnectToken = reconnectToken;
    req.requestedMode = requestedMode;

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
                            SendAck(respHeader.seq);
                            return true;
                        }
                    } else if (type == MessageType::Rejected) {
                        RejectedMsg reject{};
                        if (DeserializeStruct(payload + 1, payloadLen - 1, reject)) {
                            lastRejectReason = reject.reason;
                        }
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

void NetClient::SendInput(float moveX, float moveY, bool interactHeld, bool attackPressed,
                           bool chargingThrow, bool releaseThrow, float aimDirX, float aimDirY, bool dashPressed) {
    if (!connected) return;

    InputMsg input{ moveX, moveY, interactHeld, attackPressed, chargingThrow, releaseThrow, aimDirX, aimDirY, dashPressed };
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
                    receivedSnapshot = true;
                }
            }
        } else if (header.channel == 1 && type == MessageType::Ack) {
            AckMsg ack{};
            if (DeserializeStruct(body, bodyLen, ack)) {
                reliableSender.OnAckReceived(ack.ackedSeq);
            }
        } else if (header.channel == 1) {
            // Any other reliable message (e.g. Welcome retransmits, future server->client
            // reliable messages): run it through the receiver for in-order delivery
            // bookkeeping, and ack it unconditionally, even if it's a duplicate.
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ready;
            reliableReceiver.TryDeliverInOrder(header.seq, std::vector<uint8_t>(payload, payload + payloadLen), ready);
            SendAck(header.seq);
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
