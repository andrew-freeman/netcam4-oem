#include "rawabi_preview.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

#ifdef RAWABI_HAVE_OPENCV
#include <opencv2/highgui.hpp>
#endif

namespace rawabi {

namespace {
struct Lut {
    std::vector<uint8_t> values;
};

Lut build_gamma_lut(float gamma) {
    Lut lut;
    lut.values.resize(256);
    if (gamma <= 0.01f) gamma = 1.0f;
    for (int i = 0; i < 256; ++i) {
        float norm = static_cast<float>(i) / 255.0f;
        float corrected = std::pow(norm, 1.0f / gamma);
        lut.values[i] = static_cast<uint8_t>(std::clamp(corrected * 255.0f, 0.0f, 255.0f));
    }
    return lut;
}

inline uint16_t read_pixel(const CompletedFrame &f, std::size_t x, std::size_t y) {
    std::size_t idx = y * f.info.width + x;
    if (idx * 2 + 1 >= f.payload.size()) return 0;
    uint16_t v;
    std::memcpy(&v, &f.payload[idx * 2], sizeof(uint16_t));
    return v;
}

inline uint16_t read_clamped(const CompletedFrame &f, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= f.info.width) x = f.info.width - 1;
    if (y >= f.info.height) y = f.info.height - 1;
    return read_pixel(f, static_cast<std::size_t>(x), static_cast<std::size_t>(y));
}

uint16_t apply_black(uint16_t v, uint16_t black) {
    return (v > black) ? static_cast<uint16_t>(v - black) : 0;
}

uint8_t normalize(uint16_t v, uint8_t bits, uint16_t black, const Lut &lut) {
    uint16_t mv = (bits >= 16) ? 0xFFFFu : static_cast<uint16_t>((1u << bits) - 1u);
    uint16_t clipped = (v > mv) ? mv : v;
    uint16_t shifted = apply_black(clipped, black);
    float norm = (mv > black) ? static_cast<float>(shifted) / static_cast<float>(mv - black) : 0.0f;
    uint16_t base = static_cast<uint16_t>(std::clamp(norm * 255.0f, 0.0f, 255.0f));
    return lut.values[base];
}

inline void assign_pixel(std::vector<uint8_t> &bgr, std::size_t idx, uint8_t b, uint8_t g, uint8_t r) {
    bgr[idx * 3 + 0] = b;
    bgr[idx * 3 + 1] = g;
    bgr[idx * 3 + 2] = r;
}

void bilinear(const CompletedFrame &f, const IspConfig &cfg, const Lut &lut, PreviewFrame &out) {
    out.width = f.info.width;
    out.height = f.info.height;
    out.bgr.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    const uint8_t bits = sample_bits(f.info.fmt);
    for (uint16_t y = 0; y < f.info.height; ++y) {
        for (uint16_t x = 0; x < f.info.width; ++x) {
            bool top = (y % 2 == 0);
            bool left = (x % 2 == 0);
            uint8_t r = 0, g = 0, b = 0;
            uint16_t v = read_pixel(f, x, y);
            uint8_t base = normalize(v, bits, cfg.black_level, lut);
            BayerPattern p = f.info.pattern;
            if (p == BayerPattern::MONO) {
                assign_pixel(out.bgr, y * out.width + x, base, base, base);
                continue;
            }
            // Determine color plane
            bool red_pos = false;
            bool blue_pos = false;
            switch (p) {
            case BayerPattern::BAYER_RG1BG2:
                red_pos = top && left;
                blue_pos = (!top) && (!left);
                break;
            case BayerPattern::BAYER_BG1RG2:
                blue_pos = top && left;
                red_pos = (!top) && (!left);
                break;
            case BayerPattern::BAYER_G1RG2B:
                red_pos = top && (!left);
                blue_pos = (!top) && left;
                break;
            case BayerPattern::BAYER_G1BG2R:
                blue_pos = top && (!left);
                red_pos = (!top) && left;
                break;
            case BayerPattern::MONO:
                break;
            }
            if (red_pos) {
                r = static_cast<uint8_t>(std::clamp(base * cfg.wb_r, 0.0f, 255.0f));
                // Interpolate green and blue from neighbors
                uint16_t g1 = read_clamped(f, static_cast<int>(x) + 1, static_cast<int>(y));
                uint16_t g2 = read_clamped(f, static_cast<int>(x), static_cast<int>(y) + 1);
                uint16_t b1 = read_clamped(f, static_cast<int>(x) + 1, static_cast<int>(y) + 1);
                g = normalize((g1 + g2) / 2, bits, cfg.black_level, lut);
                b = normalize(b1, bits, cfg.black_level, lut);
            } else if (blue_pos) {
                b = static_cast<uint8_t>(std::clamp(base * cfg.wb_b, 0.0f, 255.0f));
                uint16_t g1 = read_clamped(f, static_cast<int>(x) + 1, static_cast<int>(y));
                uint16_t g2 = read_clamped(f, static_cast<int>(x), static_cast<int>(y) + 1);
                uint16_t r1 = read_clamped(f, static_cast<int>(x) + 1, static_cast<int>(y) + 1);
                g = normalize((g1 + g2) / 2, bits, cfg.black_level, lut);
                r = normalize(r1, bits, cfg.black_level, lut);
            } else {
                // Green position
                g = static_cast<uint8_t>(std::clamp(base * cfg.wb_g, 0.0f, 255.0f));
                uint16_t horiz = read_clamped(f, static_cast<int>(x) + (left ? 1 : -1), static_cast<int>(y));
                uint16_t vert = read_clamped(f, static_cast<int>(x), static_cast<int>(y) + (top ? 1 : -1));
                if ((y % 2) == (x % 2)) {
                    r = normalize(horiz, bits, cfg.black_level, lut);
                    b = normalize(vert, bits, cfg.black_level, lut);
                } else {
                    b = normalize(horiz, bits, cfg.black_level, lut);
                    r = normalize(vert, bits, cfg.black_level, lut);
                }
            }
            assign_pixel(out.bgr, y * out.width + x, b, g, r);
        }
    }
}

void half_res(const CompletedFrame &f, const IspConfig &cfg, const Lut &lut, PreviewFrame &out) {
    out.width = f.info.width / 2;
    out.height = f.info.height / 2;
    out.bgr.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    const uint8_t bits = sample_bits(f.info.fmt);
    for (uint16_t y = 0; y + 1 < f.info.height; y += 2) {
        for (uint16_t x = 0; x + 1 < f.info.width; x += 2) {
            uint16_t r = read_pixel(f, x + 1, y);
            uint16_t g1 = read_pixel(f, x, y);
            uint16_t g2 = read_pixel(f, x + 1, y + 1);
            uint16_t b = read_pixel(f, x, y + 1);
            std::size_t idx = (y / 2) * out.width + (x / 2);
            assign_pixel(out.bgr, idx,
                         normalize(b, bits, cfg.black_level, lut),
                         normalize((g1 + g2) / 2, bits, cfg.black_level, lut),
                         normalize(r, bits, cfg.black_level, lut));
        }
    }
}

void mono_view(const CompletedFrame &f, const IspConfig &cfg, const Lut &lut, PreviewFrame &out, bool green_only) {
    out.width = f.info.width;
    out.height = f.info.height;
    out.bgr.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    uint8_t bits = sample_bits(f.info.fmt);
    for (uint16_t y = 0; y < f.info.height; ++y) {
        for (uint16_t x = 0; x < f.info.width; ++x) {
            uint16_t raw = read_pixel(f, x, y);
            if (green_only && (f.info.pattern != BayerPattern::MONO)) {
                bool is_green = ((x ^ y) & 1u) == 1u;
                if (!is_green) raw = raw / 4;
            }
            uint8_t v = normalize(raw, bits, cfg.black_level, lut);
            assign_pixel(out.bgr, y * out.width + x, v, v, v);
        }
    }
}
}

