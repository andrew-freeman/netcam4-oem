#include "rawabi_generator.h"

#include <fstream>
#include <algorithm>
#include <cmath>

namespace rawabi {

RawFrameGenerator::RawFrameGenerator(FrameInfo info, GeneratorOptions opts)
    : info_(info), opts_(opts), rng_(opts.seed) {}

uint16_t RawFrameGenerator::max_val() const {
    uint8_t bits = sample_bits(info_.fmt);
    if (bits == 0 || bits > 16) return 0;
    return static_cast<uint16_t>((1u << bits) - 1u);
}

std::vector<uint16_t> RawFrameGenerator::next_frame(uint32_t frame_index) {
    std::vector<uint16_t> buf(static_cast<std::size_t>(info_.width) * info_.height);
    switch (opts_.pattern) {
    case Pattern::COLOR_BARS:
        fill_color_bars(buf, frame_index);
        break;
    case Pattern::RAMP:
        fill_ramp(buf, frame_index);
        break;
    case Pattern::CHECKER:
        fill_checker(buf, frame_index);
        break;
    case Pattern::MOVING_BOX:
        fill_box(buf, frame_index);
        break;
    case Pattern::SLANTED_EDGE:
        fill_slanted(buf, frame_index);
        break;
    }

    if (opts_.embed_counter && !buf.empty()) {
        uint16_t counter = static_cast<uint16_t>(frame_index & max_val());
        std::size_t embed = std::min<std::size_t>(buf.size(), 64);
        for (std::size_t i = 0; i < embed; ++i) {
            buf[i] = counter;
        }
    }

    return buf;
}

std::vector<uint16_t> RawFrameGenerator::load_from_file(const std::string &path,
                                                        const FrameInfo &info) {
    std::ifstream fp(path, std::ios::binary);
    if (!fp) {
        return {};
    }
    std::vector<uint16_t> buf(static_cast<std::size_t>(info.width) * info.height);
    fp.read(reinterpret_cast<char *>(buf.data()), buf.size() * sizeof(uint16_t));
    if (!fp) {
        buf.clear();
    }
    return buf;
}

void RawFrameGenerator::fill_color_bars(std::vector<uint16_t> &buf, uint32_t /*frame_index*/) {
    const uint16_t mv = max_val();
    const uint16_t colors[6] = {
        mv,
        static_cast<uint16_t>(mv * 3 / 4),
        static_cast<uint16_t>(mv / 2),
        static_cast<uint16_t>(mv / 4),
        static_cast<uint16_t>(mv / 8),
        0};
    for (uint16_t y = 0; y < info_.height; ++y) {
        for (uint16_t x = 0; x < info_.width; ++x) {
            std::size_t idx = static_cast<std::size_t>(y) * info_.width + x;
            std::size_t bar = static_cast<std::size_t>(x) * 6 / info_.width;
            uint16_t val = colors[bar % 6];
            if (info_.pattern == BayerPattern::MONO) {
                buf[idx] = val;
                continue;
            }
            bool red_row = (info_.pattern == BayerPattern::BAYER_RG1BG2 ||
                            info_.pattern == BayerPattern::BAYER_G1RG2B) ? (y % 2 == 0) : (y % 2 != 0);
            bool red_col = (info_.pattern == BayerPattern::BAYER_RG1BG2 ||
                            info_.pattern == BayerPattern::BAYER_BG1RG2) ? (x % 2 == 0) : (x % 2 != 0);
            if (red_row && red_col) {
                buf[idx] = val;
            } else if (!red_row && !red_col) {
                buf[idx] = val / 2;
            } else {
                buf[idx] = val * 3 / 4;
            }
        }
    }
}

void RawFrameGenerator::fill_ramp(std::vector<uint16_t> &buf, uint32_t frame_index) {
    const uint16_t mv = max_val();
    for (uint16_t y = 0; y < info_.height; ++y) {
        for (uint16_t x = 0; x < info_.width; ++x) {
            std::size_t idx = static_cast<std::size_t>(y) * info_.width + x;
            uint32_t v = (static_cast<uint32_t>(x) + static_cast<uint32_t>(y) + frame_index) % mv;
            buf[idx] = static_cast<uint16_t>(v);
        }
    }
}

void RawFrameGenerator::fill_checker(std::vector<uint16_t> &buf, uint32_t frame_index) {
    const uint16_t mv = max_val();
    const uint16_t bright = mv;
    const uint16_t dark = mv / 16;
    for (uint16_t y = 0; y < info_.height; ++y) {
        for (uint16_t x = 0; x < info_.width; ++x) {
            bool block = (((x / 8) ^ (y / 8) ^ (frame_index / 8)) & 1u) != 0;
            buf[static_cast<std::size_t>(y) * info_.width + x] = block ? bright : dark;
        }
    }
}

void RawFrameGenerator::fill_slanted(std::vector<uint16_t> &buf, uint32_t frame_index) {
    const uint16_t mv = max_val();
    for (uint16_t y = 0; y < info_.height; ++y) {
        for (uint16_t x = 0; x < info_.width; ++x) {
            uint32_t v = (x + frame_index) % info_.width;
            uint32_t diag = (v + y / 2) % info_.width;
            uint16_t val = static_cast<uint16_t>((diag * mv) / (info_.width ? info_.width : 1));
            buf[static_cast<std::size_t>(y) * info_.width + x] = val;
        }
    }
}

void RawFrameGenerator::fill_box(std::vector<uint16_t> &buf, uint32_t frame_index) {
    std::fill(buf.begin(), buf.end(), static_cast<uint16_t>(max_val() / 8));
    if (info_.width == 0 || info_.height == 0) {
        return;
    }
    uint16_t mv = max_val();
    uint16_t size = std::min<uint16_t>(opts_.box_size, std::min(info_.width, info_.height));
    uint16_t x_center = static_cast<uint16_t>((frame_index * 5) % info_.width);
    uint16_t y_center = static_cast<uint16_t>((frame_index * 3) % info_.height);
    uint16_t x0 = (x_center + info_.width - size / 2) % info_.width;
    uint16_t y0 = (y_center + info_.height - size / 2) % info_.height;
    for (uint16_t dy = 0; dy < size; ++dy) {
        uint16_t y = (y0 + dy) % info_.height;
        for (uint16_t dx = 0; dx < size; ++dx) {
            uint16_t x = (x0 + dx) % info_.width;
            buf[static_cast<std::size_t>(y) * info_.width + x] = mv;
        }
    }
}

} // namespace rawabi
