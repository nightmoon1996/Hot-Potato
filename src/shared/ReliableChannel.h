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