PreviewFrame render_preview(const CompletedFrame &frame,
                             const IspConfig &cfg,
                             ViewMode mode) {
    PreviewFrame out;
    Lut lut = build_gamma_lut(cfg.gamma);
    switch (mode) {
    case ViewMode::MONO:
        mono_view(frame, cfg, lut, out, false);
        break;
    case ViewMode::GREEN:
        mono_view(frame, cfg, lut, out, true);
        break;
    case ViewMode::HALF_RES:
        half_res(frame, cfg, lut, out);
        break;
    case ViewMode::BILINEAR:
        bilinear(frame, cfg, lut, out);
        break;
    }
    return out;
}

#ifdef RAWABI_HAVE_OPENCV
void display_frame(const PreviewFrame &frame,
                   const std::string &window,
                   const StatsSnapshot &stats,
                   bool blocking) {
    if (frame.bgr.empty()) return;
    cv::Mat img(frame.height, frame.width, CV_8UC3, const_cast<uint8_t *>(frame.bgr.data()));
    cv::Mat shown;
    img.copyTo(shown);
    char overlay[256];
    snprintf(overlay, sizeof(overlay), "fps %.1f | Mbps %.1f | dropped %llu | reorder %u",
             stats.fps, stats.mbps, static_cast<unsigned long long>(stats.frames_dropped), stats.reorder_depth);
    cv::putText(shown, overlay, {10, 20}, cv::FONT_HERSHEY_SIMPLEX, 0.5, {0, 255, 255}, 1);
    cv::imshow(window, shown);
    cv::waitKey(blocking ? 0 : 1);
}
#endif

} // namespace rawabi
