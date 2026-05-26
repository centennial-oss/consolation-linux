#pragma once

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

inline bool dmaBufSync(const int fd, const unsigned int flags)
{
    if (fd < 0) {
        return false;
    }

    dma_buf_sync sync {};
    sync.flags = flags;
    if (::ioctl(fd, DMA_BUF_SYNC_IOCTL, &sync) == 0) {
        return true;
    }

    // Some exporters/drivers do not implement sync; treat as optional.
    return errno == ENOTTY || errno == EINVAL || errno == EOPNOTSUPP;
}

inline bool dmaBufSyncStartRead(const int fd)
{
    return dmaBufSync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

inline bool dmaBufSyncEndRead(const int fd)
{
    return dmaBufSync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

// Ensures END is issued when START succeeded.
class DmaBufReadGuard final {
public:
    explicit DmaBufReadGuard(const int fd)
        : fd_(fd)
    {
        if (fd_ >= 0) {
            started_ = dmaBufSyncStartRead(fd_);
        }
    }

    ~DmaBufReadGuard()
    {
        if (started_) {
            dmaBufSyncEndRead(fd_);
        }
    }

    DmaBufReadGuard(const DmaBufReadGuard &) = delete;
    DmaBufReadGuard &operator=(const DmaBufReadGuard &) = delete;

    [[nodiscard]] bool started() const
    {
        return started_;
    }

private:
    int fd_ = -1;
    bool started_ = false;
};

} // namespace consolation::platform::linux
