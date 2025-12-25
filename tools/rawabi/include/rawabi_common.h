#pragma once

#include <abi/ip-video-raw.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rawabi {

constexpr std::size_t kFdHeaderSize = offsetof(video_frame_raw_t, data);
constexpr std::size_t kFhHeaderSize = offsetof(video_frame_raw_hdr_t, abi);
constexpr std::size_t kMaxFrameBytes = 32 * 1024 * 1024; // 32 MiB safety cap

enum class BayerPattern : uint8_t {
    BAYER_G1RG2B = 0,
    BAYER_RG1BG2 = 1,
    BAYER_G1BG2R = 2,
    BAYER_BG1RG2 = 3,
    MONO = 4,
};

inline uint8_t bayer_flag(BayerPattern p) {
    return static_cast<uint8_t>(p);
}

inline BayerPattern bayer_from_flag(uint8_t flag) {
    switch (flag & 0x1Fu) {
    case 0: return BayerPattern::BAYER_G1RG2B;
    case 1: return BayerPattern::BAYER_RG1BG2;
    case 2: return BayerPattern::BAYER_G1BG2R;
    case 3: return BayerPattern::BAYER_BG1RG2;
    default: return BayerPattern::MONO;
    }
}

inline std::string bayer_to_string(BayerPattern p) {
    switch (p) {
    case BayerPattern::BAYER_G1RG2B: return "g1rg2b";
    case BayerPattern::BAYER_RG1BG2: return "rg1bg2";
    case BayerPattern::BAYER_G1BG2R: return "g1bg2r";
    case BayerPattern::BAYER_BG1RG2: return "bg1rg2";
    case BayerPattern::MONO: return "mono";
    }
    return "mono";
}

inline std::optional<BayerPattern> bayer_from_string(const std::string &s) {
    if (s == "g1rg2b" || s == "grbg") return BayerPattern::BAYER_G1RG2B;
    if (s == "rg1bg2" || s == "rggb") return BayerPattern::BAYER_RG1BG2;
    if (s == "g1bg2r" || s == "gbrg") return BayerPattern::BAYER_G1BG2R;
    if (s == "bg1rg2" || s == "bggr") return BayerPattern::BAYER_BG1RG2;
    if (s == "mono" || s == "bw") return BayerPattern::MONO;
    return std::nullopt;
}

inline uint8_t sample_bits(enum sample_format fmt) {
    switch (fmt) {
    case sf_8bit: return 8;
    case sf_10bit: return 10;
    case sf_12bit: return 12;
    case sf_14bit: return 14;
    case sf_16bit: return 16;
    default: return 0;
    }
}

inline enum sample_format sample_format_from_bits(uint32_t offs_field) {
    uint32_t encoded = offs_field & 0xF0000000u;
    switch (encoded) {
    case sf_8bit: return sf_8bit;
    case sf_10bit: return sf_10bit;
    case sf_12bit: return sf_12bit;
    case sf_14bit: return sf_14bit;
    case sf_16bit: return sf_16bit;
    default: return sf_8bit;
    }
}

inline uint32_t encode_sample_format(enum sample_format fmt) {
    return static_cast<uint32_t>(fmt) & 0xF0000000u;
}

struct FrameInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    enum sample_format fmt = sf_10bit;
    BayerPattern pattern = BayerPattern::BAYER_G1RG2B;
    uint8_t orientation = 0;
    bool mirror = false;
    uint32_t flow_id = 1;
};

struct FramePacket {
    std::vector<uint8_t> data;
    bool is_fh = false;
};

struct CompletedFrame {
    FrameInfo info;
    uint32_t fseq32 = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> payload; // raw pixel data
};

struct IspConfig {
    uint16_t black_level = 0;
    float wb_r = 1.0f;
    float wb_g = 1.0f;
    float wb_b = 1.0f;
    float gamma = 2.2f;
};

struct StatsSnapshot {
    double fps = 0.0;
    double mbps = 0.0;
    double drop_rate = 0.0;
    uint32_t reorder_depth = 0;
    double latency_ms = 0.0;
    uint64_t frames_completed = 0;
    uint64_t frames_dropped = 0;
};

} // namespace rawabi
