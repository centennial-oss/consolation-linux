#pragma once

#include <cstddef>
#include <cstdint>

namespace consolation::platform::linux::mjpeg_jpeg_parse {

struct BaselineFrameInfo {
    int width = 0;
    int height = 0;
    bool valid = false;
};

// Fills width/height from SOF0 when present; does not validate the full bitstream.
[[nodiscard]] BaselineFrameInfo parseBaselineDimensions(const uint8_t *data, size_t size);

} // namespace consolation::platform::linux::mjpeg_jpeg_parse
