#include "platform/linux/MjpegDecoderJpegParse.h"

namespace consolation::platform::linux::mjpeg_jpeg_parse {

namespace {

constexpr uint8_t kMarkerPrefix = 0xFF;

} // namespace

BaselineFrameInfo parseBaselineDimensions(const uint8_t *data, const size_t size)
{
    BaselineFrameInfo info;
    if (data == nullptr || size < 4) {
        return info;
    }

    size_t offset = 0;
    if (data[0] == kMarkerPrefix && data[1] == 0xD8) {
        offset = 2;
    }

    while (offset + 3 < size) {
        if (data[offset] != kMarkerPrefix) {
            ++offset;
            continue;
        }

        const auto marker = data[offset + 1];
        offset += 2;

        if (marker == 0xD8 || marker == 0xD9) {
            continue;
        }

        if (marker == 0xDA) {
            break;
        }

        if (offset + 1 >= size) {
            break;
        }

        const auto segmentLength = static_cast<size_t>((static_cast<unsigned>(data[offset]) << 8U) | data[offset + 1]);
        if (segmentLength < 2 || offset + segmentLength > size) {
            break;
        }

        if (marker == 0xC0 || marker == 0xC1) {
            if (segmentLength >= 7) {
                info.height = static_cast<int>((static_cast<unsigned>(data[offset + 3]) << 8U) | data[offset + 4]);
                info.width = static_cast<int>((static_cast<unsigned>(data[offset + 5]) << 8U) | data[offset + 6]);
                info.valid = info.width > 0 && info.height > 0;
            }
            return info;
        }

        offset += segmentLength;
    }

    return info;
}

} // namespace consolation::platform::linux::mjpeg_jpeg_parse
