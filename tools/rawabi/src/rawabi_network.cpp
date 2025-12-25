#include "rawabi_network.h"

#include <arpa/inet.h>
#include <endian.h>
#include <random>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace rawabi {

namespace {
uint64_t monotonic_us() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

uint32_t offset_from_offs(uint32_t offs) {
    return offs & 0x0FFFFFFFu;
}
}

FrameReassembler::FrameReassembler() = default;

std::vector<CompletedFrame> FrameReassembler::ingest(const uint8_t *packet, std::size_t len) {
    std::vector<CompletedFrame> out;
    if (!packet || len < sizeof(uint32_t)) {
        return out;
    }

    uint32_t lid = *reinterpret_cast<const uint32_t *>(packet);
    if (lid & LID_TYPE) {
        auto fh = handle_fh(packet, len);
        if (fh.has_value()) {
            std::lock_guard<std::mutex> lock(mtx_);
            auto pf = std::move(fh.value());
            frames_[pf.fseq32] = std::move(pf);
            stats_.fh_packets++;
            if (frames_.size() > stats_.reorder_depth) {
                stats_.reorder_depth = static_cast<uint32_t>(frames_.size());
            }
        }
        return out;
    }

    auto complete = handle_fd(packet, len);
    if (complete.has_value()) {
        out.push_back(std::move(complete.value()));
    }
    return out;
}

FrameStats FrameReassembler::stats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

