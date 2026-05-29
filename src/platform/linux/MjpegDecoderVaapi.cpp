#include "platform/linux/MjpegDecoderBackends.h"
#include "platform/linux/MjpegPerfLog.h"

#include "capture/MonotonicClock.h"

#include <libyuv/convert_argb.h>
#include <libyuv/scale_argb.h>

#include <QByteArray>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#ifdef CONSOLATION_HAVE_LIBVA
#include <va/va.h>
#include <va/va_dec_jpeg.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <xf86drm.h>

#endif

namespace consolation::platform::linux::mjpeg_backend {

#ifdef CONSOLATION_HAVE_LIBVA

namespace {

// Re-enable VA-API MJPEG optimizations one at a time when hunting regressions.
constexpr bool kOptPersistentExportImage = true;
constexpr bool kOptReuseVaTableBuffers = true;
constexpr bool kOptFastSosHeaderParse = false;
constexpr bool kOptSkipVaSyncBeforeExport = true;

constexpr uint8_t kMarkerPrefix = 0xFF;

struct VaapiJpegTables {
    VAPictureParameterBufferJPEGBaseline picture {};
    VAIQMatrixBufferJPEGBaseline iq {};
    VAHuffmanTableBufferJPEGBaseline huffman {};
    VASliceParameterBufferJPEGBaseline slice {};
    bool pictureValid = false;
    bool iqValid = false;
    bool huffmanValid = false;
    bool sliceValid = false;
};

template <size_t N>
void copyBytes(uint8_t (&dest)[N], const std::array<uint8_t, N> &src)
{
    std::memcpy(dest, src.data(), N);
}

VAHuffmanTableBufferJPEGBaseline defaultJpegHuffmanTables()
{
    static constexpr std::array<uint8_t, 16> kLumaDcCodes = {
        0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr std::array<uint8_t, 12> kLumaDcValues = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
    static constexpr std::array<uint8_t, 16> kLumaAcCodes = {
        0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d};
    static constexpr std::array<uint8_t, 162> kLumaAcValues = {
        0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
        0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82,
        0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
        0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2,
        0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
        0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
        0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
    static constexpr std::array<uint8_t, 16> kChromaDcCodes = {
        0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr std::array<uint8_t, 12> kChromaDcValues = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
    static constexpr std::array<uint8_t, 16> kChromaAcCodes = {
        0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77};
    static constexpr std::array<uint8_t, 162> kChromaAcValues = {
        0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
        0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24,
        0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
        0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
        0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94,
        0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
        0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
        0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

    VAHuffmanTableBufferJPEGBaseline huffman {};
    huffman.load_huffman_table[0] = 1;
    huffman.load_huffman_table[1] = 1;
    copyBytes(huffman.huffman_table[0].num_dc_codes, kLumaDcCodes);
    copyBytes(huffman.huffman_table[0].dc_values, kLumaDcValues);
    copyBytes(huffman.huffman_table[0].num_ac_codes, kLumaAcCodes);
    copyBytes(huffman.huffman_table[0].ac_values, kLumaAcValues);
    copyBytes(huffman.huffman_table[1].num_dc_codes, kChromaDcCodes);
    copyBytes(huffman.huffman_table[1].dc_values, kChromaDcValues);
    copyBytes(huffman.huffman_table[1].num_ac_codes, kChromaAcCodes);
    copyBytes(huffman.huffman_table[1].ac_values, kChromaAcValues);
    return huffman;
}

bool parseDhtSegment(const uint8_t *segment, const size_t payload, VAHuffmanTableBufferJPEGBaseline &huffman)
{
    size_t offset = 0;
    while (offset + 1 < payload) {
        const auto tableClass = static_cast<uint8_t>(segment[offset] >> 4);
        const auto tableId = static_cast<uint8_t>(segment[offset] & 0x0F);
        ++offset;
        if (tableId > 1 || tableClass > 1) {
            return false;
        }

        huffman.load_huffman_table[tableId] = 1;
        auto &entry = huffman.huffman_table[tableId];

        if (offset + 16 > payload) {
            return false;
        }

        std::array<uint8_t, 16> lengths {};
        std::memcpy(lengths.data(), segment + offset, 16);
        offset += 16;

        size_t valueCount = 0;
        for (const auto length : lengths) {
            valueCount += length;
        }
        if (offset + valueCount > payload) {
            return false;
        }

        if (tableClass == 0) {
            std::memcpy(entry.num_dc_codes, lengths.data(), 16);
            const auto copyCount = std::min(valueCount, static_cast<size_t>(12));
            std::memcpy(entry.dc_values, segment + offset, copyCount);
        } else {
            std::memcpy(entry.num_ac_codes, lengths.data(), 16);
            const auto copyCount = std::min(valueCount, static_cast<size_t>(162));
            std::memcpy(entry.ac_values, segment + offset, copyCount);
        }
        offset += valueCount;
    }

    return huffman.load_huffman_table[0] != 0 || huffman.load_huffman_table[1] != 0;
}

bool parseVaapiTables(const uint8_t *data, const size_t size, VaapiJpegTables &tables)
{
    if (data == nullptr || size < 4) {
        return false;
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

        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01) {
            continue;
        }

        if (marker == 0xDA) {
            if (offset + 1 >= size) {
                break;
            }
            const auto segmentLength = static_cast<size_t>((static_cast<unsigned>(data[offset]) << 8U) | data[offset + 1]);
            if (segmentLength < 2 || offset + segmentLength > size) {
                break;
            }

            const auto *segment = data + offset + 2;
            const auto payload = segmentLength - 2;
            if (payload < 1) {
                break;
            }

            tables.slice.num_components = segment[0];
            if (tables.slice.num_components > 4 || payload < 1U + tables.slice.num_components * 2U) {
                break;
            }

            for (auto index = 0U; index < tables.slice.num_components; ++index) {
                const auto base = 1U + index * 2U;
                tables.slice.components[index].component_selector = segment[base];
                const auto tableSpec = segment[base + 1];
                tables.slice.components[index].dc_table_selector = static_cast<uint8_t>(tableSpec >> 4);
                tables.slice.components[index].ac_table_selector = static_cast<uint8_t>(tableSpec & 0x0F);
            }

            const auto scanOffset = offset + segmentLength;
            if (scanOffset >= size) {
                break;
            }

            tables.slice.slice_data_offset = static_cast<uint32_t>(scanOffset);
            tables.slice.slice_data_size = static_cast<uint32_t>(size - scanOffset);
            tables.slice.slice_data_flag = 0;
            tables.sliceValid = true;

            if (!tables.huffmanValid) {
                tables.huffman = defaultJpegHuffmanTables();
                tables.huffmanValid = true;
            }

            return tables.pictureValid && tables.iqValid && tables.huffmanValid && tables.sliceValid;
        }

        if (offset + 1 >= size) {
            break;
        }

        const auto segmentLength = static_cast<size_t>((static_cast<unsigned>(data[offset]) << 8U) | data[offset + 1]);
        if (segmentLength < 2 || offset + segmentLength > size) {
            break;
        }

        const auto *segment = data + offset + 2;
        const auto payload = segmentLength - 2;

        if (marker == 0xC0 && payload >= 6) {
            tables.picture.picture_height =
                static_cast<uint16_t>((static_cast<unsigned>(segment[1]) << 8U) | segment[2]);
            tables.picture.picture_width =
                static_cast<uint16_t>((static_cast<unsigned>(segment[3]) << 8U) | segment[4]);
            tables.picture.num_components = segment[5];
            if (tables.picture.num_components == 0 || tables.picture.num_components > 4 ||
                payload < 6U + tables.picture.num_components * 3U) {
                break;
            }

            for (auto index = 0U; index < tables.picture.num_components; ++index) {
                const auto base = 6U + index * 3U;
                tables.picture.components[index].component_id = segment[base];
                tables.picture.components[index].h_sampling_factor =
                    static_cast<uint8_t>(segment[base + 1] >> 4);
                tables.picture.components[index].v_sampling_factor =
                    static_cast<uint8_t>(segment[base + 1] & 0x0F);
                tables.picture.components[index].quantiser_table_selector = segment[base + 2];
            }
            tables.pictureValid = tables.picture.picture_width > 0 && tables.picture.picture_height > 0;
        } else if (marker == 0xDB && payload >= 1) {
            size_t tableOffset = 0;
            while (tableOffset + 1 < payload) {
                const auto tableInfo = segment[tableOffset++];
                const auto tableId = tableInfo & 0x0F;
                if (tableId > 3) {
                    break;
                }
                tables.iq.load_quantiser_table[tableId] = 1;
                if (tableOffset + 64 > payload) {
                    break;
                }
                std::memcpy(tables.iq.quantiser_table[tableId], segment + tableOffset, 64);
                tableOffset += 64;
            }
            tables.iqValid = std::any_of(
                std::begin(tables.iq.load_quantiser_table),
                std::end(tables.iq.load_quantiser_table),
                [](const uint8_t loaded) { return loaded != 0; });
        } else if (marker == 0xC4 && payload >= 1) {
            if (parseDhtSegment(segment, payload, tables.huffman)) {
                tables.huffmanValid = true;
            }
        }

        offset += segmentLength;
    }

    if (!tables.huffmanValid) {
        tables.huffman = defaultJpegHuffmanTables();
        tables.huffmanValid = true;
    }

    return tables.pictureValid && tables.iqValid && tables.huffmanValid && tables.sliceValid;
}

int openVaapiRenderNode()
{
    for (int index = 128; index < 144; ++index) {
        const QByteArray path = QByteArrayLiteral("/dev/dri/renderD") + QByteArray::number(index);
        const auto fd = ::open(path.constData(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            return fd;
        }
    }
    return -1;
}

[[nodiscard]] bool drmDriverHasNoVaapi(const int fd)
{
    drmVersionPtr version = drmGetVersion(fd);
    if (version == nullptr) {
        return false; // unknown — let libva try
    }
    const QByteArray name = QByteArray(version->name, version->name_len > 0 ? static_cast<int>(version->name_len) : 0)
                                .toLower();
    drmFreeVersion(version);
    return name.contains("v3d") || name.contains("vc4");
}

[[nodiscard]] double elapsedMs(const qint64 startNs)
{
    return static_cast<double>(consolation::capture::monotonicClockNs() - startNs) / 1'000'000.0;
}

// Sub-timings for the VA-API render path (item 3 of MJPEG_OPTIMIZATION.md). Populated only when a
// non-null pointer is threaded through; left untouched (and the clock untouched) otherwise.
struct RenderTimings {
    double sliceMs = 0.0;
    double submitMs = 0.0;
    double syncMs = 0.0;
};

} // namespace

struct VaapiDecoder::Impl {
    static constexpr int kSurfacePoolSize = 8;

    int drmFd = -1;
    VADisplay display = nullptr;
    bool vaInitialized = false;
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    std::vector<VASurfaceID> surfaces_;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    bool dmaExportSupported_ = false;
    std::atomic<uint64_t> slotInUseMask_ { 0 };
    std::vector<uint8_t> argbScratch;

    VAImage exportImage_ {};
    int exportImageWidth_ = 0;
    int exportImageHeight_ = 0;

    struct ExportedSlot {
        bool valid = false;
        std::array<int, 2> planeFds { -1, -1 };
        std::array<int, 2> planeOffsets { 0, 0 };
        std::array<int, 2> planeStrides { 0, 0 };
        std::array<uint32_t, 2> planeFourccs { 0, 0 };
        std::array<uint64_t, 2> planeModifiers { ~uint64_t { 0 }, ~uint64_t { 0 } };
        int stride = 0;
    };
    std::array<ExportedSlot, kSurfacePoolSize> exportedSlots_ {};

    ~Impl()
    {
        release();
    }

    VABufferID pictureBuffer_ = VA_INVALID_ID;
    VABufferID iqBuffer_ = VA_INVALID_ID;
    VABufferID huffmanBuffer_ = VA_INVALID_ID;
    VABufferID sliceParamBuffer_ = VA_INVALID_ID;

    void releaseVaTableBuffers()
    {
        if (display == nullptr) {
            return;
        }

        const VABufferID buffers[] = {sliceParamBuffer_, huffmanBuffer_, iqBuffer_, pictureBuffer_};
        for (const auto buffer : buffers) {
            if (buffer != VA_INVALID_ID) {
                vaDestroyBuffer(display, buffer);
            }
        }

        pictureBuffer_ = VA_INVALID_ID;
        iqBuffer_ = VA_INVALID_ID;
        huffmanBuffer_ = VA_INVALID_ID;
        sliceParamBuffer_ = VA_INVALID_ID;
    }

    [[nodiscard]] VASurfaceID surfaceAt(const int index) const
    {
        if (index < 0 || index >= static_cast<int>(surfaces_.size())) {
            return VA_INVALID_ID;
        }
        return surfaces_[static_cast<size_t>(index)];
    }

    void releaseExportImage()
    {
        if (display != nullptr && exportImage_.image_id != VA_INVALID_ID) {
            vaDestroyImage(display, exportImage_.image_id);
        }
        exportImage_ = {};
        exportImageWidth_ = 0;
        exportImageHeight_ = 0;
    }

    // Close all cached per-slot export FDs. Call whenever the underlying surfaces go away so we never
    // hand out an FD that backs a destroyed surface.
    void releaseExportedSlots()
    {
        for (auto &slot : exportedSlots_) {
            for (const auto fd : slot.planeFds) {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
        }
        exportedSlots_ = {};
    }

    void release()
    {
        releaseExportImage();
        releaseExportedSlots();
        releaseVaTableBuffers();

        slotInUseMask_.store(0, std::memory_order_release);

        if (display != nullptr && vaInitialized) {
            if (!surfaces_.empty()) {
                vaDestroySurfaces(display, surfaces_.data(), static_cast<int>(surfaces_.size()));
                surfaces_.clear();
            }
            if (context != VA_INVALID_ID) {
                vaDestroyContext(display, context);
                context = VA_INVALID_ID;
            }
            if (config != VA_INVALID_ID) {
                vaDestroyConfig(display, config);
                config = VA_INVALID_ID;
            }
            vaTerminate(display);
        }
        display = nullptr;
        vaInitialized = false;
        surfaces_.clear();
        config = VA_INVALID_ID;
        context = VA_INVALID_ID;
        if (drmFd >= 0) {
            ::close(drmFd);
            drmFd = -1;
        }
        surfaceWidth = 0;
        surfaceHeight = 0;
    }

    [[nodiscard]] bool ensureContext(const int width, const int height)
    {
        if (display == nullptr || config == VA_INVALID_ID) {
            return false;
        }
        if (context != VA_INVALID_ID && width <= surfaceWidth && height <= surfaceHeight) {
            return true;
        }

        releaseExportImage();
        releaseExportedSlots();
        releaseVaTableBuffers();
        slotInUseMask_.store(0, std::memory_order_release);

        if (!surfaces_.empty()) {
            vaDestroySurfaces(display, surfaces_.data(), static_cast<int>(surfaces_.size()));
            surfaces_.clear();
        }
        if (context != VA_INVALID_ID) {
            vaDestroyContext(display, context);
            context = VA_INVALID_ID;
        }

        surfaceWidth = width;
        surfaceHeight = height;
        surfaces_.assign(static_cast<size_t>(kSurfacePoolSize), VA_INVALID_ID);
        VASurfaceAttrib usageHint {};
        usageHint.type = VASurfaceAttribUsageHint;
        usageHint.flags = VA_SURFACE_ATTRIB_SETTABLE;
        usageHint.value.type = VAGenericValueTypeInteger;
        usageHint.value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_DECODER | VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT;

        auto createStatus = vaCreateSurfaces(
                display,
                VA_RT_FORMAT_YUV420,
                static_cast<unsigned int>(surfaceWidth),
                static_cast<unsigned int>(surfaceHeight),
                surfaces_.data(),
                kSurfacePoolSize,
                &usageHint,
                1);
        if (createStatus != VA_STATUS_SUCCESS) {
            createStatus = vaCreateSurfaces(
                display,
                VA_RT_FORMAT_YUV420,
                static_cast<unsigned int>(surfaceWidth),
                static_cast<unsigned int>(surfaceHeight),
                surfaces_.data(),
                kSurfacePoolSize,
                nullptr,
                0);
        }
        if (createStatus != VA_STATUS_SUCCESS) {
            surfaces_.clear();
            return false;
        }

        return vaCreateContext(
                   display,
                   config,
                   surfaceWidth,
                   surfaceHeight,
                   VA_PROGRESSIVE,
                   surfaces_.data(),
                   kSurfacePoolSize,
                   &context) == VA_STATUS_SUCCESS;
    }

    [[nodiscard]] int acquireSlot()
    {
        const auto allInUse = (uint64_t{1} << kSurfacePoolSize) - 1;
        while (true) {
            auto mask = slotInUseMask_.load(std::memory_order_acquire);
            if ((mask & allInUse) == allInUse) {
                return -1;
            }

            for (int index = 0; index < kSurfacePoolSize; ++index) {
                const auto bit = uint64_t{1} << index;
                if ((mask & bit) != 0) {
                    continue;
                }
                const auto desired = mask | bit;
                if (slotInUseMask_.compare_exchange_weak(mask, desired, std::memory_order_acq_rel)) {
                    return index;
                }
                break;
            }
        }
    }

    [[nodiscard]] bool hasAvailableSlot() const
    {
        const auto allInUse = (uint64_t{1} << kSurfacePoolSize) - 1;
        return (slotInUseMask_.load(std::memory_order_acquire) & allInUse) != allInUse;
    }

    void releaseSlot(const int slotIndex)
    {
        if (slotIndex < 0 || slotIndex >= kSurfacePoolSize) {
            return;
        }
        slotInUseMask_.fetch_and(~(uint64_t{1} << slotIndex), std::memory_order_release);
    }

    [[nodiscard]] bool ensureVaTableBuffers(const VaapiJpegTables &tables)
    {
        if (pictureBuffer_ != VA_INVALID_ID) {
            return true;
        }

        if (vaCreateBuffer(
                display,
                context,
                VAPictureParameterBufferType,
                sizeof(tables.picture),
                1,
                const_cast<VAPictureParameterBufferJPEGBaseline *>(&tables.picture),
                &pictureBuffer_) != VA_STATUS_SUCCESS ||
            vaCreateBuffer(
                display,
                context,
                VAIQMatrixBufferType,
                sizeof(tables.iq),
                1,
                const_cast<VAIQMatrixBufferJPEGBaseline *>(&tables.iq),
                &iqBuffer_) != VA_STATUS_SUCCESS ||
            vaCreateBuffer(
                display,
                context,
                VAHuffmanTableBufferType,
                sizeof(tables.huffman),
                1,
                const_cast<VAHuffmanTableBufferJPEGBaseline *>(&tables.huffman),
                &huffmanBuffer_) != VA_STATUS_SUCCESS ||
            vaCreateBuffer(
                display,
                context,
                VASliceParameterBufferType,
                sizeof(tables.slice),
                1,
                const_cast<VASliceParameterBufferJPEGBaseline *>(&tables.slice),
                &sliceParamBuffer_) != VA_STATUS_SUCCESS) {
            releaseVaTableBuffers();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool uploadTableBuffers(const VaapiJpegTables &tables)
    {
        void *mapped = nullptr;
        if (vaMapBuffer(display, pictureBuffer_, &mapped) != VA_STATUS_SUCCESS) {
            return false;
        }
        std::memcpy(mapped, &tables.picture, sizeof(tables.picture));
        vaUnmapBuffer(display, pictureBuffer_);

        if (vaMapBuffer(display, iqBuffer_, &mapped) != VA_STATUS_SUCCESS) {
            return false;
        }
        std::memcpy(mapped, &tables.iq, sizeof(tables.iq));
        vaUnmapBuffer(display, iqBuffer_);

        if (vaMapBuffer(display, huffmanBuffer_, &mapped) != VA_STATUS_SUCCESS) {
            return false;
        }
        std::memcpy(mapped, &tables.huffman, sizeof(tables.huffman));
        vaUnmapBuffer(display, huffmanBuffer_);

        if (vaMapBuffer(display, sliceParamBuffer_, &mapped) != VA_STATUS_SUCCESS) {
            return false;
        }
        std::memcpy(mapped, &tables.slice, sizeof(tables.slice));
        vaUnmapBuffer(display, sliceParamBuffer_);

        return true;
    }

    [[nodiscard]] bool createSliceDataBuffer(
        const uint8_t *data,
        const int bytesUsed,
        VABufferID &sliceDataBuffer) const
    {
        sliceDataBuffer = VA_INVALID_ID;
        return vaCreateBuffer(
                   display,
                   context,
                   VASliceDataBufferType,
                   static_cast<unsigned int>(bytesUsed),
                   1,
                   const_cast<uint8_t *>(data),
                   &sliceDataBuffer) == VA_STATUS_SUCCESS;
    }

    [[nodiscard]] bool renderWithBuffers(
        const VASurfaceID targetSurface,
        VABufferID pictureBuffer,
        VABufferID iqBuffer,
        VABufferID huffmanBuffer,
        VABufferID sliceParamBuffer,
        VABufferID sliceDataBuffer,
        const bool destroyTableBuffersAfterRender,
        RenderTimings *timings = nullptr)
    {
        VABufferID buffers[] = {
            pictureBuffer,
            iqBuffer,
            huffmanBuffer,
            sliceParamBuffer,
            sliceDataBuffer,
        };
        const auto submitStartNs = timings ? consolation::capture::monotonicClockNs() : 0;
        if (vaBeginPicture(display, context, targetSurface) != VA_STATUS_SUCCESS) {
            if (destroyTableBuffersAfterRender) {
                vaDestroyBuffer(display, sliceDataBuffer);
                vaDestroyBuffer(display, sliceParamBuffer);
                vaDestroyBuffer(display, huffmanBuffer);
                vaDestroyBuffer(display, iqBuffer);
                vaDestroyBuffer(display, pictureBuffer);
            } else {
                vaDestroyBuffer(display, sliceDataBuffer);
            }
            return false;
        }

        const auto renderStatus =
            vaRenderPicture(display, context, buffers, static_cast<int>(std::size(buffers)));
        const auto endStatus = vaEndPicture(display, context);
        if (timings != nullptr) {
            timings->submitMs = elapsedMs(submitStartNs);
        }

        if (destroyTableBuffersAfterRender) {
            vaDestroyBuffer(display, sliceDataBuffer);
            vaDestroyBuffer(display, sliceParamBuffer);
            vaDestroyBuffer(display, huffmanBuffer);
            vaDestroyBuffer(display, iqBuffer);
            vaDestroyBuffer(display, pictureBuffer);
        } else {
            vaDestroyBuffer(display, sliceDataBuffer);
        }

        bool syncOk = true;
        if (!kOptSkipVaSyncBeforeExport) {
            const auto syncStartNs = timings ? consolation::capture::monotonicClockNs() : 0;
            syncOk = vaSyncSurface(display, targetSurface) == VA_STATUS_SUCCESS;
            if (timings != nullptr) {
                timings->syncMs = elapsedMs(syncStartNs);
            }
        }

        return renderStatus == VA_STATUS_SUCCESS && endStatus == VA_STATUS_SUCCESS && syncOk;
    }

    [[nodiscard]] bool renderJpegToSurface(
        const VASurfaceID targetSurface,
        const uint8_t *data,
        const int bytesUsed,
        const VaapiJpegTables &tables,
        RenderTimings *timings = nullptr)
    {
        if (targetSurface == VA_INVALID_ID) {
            return false;
        }

        if (kOptReuseVaTableBuffers) {
            if (!ensureVaTableBuffers(tables) || !uploadTableBuffers(tables)) {
                releaseVaTableBuffers();
                return false;
            }

            VABufferID sliceDataBuffer = VA_INVALID_ID;
            const auto sliceStartNs = timings ? consolation::capture::monotonicClockNs() : 0;
            if (!createSliceDataBuffer(data, bytesUsed, sliceDataBuffer)) {
                return false;
            }
            if (timings != nullptr) {
                timings->sliceMs = elapsedMs(sliceStartNs);
            }

            return renderWithBuffers(
                targetSurface,
                pictureBuffer_,
                iqBuffer_,
                huffmanBuffer_,
                sliceParamBuffer_,
                sliceDataBuffer,
                false,
                timings);
        }

        VABufferID pictureBuffer = VA_INVALID_ID;
        VABufferID iqBuffer = VA_INVALID_ID;
        VABufferID huffmanBuffer = VA_INVALID_ID;
        VABufferID sliceParamBuffer = VA_INVALID_ID;
        VABufferID sliceDataBuffer = VA_INVALID_ID;

        if (vaCreateBuffer(
                display,
                context,
                VAPictureParameterBufferType,
                sizeof(tables.picture),
                1,
                const_cast<VAPictureParameterBufferJPEGBaseline *>(&tables.picture),
                &pictureBuffer) != VA_STATUS_SUCCESS ||
            vaCreateBuffer(
                display,
                context,
                VAIQMatrixBufferType,
                sizeof(tables.iq),
                1,
                const_cast<VAIQMatrixBufferJPEGBaseline *>(&tables.iq),
                &iqBuffer) != VA_STATUS_SUCCESS ||
            vaCreateBuffer(
                display,
                context,
                VAHuffmanTableBufferType,
                sizeof(tables.huffman),
                1,
                const_cast<VAHuffmanTableBufferJPEGBaseline *>(&tables.huffman),
                &huffmanBuffer) != VA_STATUS_SUCCESS ||
            vaCreateBuffer(
                display,
                context,
                VASliceParameterBufferType,
                sizeof(tables.slice),
                1,
                const_cast<VASliceParameterBufferJPEGBaseline *>(&tables.slice),
                &sliceParamBuffer) != VA_STATUS_SUCCESS) {
            return false;
        }

        const auto sliceStartNs = timings ? consolation::capture::monotonicClockNs() : 0;
        if (!createSliceDataBuffer(data, bytesUsed, sliceDataBuffer)) {
            return false;
        }
        if (timings != nullptr) {
            timings->sliceMs = elapsedMs(sliceStartNs);
        }

        return renderWithBuffers(
            targetSurface,
            pictureBuffer,
            iqBuffer,
            huffmanBuffer,
            sliceParamBuffer,
            sliceDataBuffer,
            true,
            timings);
    }

    static void closePrimeDescriptor(VADRMPRIMESurfaceDescriptor &descriptor)
    {
        for (uint32_t index = 0; index < descriptor.num_objects; ++index) {
            if (descriptor.objects[index].fd >= 0) {
                ::close(descriptor.objects[index].fd);
                descriptor.objects[index].fd = -1;
            }
        }
    }

    [[nodiscard]] bool probeSurfaceDmaExport(const VASurfaceID targetSurface) const
    {
        if (targetSurface == VA_INVALID_ID) {
            return false;
        }

        VADRMPRIMESurfaceDescriptor descriptor {};
        const auto status = vaExportSurfaceHandle(
            display,
            targetSurface,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
            &descriptor);
        if (status == VA_STATUS_SUCCESS) {
            closePrimeDescriptor(descriptor);
        }
        return status == VA_STATUS_SUCCESS;
    }

    [[nodiscard]] bool exportSurfaceDma(
        const VASurfaceID targetSurface,
        const int slotIndex,
        const int cropWidth,
        const int cropHeight,
        VaapiDmaFrame &out)
    {
        out.dmaFd = -1;
        out.planeFds = { -1, -1 };
        out.stride = 0;
        if (!dmaExportSupported_ || targetSurface == VA_INVALID_ID || slotIndex < 0 ||
            slotIndex >= kSurfacePoolSize) {
            return false;
        }

        auto &cached = exportedSlots_[static_cast<size_t>(slotIndex)];
        if (!cached.valid) {
            VADRMPRIMESurfaceDescriptor descriptor {};
            if (vaExportSurfaceHandle(
                    display,
                    targetSurface,
                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                    &descriptor) != VA_STATUS_SUCCESS) {
                return false;
            }

            if (descriptor.num_layers < 2 || descriptor.num_objects < 1) {
                closePrimeDescriptor(descriptor);
                return false;
            }

            const auto &yLayer = descriptor.layers[0];
            const auto &uvLayer = descriptor.layers[1];
            if (yLayer.num_planes < 1 || uvLayer.num_planes < 1 ||
                yLayer.object_index[0] >= descriptor.num_objects ||
                uvLayer.object_index[0] >= descriptor.num_objects) {
                closePrimeDescriptor(descriptor);
                return false;
            }

            const auto yObjectIndex = yLayer.object_index[0];
            const auto uvObjectIndex = uvLayer.object_index[0];
            const auto yObjectFd = descriptor.objects[yObjectIndex].fd;
            const auto uvObjectFd = descriptor.objects[uvObjectIndex].fd;
            if (yObjectFd < 0 || uvObjectFd < 0) {
                closePrimeDescriptor(descriptor);
                return false;
            }

            auto dupFd = ::dup(yObjectFd);
            const auto dupUvFd = ::dup(uvObjectFd);
            if (dupFd < 0 || dupUvFd < 0) {
                if (dupFd >= 0) {
                    ::close(dupFd);
                }
                if (dupUvFd >= 0) {
                    ::close(dupUvFd);
                }
                closePrimeDescriptor(descriptor);
                return false;
            }

            cached.planeFds = { dupFd, dupUvFd };
            cached.planeOffsets = { static_cast<int>(yLayer.offset[0]), static_cast<int>(uvLayer.offset[0]) };
            cached.planeStrides = { static_cast<int>(yLayer.pitch[0]), static_cast<int>(uvLayer.pitch[0]) };
            cached.planeFourccs = { yLayer.drm_format, uvLayer.drm_format };
            cached.planeModifiers = {
                descriptor.objects[yObjectIndex].drm_format_modifier,
                descriptor.objects[uvObjectIndex].drm_format_modifier,
            };
            cached.stride = static_cast<int>(yLayer.pitch[0]);
            cached.valid = true;
            closePrimeDescriptor(descriptor);
        }

        out.dmaFd = cached.planeFds[0];
        out.planeFds = cached.planeFds;
        out.planeOffsets = cached.planeOffsets;
        out.planeStrides = cached.planeStrides;
        out.planeFourccs = cached.planeFourccs;
        out.planeModifiers = cached.planeModifiers;
        out.stride = cached.stride;
        return out.dmaFd >= 0 && out.stride > 0 && cropWidth > 0 && cropHeight > 0;
    }

    [[nodiscard]] bool ensureExportImage(const int cropWidth, const int cropHeight)
    {
        if (exportImage_.image_id != VA_INVALID_ID && exportImageWidth_ == cropWidth &&
            exportImageHeight_ == cropHeight) {
            return true;
        }

        releaseExportImage();

        VAImageFormat imageFormat {};
        imageFormat.fourcc = VA_FOURCC_NV12;
        if (vaCreateImage(
                display,
                &imageFormat,
                static_cast<unsigned int>(cropWidth),
                static_cast<unsigned int>(cropHeight),
                &exportImage_) != VA_STATUS_SUCCESS) {
            return false;
        }

        exportImageWidth_ = cropWidth;
        exportImageHeight_ = cropHeight;
        return true;
    }

    [[nodiscard]] bool exportSurfaceToArgb(
        const VASurfaceID targetSurface,
        const int cropWidth,
        const int cropHeight,
        const int dstWidth,
        const int dstHeight,
        uint8_t *dstArgb,
        const int dstStride)
    {
        if (cropWidth <= 0 || cropHeight <= 0 || targetSurface == VA_INVALID_ID) {
            return false;
        }

        VAImage image {};
        bool destroyImageAfterExport = false;

        if (kOptPersistentExportImage) {
            if (!ensureExportImage(cropWidth, cropHeight)) {
                return false;
            }
            image = exportImage_;
        } else {
            VAImageFormat imageFormat {};
            imageFormat.fourcc = VA_FOURCC_NV12;
            if (vaCreateImage(
                    display,
                    &imageFormat,
                    static_cast<unsigned int>(cropWidth),
                    static_cast<unsigned int>(cropHeight),
                    &image) != VA_STATUS_SUCCESS) {
                return false;
            }
            destroyImageAfterExport = true;
        }

        if (vaGetImage(
                display,
                targetSurface,
                0,
                0,
                static_cast<unsigned int>(cropWidth),
                static_cast<unsigned int>(cropHeight),
                image.image_id) != VA_STATUS_SUCCESS) {
            if (destroyImageAfterExport) {
                vaDestroyImage(display, image.image_id);
            }
            return false;
        }

        void *mapped = nullptr;
        if (vaMapBuffer(display, image.buf, &mapped) != VA_STATUS_SUCCESS) {
            if (destroyImageAfterExport) {
                vaDestroyImage(display, image.image_id);
            }
            return false;
        }

        const auto *yPlane = static_cast<const uint8_t *>(mapped) + image.offsets[0];
        const int yStride = static_cast<int>(image.pitches[0]);
        const uint32_t fourcc = image.format.fourcc;

        int convertResult = -1;
        const auto convertCrop = [&](uint8_t *argb, const int argbStride) {
            if (fourcc == VA_FOURCC_NV12) {
                const auto *uvPlane = static_cast<const uint8_t *>(mapped) + image.offsets[1];
                return libyuv::NV12ToARGB(
                    yPlane,
                    yStride,
                    uvPlane,
                    static_cast<int>(image.pitches[1]),
                    argb,
                    argbStride,
                    cropWidth,
                    cropHeight);
            }

            if (fourcc == VA_FOURCC_I420) {
                const auto *uPlane = static_cast<const uint8_t *>(mapped) + image.offsets[1];
                const auto *vPlane = static_cast<const uint8_t *>(mapped) + image.offsets[2];
                return libyuv::I420ToARGB(
                    yPlane,
                    yStride,
                    uPlane,
                    static_cast<int>(image.pitches[1]),
                    vPlane,
                    static_cast<int>(image.pitches[2]),
                    argb,
                    argbStride,
                    cropWidth,
                    cropHeight);
            }

            if (fourcc == VA_FOURCC_YV12) {
                const auto *vPlane = static_cast<const uint8_t *>(mapped) + image.offsets[1];
                const auto *uPlane = static_cast<const uint8_t *>(mapped) + image.offsets[2];
                return libyuv::I420ToARGB(
                    yPlane,
                    yStride,
                    uPlane,
                    static_cast<int>(image.pitches[2]),
                    vPlane,
                    static_cast<int>(image.pitches[1]),
                    argb,
                    argbStride,
                    cropWidth,
                    cropHeight);
            }

            return -1;
        };

        if (cropWidth == dstWidth && cropHeight == dstHeight) {
            convertResult = convertCrop(dstArgb, dstStride);
        } else {
            const auto scratchBytes = static_cast<size_t>(cropWidth) * static_cast<size_t>(cropHeight) * 4U;
            argbScratch.resize(scratchBytes);
            convertResult = convertCrop(argbScratch.data(), cropWidth * 4);
            if (convertResult == 0) {
                convertResult = libyuv::ARGBScale(
                    argbScratch.data(),
                    cropWidth * 4,
                    cropWidth,
                    cropHeight,
                    dstArgb,
                    dstStride,
                    dstWidth,
                    dstHeight,
                    libyuv::kFilterBilinear);
            }
        }

        vaUnmapBuffer(display, image.buf);
        if (destroyImageAfterExport) {
            vaDestroyImage(display, image.image_id);
        }
        return convertResult == 0;
    }

    [[nodiscard]] bool decodeFrame(
        const uint8_t *data,
        const int bytesUsed,
        const int dstWidth,
        const int dstHeight,
        uint8_t *dstArgb,
        const int dstStride)
    {
        const auto perfEnabled = mjpeg_perf::loggingEnabled();
        const auto totalStartNs = perfEnabled ? consolation::capture::monotonicClockNs() : 0;
        mjpeg_perf::Sample perf {};

        VaapiJpegTables tables {};
        const auto parseStartNs = perfEnabled ? consolation::capture::monotonicClockNs() : 0;
        if (!parseVaapiTables(data, static_cast<size_t>(bytesUsed), tables)) {
            return false;
        }
        if (perfEnabled) {
            perf.parseMs = elapsedMs(parseStartNs);
        }

        const auto frameWidth = static_cast<int>(tables.picture.picture_width);
        const auto frameHeight = static_cast<int>(tables.picture.picture_height);
        if (!ensureContext(frameWidth, frameHeight)) {
            return false;
        }

        const auto slotIndex = acquireSlot();
        if (slotIndex < 0) {
            return false;
        }

        const auto targetSurface = surfaceAt(slotIndex);
        const auto renderStartNs = perfEnabled ? consolation::capture::monotonicClockNs() : 0;
        const auto rendered = renderJpegToSurface(targetSurface, data, bytesUsed, tables);
        if (perfEnabled) {
            perf.renderMs = elapsedMs(renderStartNs);
        }

        const auto exportStartNs = perfEnabled ? consolation::capture::monotonicClockNs() : 0;
        const auto exported =
            rendered && exportSurfaceToArgb(targetSurface, frameWidth, frameHeight, dstWidth, dstHeight, dstArgb, dstStride);
        if (perfEnabled) {
            perf.exportMs = elapsedMs(exportStartNs);
            perf.dmaPath = false;
            perf.reuseBuffers = kOptReuseVaTableBuffers;
            perf.totalMs = elapsedMs(totalStartNs);
            mjpeg_perf::recordVaapiSample(perf);
        }

        releaseSlot(slotIndex);
        return exported;
    }

    [[nodiscard]] bool decodeFrameDma(const uint8_t *data, const int bytesUsed, VaapiDmaFrame &out)
    {
        out = {};
        if (!dmaExportSupported_) {
            return false;
        }

        const auto perfEnabled = mjpeg_perf::loggingEnabled();
        const auto traceEnabled = frame_trace::enabled();
        const auto totalStartNs = perfEnabled ? consolation::capture::monotonicClockNs() : 0;
        mjpeg_perf::Sample perf {};
        RenderTimings renderTimings {};
        RenderTimings *const renderTimingsPtr = traceEnabled ? &renderTimings : nullptr;
        frame_trace::VaapiDecodeSpan traceSpan {};
        traceSpan.dmaPath = true;

        VaapiJpegTables tables {};
        const auto needParseTime = perfEnabled || traceEnabled;
        const auto parseStartNs = needParseTime ? consolation::capture::monotonicClockNs() : 0;
        if (!parseVaapiTables(data, static_cast<size_t>(bytesUsed), tables)) {
            return false;
        }
        if (needParseTime) {
            const auto parseMs = elapsedMs(parseStartNs);
            perf.parseMs = parseMs;
            traceSpan.parseMs = parseMs;
        }

        const auto frameWidth = static_cast<int>(tables.picture.picture_width);
        const auto frameHeight = static_cast<int>(tables.picture.picture_height);
        if (!ensureContext(frameWidth, frameHeight)) {
            return false;
        }

        const auto slotIndex = acquireSlot();
        if (slotIndex < 0) {
            return false;
        }

        const auto targetSurface = surfaceAt(slotIndex);
        const auto renderStartNs = perfEnabled ? consolation::capture::monotonicClockNs() : 0;
        if (!renderJpegToSurface(targetSurface, data, bytesUsed, tables, renderTimingsPtr)) {
            releaseSlot(slotIndex);
            return false;
        }
        if (perfEnabled) {
            perf.renderMs = elapsedMs(renderStartNs);
        }

        const auto exportStartNs = needParseTime ? consolation::capture::monotonicClockNs() : 0;
        if (!exportSurfaceDma(targetSurface, slotIndex, frameWidth, frameHeight, out)) {
            releaseSlot(slotIndex);
            return false;
        }
        if (perfEnabled) {
            perf.exportMs = elapsedMs(exportStartNs);
            perf.dmaPath = true;
            perf.reuseBuffers = kOptReuseVaTableBuffers;
            perf.totalMs = elapsedMs(totalStartNs);
            mjpeg_perf::recordVaapiSample(perf);
        }

        out.width = frameWidth;
        out.height = frameHeight;
        out.slotIndex = slotIndex;
        if (traceEnabled) {
            traceSpan.exportFdMs = elapsedMs(exportStartNs);
            traceSpan.sliceBufferMs = renderTimings.sliceMs;
            traceSpan.vaSubmitMs = renderTimings.submitMs;
            traceSpan.vaSyncMs = renderTimings.syncMs;
            traceSpan.seq = frame_trace::nextSeq();
            frame_trace::recordVaapiDecode(traceSpan);
            out.seq = traceSpan.seq;
        }
        return true;
    }

};

VaapiDecoder::~VaapiDecoder()
{
    delete impl_;
}

bool VaapiDecoder::initialize(const int maxWidth, const int maxHeight)
{
    delete impl_;
    impl_ = nullptr;
    disabled_ = false;

    const auto drmFd = openVaapiRenderNode();
    if (drmFd < 0) {
        return false;
    }

    if (drmDriverHasNoVaapi(drmFd)) {
        ::close(drmFd);
        return false;
    }

    auto *impl = new Impl();
    impl->drmFd = drmFd;
    impl->display = vaGetDisplayDRM(drmFd);
    if (impl->display == nullptr) {
        impl->release();
        delete impl;
        return false;
    }

    int majorVersion = 0;
    int minorVersion = 0;
    if (vaInitialize(impl->display, &majorVersion, &minorVersion) != VA_STATUS_SUCCESS) {
        impl->release();
        delete impl;
        return false;
    }
    impl->vaInitialized = true;

    VAConfigAttrib attrib {};
    attrib.type = VAConfigAttribRTFormat;
    VAConfigAttrib attribs[] = {attrib};
    if (vaGetConfigAttributes(impl->display, VAProfileJPEGBaseline, VAEntrypointVLD, attribs, 1) != VA_STATUS_SUCCESS ||
        (attribs[0].value & VA_RT_FORMAT_YUV420) == 0) {
        impl->release();
        delete impl;
        return false;
    }

    if (vaCreateConfig(impl->display, VAProfileJPEGBaseline, VAEntrypointVLD, nullptr, 0, &impl->config) !=
            VA_STATUS_SUCCESS ||
        !impl->ensureContext(maxWidth, maxHeight)) {
        impl->release();
        delete impl;
        return false;
    }

    impl->dmaExportSupported_ = impl->probeSurfaceDmaExport(impl->surfaceAt(0));

    impl_ = impl;
    return true;
}

void VaapiDecoder::disable()
{
    disabled_ = true;
}

bool VaapiDecoder::dmaExportSupported() const
{
    return !disabled_ && impl_ != nullptr && impl_->dmaExportSupported_;
}

bool VaapiDecoder::dmaSlotsAvailable() const
{
    return !disabled_ && impl_ != nullptr && impl_->hasAvailableSlot();
}

bool VaapiDecoder::decodeDma(const uint8_t *data, const int bytesUsed, VaapiDmaFrame &out)
{
    if (disabled_ || impl_ == nullptr) {
        return false;
    }
    return impl_->decodeFrameDma(data, bytesUsed, out);
}

void VaapiDecoder::releaseDmaSlot(const int slotIndex)
{
    if (impl_ != nullptr) {
        impl_->releaseSlot(slotIndex);
    }
}

bool VaapiDecoder::decode(
    const uint8_t *data,
    const int bytesUsed,
    const int dstWidth,
    const int dstHeight,
    uint8_t *dstArgb,
    const int dstStride)
{
    if (disabled_ || impl_ == nullptr) {
        return false;
    }
    return impl_->decodeFrame(data, bytesUsed, dstWidth, dstHeight, dstArgb, dstStride);
}

#else

VaapiDecoder::~VaapiDecoder() = default;

bool VaapiDecoder::initialize(int, int)
{
    return false;
}

void VaapiDecoder::disable() {}

bool VaapiDecoder::dmaExportSupported() const
{
    return false;
}

bool VaapiDecoder::dmaSlotsAvailable() const
{
    return false;
}

bool VaapiDecoder::decodeDma(const uint8_t *, int, VaapiDmaFrame &)
{
    return false;
}

void VaapiDecoder::releaseDmaSlot(int) {}

bool VaapiDecoder::decode(const uint8_t *, int, int, int, uint8_t *, int)
{
    return false;
}

#endif

} // namespace consolation::platform::linux::mjpeg_backend
