#pragma once

#include "rawabi_common.h"

#include <random>
#include <string>
#include <vector>

namespace rawabi {

enum class Pattern {
    COLOR_BARS,
    RAMP,
    CHECKER,
    MOVING_BOX,
    SLANTED_EDGE,
};

struct GeneratorOptions {
    Pattern pattern = Pattern::COLOR_BARS;
    uint32_t seed = 0x12345678u;
    uint16_t box_size = 64;
    bool embed_counter = true;
};

class RawFrameGenerator {
  public:
    explicit RawFrameGenerator(FrameInfo info, GeneratorOptions opts = {});
    std::vector<uint16_t> next_frame(uint32_t frame_index);

    static std::vector<uint16_t> load_from_file(const std::string &path,
                                                const FrameInfo &info);

  private:
    FrameInfo info_;
    GeneratorOptions opts_;
    std::mt19937 rng_;

    uint16_t max_val() const;
    void fill_color_bars(std::vector<uint16_t> &buf, uint32_t frame_index);
    void fill_ramp(std::vector<uint16_t> &buf, uint32_t frame_index);
    void fill_checker(std::vector<uint16_t> &buf, uint32_t frame_index);
    void fill_slanted(std::vector<uint16_t> &buf, uint32_t frame_index);
    void fill_box(std::vector<uint16_t> &buf, uint32_t frame_index);
};

} // namespace rawabi
