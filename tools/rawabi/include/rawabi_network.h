#pragma once

#include "rawabi_common.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace rawabi {

struct SenderOptions {
    std::string destination_ip = "127.0.0.1";
    uint16_t destination_port = 10000;
    uint32_t flow_id = 1;
    uint16_t fragment_payload = 1400;
    double loss_percent = 0.0;
    double duplicate_percent = 0.0;
    uint32_t reorder_window = 0;
};

struct ReorderBuffer {
    std::deque<std::vector<uint8_t>> pending;
    uint32_t window = 0;
};

struct FrameStats {
    uint64_t frames_completed = 0;
    uint64_t frames_dropped = 0;
    uint64_t fd_packets = 0;
    uint64_t fh_packets = 0;
    uint64_t bytes = 0;
    uint32_t reorder_depth = 0;
};

class FrameReassembler {
  public:
    FrameReassembler();
    std::vector<CompletedFrame> ingest(const uint8_t *packet, std::size_t len);
    FrameStats stats() const;
    void expire_older_than(uint32_t recent_fseq, uint32_t max_distance = 4);

  private:
    struct PendingFrame {
        FrameInfo info;
        uint32_t fseq32 = 0;
        uint8_t fseq8 = 0;
        uint64_t ts = 0;
        std::vector<uint8_t> data;
        std::vector<bool> received;
        std::size_t expected = 0;
        std::size_t received_bytes = 0;
        bool has_fh = false;
    };

    std::unordered_map<uint32_t, PendingFrame> frames_; // keyed by fseq32
    FrameStats stats_{};
    mutable std::mutex mtx_;

    std::optional<PendingFrame> handle_fh(const uint8_t *packet, std::size_t len);
    std::optional<CompletedFrame> handle_fd(const uint8_t *packet, std::size_t len);
};

std::vector<FramePacket> fragment_frame(const FrameInfo &info,
                                        uint32_t fseq32,
                                        uint64_t timestamp_us,
                                        const uint8_t *payload,
                                        std::size_t payload_size,
                                        const SenderOptions &opts,
                                        ReorderBuffer &reorder);

} // namespace rawabi
