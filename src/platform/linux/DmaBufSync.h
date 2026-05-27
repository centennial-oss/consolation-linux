#pragma once

#include <atomic>
#include <cerrno>

#include <fcntl.h>
#include <sys/ioctl.h>

#include <linux/dma-buf.h>
#include <linux/ioctl.h>

#ifndef DMA_BUF_SYNC_IOCTL
#define DMA_BUF_BASE 'b'
#define DMA_BUF_SYNC_IOCTL _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#endif

namespace consolation::platform::linux {

enum class DmaBufSyncSupport {
    Unknown,
    Supported,
    Unsupported,
};

inline std::atomic<DmaBufSyncSupport> &dmaBufSyncSupport()
{
    static std::atomic<DmaBufSyncSupport> support { DmaBufSyncSupport::Unknown };
    return support;
}

[[nodiscard]] inline bool dmaBufSyncIsOptional()
{
    return dmaBufSyncSupport().load(std::memory_order_relaxed) == DmaBufSyncSupport::Unsupported;
}

inline bool dmaBufSync(const int fd, const unsigned int flags)
{
    if (fd < 0) {
        return false;
    }

    auto &support = dmaBufSyncSupport();
    if (support.load(std::memory_order_relaxed) == DmaBufSyncSupport::Unsupported) {
        return true;
    }

    dma_buf_sync sync {};
    sync.flags = flags;
    if (::ioctl(fd, DMA_BUF_SYNC_IOCTL, &sync) == 0) {
        support.store(DmaBufSyncSupport::Supported, std::memory_order_relaxed);
        return true;
    }

    // Some exporters/drivers do not implement sync; treat as optional.
    if (errno == ENOTTY || errno == EINVAL || errno == EOPNOTSUPP) {
        support.store(DmaBufSyncSupport::Unsupported, std::memory_order_relaxed);
        return true;
    }

    return false;
}

inline bool dmaBufSyncStartRead(const int fd)
{
    return dmaBufSync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

inline bool dmaBufSyncEndRead(const int fd)
{
    return dmaBufSync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

// Call before EGL/GL first reads a dma-buf (e.g. on import). Pair with dmaBufSyncEndRead after GPU work.
[[nodiscard]] inline bool dmaBufBeginRead(int &activeFd, const int fd)
{
    activeFd = -1;
    if (fd < 0) {
        return false;
    }
    if (!dmaBufSyncStartRead(fd)) {
        return false;
    }
    if (!dmaBufSyncIsOptional()) {
        activeFd = fd;
    }
    return true;
}

inline void dmaBufEndRead(int &activeFd)
{
    if (activeFd < 0) {
        return;
    }
    dmaBufSyncEndRead(activeFd);
    activeFd = -1;
}

// Ensures END is issued when START succeeded. Prefer dmaBufBeginRead/dmaBufEndRead when END must
// follow glFinish rather than scope exit.
class DmaBufReadGuard final {
public:
    explicit DmaBufReadGuard(const int fd)
        : fd_(fd)
    {
        if (fd_ >= 0) {
            started_ = dmaBufSyncStartRead(fd_);
            if (started_ && !dmaBufSyncIsOptional()) {
                activeFd_ = fd_;
            }
        }
    }

    ~DmaBufReadGuard()
    {
        dmaBufEndRead(activeFd_);
    }

    DmaBufReadGuard(const DmaBufReadGuard &) = delete;
    DmaBufReadGuard &operator=(const DmaBufReadGuard &) = delete;

    [[nodiscard]] bool started() const
    {
        return started_;
    }

private:
    int fd_ = -1;
    int activeFd_ = -1;
    bool started_ = false;
};

} // namespace consolation::platform::linux
