#pragma once

#include "rawabi_common.h"

#include <vector>

namespace rawabi {

enum class ViewMode {
    MONO,
    GREEN,
    HALF_RES,
    BILINEAR,
};

struct PreviewFrame {
    std::vector<uint8_t> bgr;
    uint16_t width = 0;
    uint16_t height = 0;
};

PreviewFrame render_preview(const CompletedFrame &frame,
                             const IspConfig &cfg,
                             ViewMode mode);

#ifdef RAWABI_HAVE_OPENCV
void display_frame(const PreviewFrame &frame,
                   const std::string &window,
                   const StatsSnapshot &stats,
                   bool blocking = false);
#endif

} // namespace rawabi