void FrameReassembler::expire_older_than(uint32_t recent_fseq, uint32_t max_distance) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (recent_fseq >= it->first && recent_fseq - it->first > max_distance) {
            stats_.frames_dropped++;
            it = frames_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<FrameReassembler::PendingFrame> FrameReassembler::handle_fh(const uint8_t *packet, std::size_t len) {
    if (len < kFhHeaderSize) {
        return std::nullopt;
    }
    const video_frame_raw_hdr_t *fh = reinterpret_cast<const video_frame_raw_hdr_t *>(packet);
    PendingFrame pf;
    pf.fseq32 = ntohl(fh->fseq);
    pf.fseq8 = static_cast<uint8_t>(pf.fseq32 & 0xFFu);
    pf.ts = be64toh(fh->ts);
    pf.info.width = ntohs(fh->x_dim);
    pf.info.height = ntohs(fh->y_dim);
    pf.info.fmt = sample_format_from_bits(ntohl(fh->fsize));
    pf.info.flow_id = fh->lid & 0x7FFFFFFFu;
    pf.info.pattern = BayerPattern::MONO;
    pf.has_fh = true;
    pf.expected = ntohl(fh->fsize) & 0x0FFFFFFFu;
    if (pf.expected == 0 || pf.expected > kMaxFrameBytes) {
        return std::nullopt;
    }
    pf.data.resize(pf.expected);
    pf.received.resize((pf.expected + 1023) / 1024, false);
    return pf;
}

std::optional<CompletedFrame> FrameReassembler::handle_fd(const uint8_t *packet, std::size_t len) {
    if (len < kFdHeaderSize) {
        return std::nullopt;
    }
    const video_frame_raw_t *fd = reinterpret_cast<const video_frame_raw_t *>(packet);
    uint16_t payload_size = ntohs(fd->size);
    if (kFdHeaderSize + payload_size > len) {
        return std::nullopt;
    }

    uint32_t offs_raw = ntohl(fd->offs);
    uint32_t offset = offset_from_offs(offs_raw);
    auto fmt = sample_format_from_bits(offs_raw);
    uint16_t width = ntohs(fd->x_dim);
    uint16_t height = ntohs(fd->y_dim);
    uint8_t fseq8 = fd->fseq;
    uint32_t flow_id = fd->lid & 0x7FFFFFFFu;
    BayerPattern pattern = bayer_from_flag(fd->flags);

    std::lock_guard<std::mutex> lock(mtx_);
    stats_.fd_packets++;
    stats_.bytes += payload_size;
    if (frames_.size() > stats_.reorder_depth) {
        stats_.reorder_depth = static_cast<uint32_t>(frames_.size());
    }

    PendingFrame *pf = nullptr;
    for (auto &kv : frames_) {
        if (kv.second.fseq8 == fseq8 && kv.second.info.flow_id == flow_id) {
            pf = &kv.second;
            break;
        }
    }
    if (!pf) {
        PendingFrame guess;
        guess.fseq8 = fseq8;
        guess.fseq32 = static_cast<uint32_t>(fseq8);
        guess.info.width = width;
        guess.info.height = height;
        guess.info.fmt = fmt;
        guess.info.flow_id = flow_id;
        guess.info.pattern = BayerPattern::MONO;
        uint8_t bits = sample_bits(fmt);
        std::size_t bpp = (bits + 7u) / 8u;
        if (bpp == 0) bpp = 1;
        guess.expected = static_cast<std::size_t>(width) * height * bpp;
        if (guess.expected == 0 || guess.expected > kMaxFrameBytes) {
            stats_.frames_dropped++;
            return std::nullopt;
        }
        guess.data.resize(guess.expected);
        guess.received.resize((guess.expected + 1023) / 1024, false);
        auto inserted = frames_.emplace(guess.fseq32, std::move(guess));
        pf = &inserted.first->second;
    }

    if (offset + payload_size > pf->data.size()) {
        stats_.frames_dropped++;
        return std::nullopt;
    }
    std::memcpy(pf->data.data() + offset, packet + kFdHeaderSize, payload_size);
    std::size_t block = offset / 1024;
    if (block < pf->received.size()) {
        pf->received[block] = true;
    }
    pf->received_bytes += payload_size;
    pf->info.fmt = fmt;
    pf->info.width = width;
    pf->info.height = height;
    pf->info.pattern = pattern;

    bool complete = pf->expected > 0 ? (pf->received_bytes >= pf->expected) : false;
    if (complete) {
        CompletedFrame cf;
        cf.info = pf->info;
        cf.fseq32 = pf->fseq32;
        cf.timestamp = pf->ts ? pf->ts : monotonic_us();
        cf.payload = std::move(pf->data);
        stats_.frames_completed++;
        frames_.erase(pf->fseq32);
        return cf;
    }

    return std::nullopt;
}

std::vector<FramePacket> fragment_frame(const FrameInfo &info,
                                        uint32_t fseq32,
                                        uint64_t timestamp_us,
                                        const uint8_t *payload,
                                        std::size_t payload_size,
                                        const SenderOptions &opts,
                                        ReorderBuffer &reorder) {
    std::vector<FramePacket> packets;
    if (!payload || payload_size == 0) {
        return packets;
    }

    video_frame_raw_hdr_t fh{};
    fh.lid = (uint32_t)LID_FH | (opts.flow_id & 0x7FFFFFFFu);
    fh.fseq = htonl(fseq32);
    fh.ts = htobe64(timestamp_us);
    fh.x_dim = htons(info.width);
    fh.y_dim = htons(info.height);
    fh.fsize = htonl((static_cast<uint32_t>(payload_size) & 0x0FFFFFFFu) | encode_sample_format(info.fmt));
    fh.osize = 0;

    FramePacket fh_packet;
    fh_packet.is_fh = true;
    fh_packet.data.resize(kFhHeaderSize);
    std::memcpy(fh_packet.data.data(), &fh, kFhHeaderSize);
    packets.push_back(std::move(fh_packet));

    uint32_t offset = 0;
    std::mt19937 rng(static_cast<uint32_t>(timestamp_us ^ payload_size));
    std::uniform_real_distribution<double> dist01(0.0, 100.0);

    while (offset < payload_size) {
        uint32_t chunk = std::min<uint32_t>(opts.fragment_payload, static_cast<uint32_t>(payload_size - offset));
        FramePacket pkt;
        pkt.is_fh = false;
        pkt.data.resize(kFdHeaderSize + chunk);
        auto *fd = reinterpret_cast<video_frame_raw_t *>(pkt.data.data());
        fd->lid = (opts.flow_id & 0x7FFFFFFFu) | (uint32_t)LID_FD;
        fd->flags = static_cast<uint8_t>(bayer_flag(info.pattern));
        fd->fseq = static_cast<uint8_t>(fseq32 & 0xFFu);
        fd->size = htons(static_cast<uint16_t>(chunk));
        fd->x_dim = htons(info.width);
        fd->y_dim = htons(info.height);
        fd->offs = htonl((offset & 0x0FFFFFFFu) | encode_sample_format(info.fmt));
        std::memcpy(fd->data, payload + offset, chunk);
        offset += chunk;

        double roll = dist01(rng);
        if (roll < opts.loss_percent) {
            continue;
        }
        if (roll < opts.loss_percent + opts.duplicate_percent) {
            packets.push_back(pkt);
        }

        if (opts.reorder_window > 0) {
            reorder.pending.push_back(pkt.data);
            if (reorder.pending.size() > opts.reorder_window) {
                FramePacket emit;
                emit.data = std::move(reorder.pending.front());
                reorder.pending.pop_front();
                packets.push_back(std::move(emit));
            }
        } else {
            packets.push_back(std::move(pkt));
        }
    }

    while (opts.reorder_window > 0 && !reorder.pending.empty()) {
        FramePacket emit;
        emit.data = std::move(reorder.pending.front());
        reorder.pending.pop_front();
        packets.push_back(std::move(emit));
    }

    return packets;
}

} // namespace rawabi
