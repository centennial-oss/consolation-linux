#pragma once

#include <QMetaType>

#include <memory>

namespace consolation::capture {

// V4L2 capture buffer exported via VIDIOC_EXPBUF (NV12 spike). The dma fd is owned by the
// capture session buffer pool; the handle only controls when the buffer is requeued (QBUF).
struct DmaBufFrame {
    int bufferIndex = -1;
    int dmaFd = -1;
    int width = 0;
    int height = 0;
    int stride = 0;
    int bytesUsed = 0;
};

using DmaBufFrameHandle = std::shared_ptr<DmaBufFrame>;

} // namespace consolation::capture

Q_DECLARE_METATYPE(consolation::capture::DmaBufFrameHandle)
