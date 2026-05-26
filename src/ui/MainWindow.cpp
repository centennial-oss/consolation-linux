#include "ui/MainWindow.h"

#include "AppMetadata.h"
#include "app/BuildInfo.h"
#include "capture/CaptureBackendManager.h"
#include "capture/CaptureSession.h"
#include "capture/DmaBufFrame.h"
#include "capture/MonotonicClock.h"
#include "platform/linux/Nv12DmaBufGl.h"
#include "platform/linux/RgbDmaBufGl.h"
#include "platform/linux/YuyvDmaBufGl.h"
#include "platform/linux/I420DmaBufGl.h"
#include "platform/linux/DmaBufSync.h"

#include "capture/FourCc.h"
#include "platform/linux/PipeWireAudioSession.h"
#include "platform/linux/V4L2CaptureSession.h"
#include "ui/AppIcon.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QIODevice>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QMetaType>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QSet>
#include <QStringList>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#include <QSvgRenderer>

#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <optional>
#include <utility>
#include <unistd.h>
#include <array>
#include <atomic>
#include <set>
#include <vector>

namespace consolation::ui {

class ScreenInhibitor final {
public:
    void inhibit(QWindow *window)
    {
        if (active_) {
            return;
        }

        const auto appName = QString::fromUtf8(consolation::app::AppMetadata::displayName);
        const auto reason = QStringLiteral("Video playback is in progress.");

        QDBusInterface freedesktop(
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("/org/freedesktop/ScreenSaver"),
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QDBusConnection::sessionBus());
        const QDBusReply<uint> freedesktopReply = freedesktop.call(QStringLiteral("Inhibit"), appName, reason);
        if (freedesktopReply.isValid()) {
            active_ = true;
            service_ = Service::FreedesktopScreenSaver;
            cookie_ = freedesktopReply.value();
            return;
        }

        QDBusInterface gnome(
            QStringLiteral("org.gnome.SessionManager"),
            QStringLiteral("/org/gnome/SessionManager"),
            QStringLiteral("org.gnome.SessionManager"),
            QDBusConnection::sessionBus());
        const auto xid = static_cast<uint>(window != nullptr ? window->winId() : 0);
        const QDBusReply<uint> gnomeReply = gnome.call(QStringLiteral("Inhibit"), appName, xid, reason, uint { 8 });
        if (gnomeReply.isValid()) {
            active_ = true;
            service_ = Service::GnomeSessionManager;
            cookie_ = gnomeReply.value();
        }
    }

    void uninhibit()
    {
        if (!active_) {
            return;
        }

        switch (service_) {
        case Service::FreedesktopScreenSaver: {
            QDBusInterface freedesktop(
                QStringLiteral("org.freedesktop.ScreenSaver"),
                QStringLiteral("/org/freedesktop/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"),
                QDBusConnection::sessionBus());
            freedesktop.call(QStringLiteral("UnInhibit"), cookie_);
            break;
        }
        case Service::GnomeSessionManager: {
            QDBusInterface gnome(
                QStringLiteral("org.gnome.SessionManager"),
                QStringLiteral("/org/gnome/SessionManager"),
                QStringLiteral("org.gnome.SessionManager"),
                QDBusConnection::sessionBus());
            gnome.call(QStringLiteral("Uninhibit"), cookie_);
            break;
        }
        }

        active_ = false;
        cookie_ = 0;
    }

private:
    enum class Service {
        FreedesktopScreenSaver,
        GnomeSessionManager,
    };

    bool active_ = false;
    Service service_ = Service::FreedesktopScreenSaver;
    uint cookie_ = 0;
};

class FrameRenderer {
public:
    using FramePresentedHandler = std::function<void(qint64 latencyNs)>;

    virtual ~FrameRenderer() = default;
    virtual QWidget *widget() = 0;
    virtual void setFirstFramePaintedHandler(std::function<void()> handler)
    {
        Q_UNUSED(handler);
    }
    virtual void setFramePresentedHandler(FramePresentedHandler handler)
    {
        framePresentedHandler_ = std::move(handler);
    }
    virtual void setFrame(capture::FrameHandle frame, qint64 capturedAtNs = 0) = 0;
    virtual void setDmaFrame(capture::DmaBufFrameHandle frame)
    {
        Q_UNUSED(frame);
    }
    virtual int takePaintCount() = 0;

protected:
    void reportFramePresented()
    {
        if (!framePresentedHandler_ || displayCapturedAtNs_ <= 0) {
            return;
        }
        framePresentedHandler_(capture::monotonicClockNs() - displayCapturedAtNs_);
        displayCapturedAtNs_ = 0;
    }

    FramePresentedHandler framePresentedHandler_;
    qint64 displayCapturedAtNs_ = 0;
};

class CpuFrameRenderer final : public QWidget, public FrameRenderer {
public:
    explicit CpuFrameRenderer(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAutoFillBackground(false);
    }

    QWidget *widget() override
    {
        return this;
    }

    void setFirstFramePaintedHandler(std::function<void()> handler) override
    {
        firstFramePaintedHandler_ = std::move(handler);
    }

    void setFrame(capture::FrameHandle frame, const qint64 capturedAtNs = 0) override
    {
        displayCapturedAtNs_ = capturedAtNs;
        frame_ = std::move(frame);
        updateTargetRect();
        repaint();
    }

    int takePaintCount() override
    {
        const auto count = paintCount_;
        paintCount_ = 0;
        return count;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        paintFrame(painter);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        updateTargetRect();
    }

private:
    void updateTargetRect()
    {
        if (!frame_ || frame_->isNull()) {
            targetRect_ = {};
            return;
        }

        auto targetSize = frame_->size();
        targetSize.scale(size(), Qt::KeepAspectRatio);
        targetRect_ = QRect(
            QPoint((width() - targetSize.width()) / 2, (height() - targetSize.height()) / 2),
            targetSize);
    }

    void paintFrame(QPainter &painter)
    {
        ++paintCount_;
        painter.fillRect(rect(), Qt::black);
        if (!frame_ || frame_->isNull()) {
            return;
        }

        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        if (targetRect_.size() == frame_->size()) {
            painter.drawImage(targetRect_.topLeft(), *frame_);
        } else {
            painter.drawImage(targetRect_, *frame_);
        }
        reportFramePresented();
        notifyFirstFramePainted();
    }

    void notifyFirstFramePainted()
    {
        if (firstFramePainted_ || !firstFramePaintedHandler_) {
            return;
        }
        firstFramePainted_ = true;
        firstFramePaintedHandler_();
    }

    capture::FrameHandle frame_;
    QRect targetRect_;
    std::function<void()> firstFramePaintedHandler_;
    bool firstFramePainted_ = false;
    int paintCount_ = 0;
};

// QWindow subclass that provides the OpenGL surface for the dedicated render thread.
class GpuRenderWindow final : public QWindow {
    Q_OBJECT
public:
    GpuRenderWindow()
    {
        setSurfaceType(QSurface::OpenGLSurface);
        QSurfaceFormat fmt;
        fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        fmt.setDepthBufferSize(0);
        fmt.setStencilBufferSize(0);
        setFormat(fmt);
    }

signals:
    void windowExposed();
    void windowResized(QSize size, float dpr);

protected:
    void exposeEvent(QExposeEvent *) override
    {
        if (isExposed()) {
            emit windowExposed();
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWindow::resizeEvent(event);
        emit windowResized(size(), static_cast<float>(devicePixelRatio()));
    }
};

// Dedicated render thread that owns the GL context and DMA-BUF renderers.
// Receives frames via a single-slot mutex+condvar queue — no Qt event queue on the hot path.
class VideoRenderThread final : public QThread {
    Q_OBJECT
public:
    explicit VideoRenderThread(GpuRenderWindow *window, QObject *parent = nullptr)
        : QThread(parent)
        , window_(window)
    {}

    ~VideoRenderThread() override
    {
        requestStop();
        wait();
    }

    // Thread-safe: called from any thread to deliver a frame to the render loop.
    void enqueueFrame(capture::DmaBufFrameHandle frame)
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            pendingFrame_ = std::move(frame);
        }
        cv_.notify_one();
    }

    // Thread-safe: called from UI thread on resize.
    void notifyResize(const QSize size, const float dpr)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        windowSize_ = size;
        dpr_ = dpr;
    }

    // Thread-safe: signals the render loop to exit.
    void requestStop()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
    }

    int takePaintCount()
    {
        return paintCount_.exchange(0);
    }

signals:
    void firstFramePainted();
    void framePresented(qint64 latencyNs);
    void dmaCpuFallback(capture::DmaBufFrameHandle frame);

protected:
    void run() override
    {
        QOpenGLContext context;
        context.setFormat(window_->format());
        if (!context.create()) {
            std::cout << "[VideoRenderThread] failed to create GL context" << std::endl;
            std::cout.flush();
            return;
        }
        if (!context.makeCurrent(window_)) {
            std::cout << "[VideoRenderThread] failed to make GL context current" << std::endl;
            std::cout.flush();
            return;
        }

        QOpenGLFunctions gl;
        gl.initializeOpenGLFunctions();

        nv12Gl_.emplace();
        if (!nv12Gl_->initialize()) {
            std::cout << "[VideoRenderThread] NV12 DMA-BUF GL unavailable ("
                      << nv12Gl_->lastInitFailure().toStdString() << "); NV12 will use CPU decode" << std::endl;
            nv12Gl_.reset();
        }
        rgbGl_.emplace();
        if (!rgbGl_->initialize()) {
            std::cout << "[VideoRenderThread] RGB/BGR DMA-BUF GL unavailable ("
                      << rgbGl_->lastInitFailure().toStdString() << "); RGB24/BGR24 will use CPU decode" << std::endl;
            rgbGl_.reset();
        }
        yuyvGl_.emplace();
        if (!yuyvGl_->initialize()) {
            std::cout << "[VideoRenderThread] YUYV DMA-BUF GL unavailable ("
                      << yuyvGl_->lastInitFailure().toStdString() << "); YUYV/YUY2 will use CPU decode" << std::endl;
            yuyvGl_.reset();
        }
        i420Gl_.emplace();
        if (!i420Gl_->initialize()) {
            std::cout << "[VideoRenderThread] I420/YV12 DMA-BUF GL unavailable ("
                      << i420Gl_->lastInitFailure().toStdString() << "); YU12/I420/YV12 will use CPU decode" << std::endl;
            i420Gl_.reset();
        }
        std::cout.flush();

        while (true) {
            capture::DmaBufFrameHandle frame;
            QSize windowSize;
            float dpr = 1.0f;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return pendingFrame_ || stopping_; });
                if (stopping_) break;
                frame = std::move(pendingFrame_);
                windowSize = windowSize_;
                dpr = dpr_;
            }

            if (!frame) continue;

            if (!tryRenderFrame(frame, windowSize, dpr, gl)) {
                boundDmaFrame_.reset();
                emit dmaCpuFallback(std::move(frame));
            } else {
                context.swapBuffers(window_);
                ++paintCount_;
            }
        }

        if (nv12Gl_.has_value()) { nv12Gl_->releaseAllSlots(); nv12Gl_->shutdown(); nv12Gl_.reset(); }
        if (rgbGl_.has_value()) { rgbGl_->releaseAllSlots(); rgbGl_->shutdown(); rgbGl_.reset(); }
        if (yuyvGl_.has_value()) { yuyvGl_->releaseAllSlots(); yuyvGl_->shutdown(); yuyvGl_.reset(); }
        if (i420Gl_.has_value()) { i420Gl_->releaseAllSlots(); i420Gl_->shutdown(); i420Gl_.reset(); }
        context.doneCurrent();
    }

private:
    bool tryRenderFrame(const capture::DmaBufFrameHandle &frame,
                        const QSize windowSize, const float dpr,
                        QOpenGLFunctions &gl)
    {
        if (!frame) return false;

        platform::linux::DmaBufReadGuard syncGuard(frame->dmaFd);
        if (!syncGuard.started()) return false;

        const QSize srcSize(frame->width, frame->height);
        if (srcSize != lastSourceSize_ || windowSize != lastWindowSize_) {
            cachedTargetRect_ = computeTargetRect(srcSize, windowSize);
            lastSourceSize_ = srcSize;
            lastWindowSize_ = windowSize;
        }
        const auto &targetRect = cachedTargetRect_;
        gl.glViewport(0, 0,
                      static_cast<GLsizei>(windowSize.width() * dpr),
                      static_cast<GLsizei>(windowSize.height() * dpr));

        bool rendered = false;
        if (frame->layout == capture::DmaBufLayout::Nv12) {
            if (!nv12Gl_ || !nv12Gl_->isAvailable()) return false;
            if (boundDmaFrame_ != frame) {
                if (!nv12Gl_->bindFrame(frame)) { boundDmaFrame_.reset(); return false; }
                boundDmaFrame_ = frame;
            }
            nv12Gl_->draw(windowSize, targetRect, dpr);
            rendered = true;
        } else if (frame->layout == capture::DmaBufLayout::Rgb888 ||
                   frame->layout == capture::DmaBufLayout::Bgr888) {
            if (!rgbGl_ || !rgbGl_->isAvailable()) return false;
            if (boundDmaFrame_ != frame) {
                if (!rgbGl_->bindFrame(frame)) { boundDmaFrame_.reset(); return false; }
                boundDmaFrame_ = frame;
            }
            rgbGl_->draw(windowSize, targetRect, dpr);
            rendered = true;
        } else if (frame->layout == capture::DmaBufLayout::Yuyv422) {
            if (!yuyvGl_ || !yuyvGl_->isAvailable()) return false;
            if (boundDmaFrame_ != frame) {
                if (!yuyvGl_->bindFrame(frame)) { boundDmaFrame_.reset(); return false; }
                boundDmaFrame_ = frame;
            }
            yuyvGl_->draw(windowSize, targetRect, dpr);
            rendered = true;
        } else if (frame->layout == capture::DmaBufLayout::I420 ||
                   frame->layout == capture::DmaBufLayout::Yv12) {
            if (!i420Gl_ || !i420Gl_->isAvailable()) return false;
            if (boundDmaFrame_ != frame) {
                if (!i420Gl_->bindFrame(frame)) { boundDmaFrame_.reset(); return false; }
                boundDmaFrame_ = frame;
            }
            i420Gl_->draw(windowSize, targetRect, dpr);
            rendered = true;
        }

        if (!rendered) return false;

        if (!firstFramePainted_) {
            firstFramePainted_ = true;
            emit firstFramePainted();
        }
        if (frame->capturedAtNs > 0) {
            emit framePresented(capture::monotonicClockNs() - frame->capturedAtNs);
        }
        return true;
    }

    QRect computeTargetRect(const QSize srcSize, const QSize windowSize) const
    {
        auto targetSize = srcSize;
        targetSize.scale(windowSize, Qt::KeepAspectRatio);
        return QRect(
            QPoint((windowSize.width() - targetSize.width()) / 2,
                   (windowSize.height() - targetSize.height()) / 2),
            targetSize);
    }

    GpuRenderWindow *window_;

    std::mutex mutex_;
    std::condition_variable cv_;
    capture::DmaBufFrameHandle pendingFrame_;
    QSize windowSize_;
    float dpr_ = 1.0f;
    bool stopping_ = false;

    std::atomic<int> paintCount_{0};
    bool firstFramePainted_ = false;

    std::optional<platform::linux::Nv12DmaBufGl> nv12Gl_;
    std::optional<platform::linux::RgbDmaBufGl> rgbGl_;
    std::optional<platform::linux::YuyvDmaBufGl> yuyvGl_;
    std::optional<platform::linux::I420DmaBufGl> i420Gl_;
    capture::DmaBufFrameHandle boundDmaFrame_;

    QSize lastSourceSize_;
    QSize lastWindowSize_;
    QRect cachedTargetRect_;
};

class GpuFrameRenderer final : public QWidget, public FrameRenderer {
public:
    using DmaGlFailedHandler = std::function<void()>;
    using DmaCpuFallbackHandler = std::function<void(capture::DmaBufFrameHandle)>;

    explicit GpuFrameRenderer(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAutoFillBackground(false);

        renderWindow_ = new GpuRenderWindow();
        renderContainer_ = QWidget::createWindowContainer(renderWindow_, this);
        renderContainer_->setGeometry(rect());
        renderContainer_->show();
        renderContainer_->lower();

        renderThread_ = new VideoRenderThread(renderWindow_, this);

        connect(renderWindow_, &GpuRenderWindow::windowExposed, this, [this]() {
            if (!renderThread_->isRunning()) {
                renderThread_->notifyResize(renderWindow_->size(),
                                            static_cast<float>(renderWindow_->devicePixelRatio()));
                renderThread_->start();
            }
        });

        connect(renderWindow_, &GpuRenderWindow::windowResized, this, [this](QSize size, float dpr) {
            renderThread_->notifyResize(size, dpr);
        });

        connect(renderThread_, &VideoRenderThread::firstFramePainted, this, [this]() {
            if (!firstFramePainted_ && firstFramePaintedHandler_) {
                firstFramePainted_ = true;
                firstFramePaintedHandler_();
            }
        });

        connect(renderThread_, &VideoRenderThread::framePresented, this, [this](const qint64 latencyNs) {
            if (framePresentedHandler_) {
                framePresentedHandler_(latencyNs);
            }
        });

        connect(renderThread_, &VideoRenderThread::dmaCpuFallback, this,
                [this](capture::DmaBufFrameHandle frame) {
                    if (dmaCpuFallbackHandler_) {
                        dmaCpuFallbackHandler_(std::move(frame));
                    }
                });
    }

    ~GpuFrameRenderer() override
    {
        renderThread_->requestStop();
        renderThread_->wait();
    }

    void setDmaGlFailedHandler(DmaGlFailedHandler handler)
    {
        dmaGlFailedHandler_ = std::move(handler);
    }

    void setDmaCpuFallbackHandler(DmaCpuFallbackHandler handler)
    {
        dmaCpuFallbackHandler_ = std::move(handler);
    }

    // Thread-safe: enqueue a DMA frame directly on the render thread (no UI event loop).
    void submitDmaFrame(capture::DmaBufFrameHandle frame)
    {
        renderThread_->enqueueFrame(std::move(frame));
    }

    void releaseDmaGlState()
    {
        if (renderThread_->isRunning()) {
            renderThread_->requestStop();
            renderThread_->wait();
        }
    }

    QWidget *widget() override
    {
        return this;
    }

    void setFirstFramePaintedHandler(std::function<void()> handler) override
    {
        firstFramePaintedHandler_ = std::move(handler);
    }

    void setFrame(capture::FrameHandle frame, const qint64 capturedAtNs = 0) override
    {
        if (renderContainer_) renderContainer_->hide();
        displayCapturedAtNs_ = capturedAtNs;
        cpuFrame_ = std::move(frame);
        updateCpuTargetRect();
        repaint();
    }

    void setDmaFrame(capture::DmaBufFrameHandle frame) override
    {
        cpuFrame_.reset();
        if (renderContainer_) renderContainer_->show();
        submitDmaFrame(std::move(frame));
    }

    int takePaintCount() override
    {
        return renderThread_->takePaintCount() + std::exchange(cpuPaintCount_, 0);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (renderContainer_) {
            renderContainer_->setGeometry(rect());
        }
        updateCpuTargetRect();
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        if (!cpuFrame_ || cpuFrame_->isNull()) return;

        QPainter painter(this);
        ++cpuPaintCount_;
        painter.fillRect(rect(), Qt::black);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        if (cpuTargetRect_.size() == cpuFrame_->size()) {
            painter.drawImage(cpuTargetRect_.topLeft(), *cpuFrame_);
        } else {
            painter.drawImage(cpuTargetRect_, *cpuFrame_);
        }
        reportFramePresented();
        if (!firstFramePainted_ && firstFramePaintedHandler_) {
            firstFramePainted_ = true;
            firstFramePaintedHandler_();
        }
    }

private:
    void updateCpuTargetRect()
    {
        if (!cpuFrame_ || cpuFrame_->isNull()) {
            cpuTargetRect_ = {};
            return;
        }
        auto targetSize = cpuFrame_->size();
        targetSize.scale(size(), Qt::KeepAspectRatio);
        cpuTargetRect_ = QRect(
            QPoint((width() - targetSize.width()) / 2, (height() - targetSize.height()) / 2),
            targetSize);
    }

    GpuRenderWindow *renderWindow_ = nullptr;
    QWidget *renderContainer_ = nullptr;
    VideoRenderThread *renderThread_ = nullptr;

    capture::FrameHandle cpuFrame_;
    QRect cpuTargetRect_;
    DmaGlFailedHandler dmaGlFailedHandler_;
    DmaCpuFallbackHandler dmaCpuFallbackHandler_;
    std::function<void()> firstFramePaintedHandler_;
    bool firstFramePainted_ = false;
    int cpuPaintCount_ = 0;
};

bool pixelFormatSupportsDmaDisplay(const QString &pixelFormat)
{
    const auto upper = pixelFormat.toUpper();
    return upper == QStringLiteral("NV12") || upper == QStringLiteral("RGB3") ||
        upper == QStringLiteral("RGB24") || upper == QStringLiteral("BGR3") ||
        upper == QStringLiteral("BGR24") || upper == QStringLiteral("YUYV") ||
        upper == QStringLiteral("YUY2") || upper == QStringLiteral("YU12") ||
        upper == QStringLiteral("I420") || upper == QStringLiteral("YV12") ||
        upper == QStringLiteral("YVU420");
}

bool canCreateOpenGLContext()
{
    QOpenGLContext context;
    if (!context.create()) {
        return false;
    }

    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid()) {
        return false;
    }

    const auto madeCurrent = context.makeCurrent(&surface);
    if (madeCurrent) {
        context.doneCurrent();
    }
    return madeCurrent;
}

class VideoSurface final : public QWidget {
public:
    explicit VideoSurface(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAutoFillBackground(false);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        if (canCreateOpenGLContext()) {
            auto *renderer = new GpuFrameRenderer(this);
            renderer_ = renderer;
            rendererWidget_ = renderer;
            if (dmaGlFailedHandler_) {
                renderer->setDmaGlFailedHandler(dmaGlFailedHandler_);
            }
            if (dmaCpuFallbackHandler_) {
                renderer->setDmaCpuFallbackHandler(dmaCpuFallbackHandler_);
            }
            std::cout << "[MainWindow capture] video renderer: OpenGL" << std::endl;
        } else {
            auto *renderer = new CpuFrameRenderer(this);
            renderer_ = renderer;
            rendererWidget_ = renderer;
            std::cout << "[MainWindow capture] video renderer: CPU fallback" << std::endl;
        }
        std::cout.flush();
        rendererWidget_->lower();
        renderer_->setFirstFramePaintedHandler([this]() { hideStartupOverlay(); });
        if (framePresentedHandler_) {
            renderer_->setFramePresentedHandler(framePresentedHandler_);
        }

        startupOverlay_ = new QLabel(QStringLiteral("Connecting to Capture Card..."), this);
        startupOverlay_->setAlignment(Qt::AlignCenter);
        startupOverlay_->setStyleSheet(QStringLiteral("color: #808080; background-color: black; font-size: 32px; font-weight: 500;"));
        startupOverlay_->hide();

    }

    void setFrame(capture::FrameHandle frame, const qint64 capturedAtNs = 0)
    {
        renderer_->setFrame(std::move(frame), capturedAtNs);
    }

    void setFramePresentedHandler(FrameRenderer::FramePresentedHandler handler)
    {
        framePresentedHandler_ = std::move(handler);
        if (renderer_ != nullptr) {
            renderer_->setFramePresentedHandler(framePresentedHandler_);
        }
    }

    void setStartupOverlayVisible(const bool visible)
    {
        if (!startupOverlay_) {
            return;
        }
        startupOverlay_->setVisible(visible);
        if (visible) {
            startupOverlay_->raise();
        }
    }

    void setPendingFrame(capture::FrameHandle frame, const qint64 capturedAtNs = 0)
    {
        pendingCapturedAtNs_ = capturedAtNs;
        pendingFrame_ = std::move(frame);
        schedulePresent();
    }

    // Thread-safe: deliver a DMA frame straight to the render thread.
    void submitDmaFrame(capture::DmaBufFrameHandle frame)
    {
        if (!frame) {
            return;
        }
        if (auto *gpu = dynamic_cast<GpuFrameRenderer *>(renderer_)) {
            gpu->submitDmaFrame(std::move(frame));
            presentDmaCount_.fetch_add(1, std::memory_order_relaxed);
            uiFrameCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void setDmaGlFailedHandler(GpuFrameRenderer::DmaGlFailedHandler handler)
    {
        dmaGlFailedHandler_ = std::move(handler);
        if (auto *gpu = dynamic_cast<GpuFrameRenderer *>(renderer_)) {
            gpu->setDmaGlFailedHandler(dmaGlFailedHandler_);
        }
    }

    void setDmaCpuFallbackHandler(GpuFrameRenderer::DmaCpuFallbackHandler handler)
    {
        dmaCpuFallbackHandler_ = std::move(handler);
        if (auto *gpu = dynamic_cast<GpuFrameRenderer *>(renderer_)) {
            gpu->setDmaCpuFallbackHandler(dmaCpuFallbackHandler_);
        }
    }

    int takeUiFrameCount()
    {
        return uiFrameCount_.exchange(0, std::memory_order_relaxed);
    }

    void setOverlay(QWidget *overlay)
    {
        overlay_ = overlay;
        positionOverlays();
    }

    void setControlsOverlay(QWidget *overlay)
    {
        controlsOverlay_ = overlay;
        positionOverlays();
    }

    int takePaintCount()
    {
        return renderer_->takePaintCount();
    }

    void takePresentPathCounts(int &dmaPresents, int &cpuPresents)
    {
        dmaPresents = presentDmaCount_.exchange(0, std::memory_order_relaxed);
        cpuPresents = presentCpuCount_.exchange(0, std::memory_order_relaxed);
    }

    void releasePlaybackFrames()
    {
        pendingFrame_.reset();
        if (auto *gpu = dynamic_cast<GpuFrameRenderer *>(renderer_)) {
            gpu->releaseDmaGlState();
        } else {
            renderer_->setFrame({});
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        rendererWidget_->setGeometry(rect());
        rendererWidget_->lower();
        positionOverlays();
    }

private:
    void hideStartupOverlay()
    {
        if (startupOverlay_) {
            startupOverlay_->hide();
        }
    }

    void schedulePresent()
    {
        if (!pendingFrame_ || pendingFrame_->isNull()) {
            pendingFrame_.reset();
            return;
        }

        setFrame(std::move(pendingFrame_), pendingCapturedAtNs_);
        pendingCapturedAtNs_ = 0;
        presentCpuCount_.fetch_add(1, std::memory_order_relaxed);
        uiFrameCount_.fetch_add(1, std::memory_order_relaxed);
    }

    void positionOverlays()
    {
        if (overlay_ != nullptr) {
            overlay_->adjustSize();
            const int margin = 14;
            const auto overlaySize = overlay_->sizeHint();
            overlay_->setGeometry(
                margin,
                std::max(margin, height() - overlaySize.height() - margin),
                std::min(overlaySize.width(), std::max(0, width() - margin * 2)),
                overlaySize.height());
            overlay_->raise();
        }

        if (controlsOverlay_ != nullptr) {
            controlsOverlay_->adjustSize();
            const int margin = 16;
            const auto overlaySize = controlsOverlay_->sizeHint();
            const auto overlayWidth = std::min(overlaySize.width(), std::max(0, width() - margin * 2));
            controlsOverlay_->setGeometry(
                std::max(margin, (width() - overlayWidth) / 2),
                std::max(margin, height() - overlaySize.height() - margin),
                overlayWidth,
                overlaySize.height());
            controlsOverlay_->raise();
        }

        if (startupOverlay_ != nullptr) {
            startupOverlay_->setGeometry(rect());
            startupOverlay_->raise();
        }
    }

    FrameRenderer *renderer_ = nullptr;
    QWidget *rendererWidget_ = nullptr;
    GpuFrameRenderer::DmaGlFailedHandler dmaGlFailedHandler_;
    GpuFrameRenderer::DmaCpuFallbackHandler dmaCpuFallbackHandler_;
    FrameRenderer::FramePresentedHandler framePresentedHandler_;
    capture::FrameHandle pendingFrame_;
    qint64 pendingCapturedAtNs_ = 0;
    QPointer<QLabel> startupOverlay_;
    QPointer<QWidget> overlay_;
    QPointer<QWidget> controlsOverlay_;
    std::atomic<int> uiFrameCount_{0};
    std::atomic<int> presentDmaCount_{0};
    std::atomic<int> presentCpuCount_{0};
};


QString resolveFrameMemoryLabel(const capture::VideoTelemetrySnapshot &telemetry)
{
    if (telemetry.dmaFramesInWindow > 0 && telemetry.cpuFramesInWindow == 0) {
        return capture::frameMemoryLabel(capture::VideoFrameMemory::DmaBuf);
    }
    if (telemetry.cpuFramesInWindow > 0 && telemetry.dmaFramesInWindow == 0) {
        return capture::frameMemoryLabel(capture::VideoFrameMemory::Mmap);
    }
    if (telemetry.dmaFramesInWindow > 0 && telemetry.cpuFramesInWindow > 0) {
        return capture::frameMemoryLabel(capture::VideoFrameMemory::Mixed);
    }
    if (telemetry.dmaCapturePathEnabled) {
        return capture::frameMemoryLabel(capture::VideoFrameMemory::DmaBuf);
    }
    return capture::frameMemoryLabel(telemetry.frameMemory);
}

namespace {

void logCaptureStartup(const char *stage, const QString &detail = {})
{
    std::cout << "[MainWindow capture] " << stage;
    if (!detail.isEmpty()) {
        std::cout << ": " << detail.toStdString();
    }
    std::cout << std::endl;
    std::cout.flush();
}

int xioctl(const int fd, const unsigned long request, void *arg)
{
    int result = 0;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

quint32 stringToFourCc(const QString &value)
{
    const auto latin = value.toLatin1();
    if (latin.size() < 4) {
        return 0;
    }
    return v4l2_fourcc(latin[0], latin[1], latin[2], latin[3]);
}

QString deviceListSignature(const std::vector<capture::CaptureDevice> &devices)
{
    QStringList entries;
    for (const auto &device : devices) {
        QStringList formats;
        for (const auto &format : device.formats) {
            formats << QStringLiteral("%1x%2/%3/%4")
                           .arg(format.width)
                           .arg(format.height)
                           .arg(format.framesPerSecond, 0, 'f', 2)
                           .arg(format.pixelFormat);
        }
        entries << QStringLiteral("%1|%2|%3")
                       .arg(device.stableId, device.devicePath, formats.join(QLatin1Char(',')));
    }
    return entries.join(QLatin1Char(';'));
}

bool isExpectedDeviceRemovalMessage(const QString &message)
{
    return message.contains(QStringLiteral("No such device"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("No such file or directory"), Qt::CaseInsensitive);
}

constexpr auto accentColor = "#CC11BB";
constexpr auto panelStyle = R"(
    QFrame#startupPanel {
        background-color: rgba(16, 0, 24, 64);
        border: 1px solid rgba(255, 255, 255, 85);
        border-radius: 18px;
    }
)";
constexpr auto comboStyle = R"(
    QComboBox {
        color: white;
        background: transparent;
        border: none;
        border-bottom: 2px solid rgba(255, 255, 255, 190);
        padding: 6px 28px 7px 0;
        font-size: 18px;
    }
    QComboBox::drop-down {
        border: none;
        width: 24px;
    }
    QComboBox QAbstractItemView {
        color: white;
        background-color: #250019;
        selection-background-color: #CC11BB;
    }
)";
constexpr auto startupMenuButtonStyle = R"(
    QPushButton {
        color: white;
        background: transparent;
        border: none;
        border-bottom: 2px solid rgba(255, 255, 255, 190);
        padding: 6px 28px 7px 0;
        font-size: 18px;
        text-align: left;
    }
    QPushButton:disabled {
        color: rgba(255, 255, 255, 120);
        border-bottom-color: rgba(255, 255, 255, 90);
    }
    QPushButton::menu-indicator {
        subcontrol-origin: padding;
        subcontrol-position: center right;
    }
    QMenu {
        color: white;
        background-color: #250019;
        border: 1px solid rgba(255, 255, 255, 80);
    }
    QMenu::item {
        font-size: 18px;
        padding: 5px 42px 5px 16px;
    }
    QMenu::item:selected {
        background-color: #CC11BB;
    }
    QMenu::separator {
        height: 1px;
        background: rgba(255, 255, 255, 90);
        margin: 8px 12px;
    }
)";
constexpr auto pillButtonStyle = R"(
    QPushButton {
        color: white;
        background-color: rgba(0, 0, 0, 38);
        border: 1px solid rgba(255, 255, 255, 145);
        border-radius: 18px;
        padding: 9px 18px;
        font-weight: 700;
        min-height: 18px;
    }
    QPushButton:hover {
        border-color: #CC11BB;
        background-color: rgba(204, 17, 187, 48);
    }
)";
constexpr auto dialogStyle = R"(
    QDialog {
        background-color: #191919;
        color: white;
    }
    QLabel {
        color: white;
    }
    QTextEdit {
        color: white;
        background-color: rgba(255, 255, 255, 26);
        border: 1px solid rgba(255, 255, 255, 55);
        border-radius: 8px;
        padding: 10px;
        font-family: monospace;
    }
    QPushButton {
        color: white;
        background-color: rgba(255, 255, 255, 20);
        border: 1px solid rgba(255, 255, 255, 120);
        border-radius: 16px;
        padding: 7px 14px;
        font-weight: 700;
    }
    QPushButton:hover {
        border-color: #CC11BB;
        background-color: rgba(204, 17, 187, 48);
    }
)";
constexpr auto playbackButtonStyle = R"(
    QPushButton {
        background-color: rgba(255, 255, 255, 22);
        border: none;
        border-radius: 32px;
    }
    QPushButton:hover {
        background-color: rgba(255, 255, 255, 42);
    }
)";
constexpr auto playbackSliderStyle = R"(
    QSlider::groove:horizontal {
        height: 8px;
        background: rgba(255, 255, 255, 145);
        border-radius: 4px;
    }
    QSlider::sub-page:horizontal {
        background: #CC11BB;
        border-radius: 4px;
    }
    QSlider::handle:horizontal {
        background: #CC11BB;
        border: 6px solid #4a4a4f;
        width: 18px;
        height: 18px;
        margin: -11px 0;
        border-radius: 15px;
    }
)";
constexpr bool showVideoStatsOverlay = true;
constexpr bool showAdvancedVideoStats = true;

QPixmap renderIconPixmap(const QString &resourcePath, const QColor &color, const int size)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    auto svg = QString::fromUtf8(file.readAll());
    svg.replace(QStringLiteral("currentColor"), color.name(QColor::HexRgb));

    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    renderer.render(&painter);
    painter.end();
    return pixmap;
}

class GradientBackground final : public QWidget {
public:
    explicit GradientBackground(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        QLinearGradient gradient(rect().topLeft(), rect().bottomLeft());
        gradient.setColorAt(0.0, QColor("#8C0573"));
        gradient.setColorAt(0.48, QColor("#470330"));
        gradient.setColorAt(1.0, QColor("#000000"));
        painter.fillRect(rect(), gradient);
    }
};

QLabel *makePlaceholderIcon(QWidget *parent)
{
    auto *icon = new QLabel(parent);
    icon->setFixedSize(64, 64);
    icon->setPixmap(createAppIcon().pixmap(64, 64));
    icon->setScaledContents(true);
    icon->setStyleSheet(QStringLiteral("border-radius: 16px;"));
    return icon;
}

QLabel *makeFieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setStyleSheet(QStringLiteral("color: white; font-size: 17px; font-weight: 700;"));
    return label;
}

QFrame *makeDivider(QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 65);"));
    return divider;
}

QComboBox *makeStartupCombo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->setStyleSheet(QString::fromUtf8(comboStyle));
    combo->setMinimumWidth(320);
    combo->setMaximumWidth(340);
    return combo;
}

QPushButton *makeStartupMenuButton(QWidget *parent)
{
    auto *button = new QPushButton(parent);
    button->setStyleSheet(QString::fromUtf8(startupMenuButtonStyle));
    button->setMinimumWidth(320);
    button->setMaximumWidth(340);
    return button;
}

QString frameRateLabel(const double fps)
{
    const auto rounded = std::round(fps);
    if (std::abs(fps - rounded) <= 0.01) {
        return QStringLiteral("%1 fps").arg(static_cast<int>(rounded));
    }
    return QStringLiteral("%1 fps").arg(QString::number(fps, 'f', 2));
}

int resolutionPreferenceIndex(const int width, const int height)
{
    static constexpr std::array<std::pair<int, int>, 4> preferred {{
        {3840, 2160},
        {2560, 1440},
        {1920, 1080},
        {1280, 720},
    }};

    for (auto index = 0U; index < preferred.size(); ++index) {
        if (preferred[index].first == width && preferred[index].second == height) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int pixelFormatPreference(const QString &pixelFormat)
{
    const auto upper = pixelFormat.toUpper();
    if (upper == QStringLiteral("NV12")) {
        return 0;
    }
    if (upper == QStringLiteral("YUYV")) {
        return 1;
    }
    if (upper == QStringLiteral("YUY2")) {
        return 2;
    }
    if (upper == QStringLiteral("MJPG") || upper == QStringLiteral("MJPEG") || upper == QStringLiteral("JPEG")) {
        return 4;
    }
    static const QSet<QString> uncompressed {
        QStringLiteral("I420"),
        QStringLiteral("YV12"),
        QStringLiteral("YU12"),
        QStringLiteral("YVU420"),
        QStringLiteral("RGB3"),
        QStringLiteral("BGR3"),
        QStringLiteral("RGB24"),
        QStringLiteral("BGR24"),
    };
    if (uncompressed.contains(upper)) {
        return 3;
    }
    return 5;
}

bool formatIsPreferredDefault(const capture::CaptureFormat &format)
{
    return format.width == 1920 &&
        format.height == 1080 &&
        std::abs(format.framesPerSecond - 60.0) <= 0.5 &&
        (format.pixelFormat == QStringLiteral("NV12") ||
         format.pixelFormat == QStringLiteral("YUYV") ||
         format.pixelFormat == QStringLiteral("YUY2"));
}

QString formatLeafLabel(const capture::CaptureFormat &format)
{
    return format.pixelFormat.isEmpty() ? format.label : format.pixelFormat;
}

QPushButton *makePillButton(const QIcon &icon, const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(icon, text, parent);
    button->setIconSize(QSize(18, 18));
    button->setFixedHeight(38);
    button->setStyleSheet(QString::fromUtf8(pillButtonStyle));
    return button;
}

QFrame *makeBarDivider(QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedHeight(38);
    divider->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 70);"));
    return divider;
}

void setPlaybackButtonPixmap(QPushButton *button, const QPixmap &pixmap)
{
    if (button == nullptr) {
        return;
    }

    auto *iconLabel = button->findChild<QLabel *>(QStringLiteral("playbackButtonIcon"));
    if (iconLabel == nullptr) {
        return;
    }

    iconLabel->setPixmap(pixmap);
}

QPushButton *makePlaybackCircleButton(const QPixmap &pixmap, QWidget *parent)
{
    auto *button = new QPushButton(parent);
    button->setFixedSize(36, 36);
    button->setStyleSheet(QString::fromUtf8(playbackButtonStyle));

    auto *layout = new QVBoxLayout(button);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *iconLabel = new QLabel(button);
    iconLabel->setObjectName(QStringLiteral("playbackButtonIcon"));
    iconLabel->setFixedSize(24, 24);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setPixmap(pixmap);
    iconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(iconLabel, 0, Qt::AlignCenter);

    return button;
}

QLabel *makePlaybackIconLabel(const QPixmap &pixmap, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setFixedSize(24, 24);
    label->setPixmap(pixmap);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}

void enableMouseTrackingTree(QWidget *widget)
{
    if (widget == nullptr) {
        return;
    }

    widget->setMouseTracking(true);
    const auto children = widget->findChildren<QWidget *>();
    for (auto *child : children) {
        child->setMouseTracking(true);
    }
}

QSlider *makePlaybackSlider(QWidget *parent)
{
    auto *slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(0, 100);
    slider->setFixedWidth(170);
    slider->setStyleSheet(QString::fromUtf8(playbackSliderStyle));
    return slider;
}

void addInfoRow(QVBoxLayout *layout, QWidget *parent, const QString &symbol, const QString &title, const QString &body)
{
    auto *row = new QHBoxLayout();
    row->setSpacing(14);

    auto *icon = new QLabel(symbol, parent);
    icon->setFixedWidth(32);
    icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    icon->setStyleSheet(QStringLiteral("color: %1; font-size: 22px; font-weight: 700;").arg(accentColor));

    auto *text = new QLabel(parent);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setText(QStringLiteral("<b>%1</b><br>%2").arg(title.toHtmlEscaped(), body.toHtmlEscaped()));
    text->setStyleSheet(QStringLiteral("color: white; font-size: 14px; line-height: 1.3;"));

    row->addWidget(icon);
    row->addWidget(text, 1);
    layout->addLayout(row);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<capture::VideoTelemetrySnapshot>("consolation::capture::VideoTelemetrySnapshot");
    qRegisterMetaType<capture::DmaBufFrameHandle>("consolation::capture::DmaBufFrameHandle");

    setWindowTitle(QStringLiteral("%1 %2").arg(
        QString::fromUtf8(consolation::app::AppMetadata::displayName),
        QString::fromUtf8(consolation::app::BuildInfo::releaseVersion)));
    setWindowIcon(createAppIcon());
    resize(1200, 760);
    setMinimumSize(820, 520);

    controlsHideTimer_ = new QTimer(this);
    controlsHideTimer_->setSingleShot(true);
    controlsHideTimer_->setInterval(3000);
    connect(controlsHideTimer_, &QTimer::timeout, this, [this]() { hidePlaybackControls(); });

    statsOverlayTimer_ = new QTimer(this);
    statsOverlayTimer_->setInterval(500);
    connect(statsOverlayTimer_, &QTimer::timeout, this, [this]() { updateStatsOverlay(); });

    startupRefreshTimer_ = new QTimer(this);
    startupRefreshTimer_->setInterval(3000);
    connect(startupRefreshTimer_, &QTimer::timeout, this, [this]() { refreshStartupDevices(); });

    buildStoppedState();

    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    uninhibitScreenSaver();
    if (captureSession_ && captureThread_ && captureThread_->isRunning()) {
        QMetaObject::invokeMethod(
            captureSession_.get(), &capture::CaptureSession::stop, Qt::BlockingQueuedConnection);
    }
    if (captureThread_) {
        captureThread_->quit();
        captureThread_->wait();
        delete captureThread_;
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (playbackControls_ && event->type() == QEvent::MouseMove) {
        if (auto *widget = qobject_cast<QWidget *>(watched); widget != nullptr) {
            if (widget == this || isAncestorOf(widget)) {
                showPlaybackControls();
                resetPlaybackControlsTimer();
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::buildStoppedState()
{
    playbackStopping_.store(false, std::memory_order_release);
    playbackControls_.clear();
    statsOverlay_.clear();
    latestTelemetry_ = {};
    cachedStatsOverlayText_.clear();
    displayPath_ = capture::VideoDisplayPath::Unknown;
    uiFps_ = 0.0;
    paintFps_ = 0.0;
    if (statsOverlayTimer_ != nullptr) {
        statsOverlayTimer_->stop();
    }
    if (controlsHideTimer_ != nullptr) {
        controlsHideTimer_->stop();
    }
    devices_ = capture::CaptureBackendManager().enumerateDevices();
    const auto hasDevices = !devices_.empty();

    auto *root = new GradientBackground(this);
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(24, 24, 24, 18);
    rootLayout->setSpacing(0);

    rootLayout->addStretch(1);

    auto *panel = new QFrame(root);
    panel->setObjectName(QStringLiteral("startupPanel"));
    panel->setStyleSheet(QString::fromUtf8(panelStyle));
    panel->setMaximumWidth(660);
    panel->setMinimumWidth(560);

    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(28, 28, 28, 28);
    panelLayout->setSpacing(20);

    auto *header = new QHBoxLayout();
    header->setSpacing(14);
    header->addWidget(makePlaceholderIcon(panel));

    auto *title = new QLabel(QStringLiteral("Consolation"), panel);
    title->setStyleSheet(QStringLiteral("color: white; font-size: 31px; font-weight: 800;"));
    header->addWidget(title);

    auto *version = new QLabel(
        QStringLiteral("v%1").arg(QString::fromUtf8(consolation::app::BuildInfo::releaseVersion)),
        panel);
    version->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180); font-size: 18px;"));
    header->addWidget(version);
    header->addStretch();
    panelLayout->addLayout(header);
    panelLayout->addWidget(makeDivider(panel));

    auto *form = new QGridLayout();
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(30);
    form->setColumnMinimumWidth(0, 250);

    auto *deviceCombo = makeStartupCombo(panel);
    auto *formatButton = makeStartupMenuButton(panel);

    if (hasDevices) {
        for (auto index = 0; index < static_cast<int>(devices_.size()); ++index) {
            const auto &device = devices_[index];
            deviceCombo->addItem(
                QStringLiteral("%1 (%2)").arg(device.displayName, device.devicePath),
                index);
        }
    } else {
        deviceCombo->addItem(QStringLiteral("No Capture Cards Detected"), -1);
        formatButton->setText(QStringLiteral("No Capture Cards Detected"));
        deviceCombo->setEnabled(false);
        formatButton->setEnabled(false);
        selectedDevice_ = {};
        selectedFormat_ = {};
    }

    form->addWidget(makeFieldLabel(QStringLiteral("Device"), panel), 0, 0);
    form->addWidget(deviceCombo, 0, 1);

    form->addWidget(makeFieldLabel(QStringLiteral("Frame Rate & Resolution"), panel), 1, 0);
    form->addWidget(formatButton, 1, 1);

    panelLayout->addLayout(form);

    auto *playButton = new QPushButton(panel);
    playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    playButton->setIconSize(QSize(42, 42));
    playButton->setFixedSize(72, 72);
    playButton->setEnabled(false);
    playButton->setToolTip(QStringLiteral("Start playback."));
    playButton->setStyleSheet(QStringLiteral(
        "QPushButton { color: white; border-radius: 36px; border: none; "
        "background-color: rgba(255, 255, 255, 85); }"
        "QPushButton:disabled { background-color: rgba(255, 255, 255, 34); }"));
    connect(playButton, &QPushButton::clicked, this, [this, playButton]() {
        const auto deviceIndex = selectedDevice_.devicePath.isEmpty() ? -1 : 0;
        Q_UNUSED(deviceIndex);
        playButton->setEnabled(false);
        startPlayback();
    });
    const auto selectFormat = [this, formatButton, playButton](const int formatIndex) {
        if (selectedDevice_.devicePath.isEmpty() ||
            formatIndex < 0 ||
            formatIndex >= static_cast<int>(selectedDevice_.formats.size())) {
            selectedFormat_ = {};
            formatButton->setText(QStringLiteral("Select Format"));
            playButton->setEnabled(false);
            return;
        }

        selectedFormat_ = selectedDevice_.formats[formatIndex];
        formatButton->setText(selectedFormat_.label);
        playButton->setEnabled(true);
        QTimer::singleShot(0, this, [this]() { preconfigureSelectedFormat(); });
    };
    const auto rebuildFormatMenu = [this, deviceCombo, formatButton, playButton, selectFormat]() {
        const auto deviceIndex = deviceCombo->currentData().toInt();
        if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices_.size())) {
            selectedDevice_ = {};
            selectedFormat_ = {};
            if (auto *oldMenu = formatButton->menu(); oldMenu != nullptr) {
                formatButton->setMenu(nullptr);
                oldMenu->deleteLater();
            }
            formatButton->setText(QStringLiteral("No Capture Cards Detected"));
            formatButton->setEnabled(false);
            playButton->setEnabled(false);
            return;
        }

        selectedDevice_ = devices_[deviceIndex];
        formatButton->setEnabled(!selectedDevice_.formats.empty());
        playButton->setEnabled(false);

        auto *menu = new QMenu(formatButton);

        std::vector<std::pair<int, int>> resolutions;
        std::set<std::pair<int, int>> seenResolutions;
        for (const auto &format : selectedDevice_.formats) {
            const auto key = std::make_pair(format.width, format.height);
            if (seenResolutions.insert(key).second) {
                resolutions.push_back(key);
            }
        }
        std::sort(resolutions.begin(), resolutions.end(), [](const auto &lhs, const auto &rhs) {
            const auto lhsPreferred = resolutionPreferenceIndex(lhs.first, lhs.second);
            const auto rhsPreferred = resolutionPreferenceIndex(rhs.first, rhs.second);
            if (lhsPreferred >= 0 || rhsPreferred >= 0) {
                if (lhsPreferred < 0) {
                    return false;
                }
                if (rhsPreferred < 0) {
                    return true;
                }
                return lhsPreferred < rhsPreferred;
            }
            if (lhs.first != rhs.first) {
                return lhs.first > rhs.first;
            }
            return lhs.second > rhs.second;
        });

        auto addedDivider = false;
        auto defaultFormatIndex = -1;
        for (const auto &[width, height] : resolutions) {
            if (!addedDivider && resolutionPreferenceIndex(width, height) < 0 && !menu->isEmpty()) {
                menu->addSeparator();
                addedDivider = true;
            }

            auto *resolutionMenu = menu->addMenu(QStringLiteral("%1x%2").arg(width).arg(height));

            std::vector<int> frameRateKeys;
            std::set<int> seenFrameRates;
            for (auto formatIndex = 0; formatIndex < static_cast<int>(selectedDevice_.formats.size()); ++formatIndex) {
                const auto &format = selectedDevice_.formats[formatIndex];
                if (format.width != width || format.height != height) {
                    continue;
                }
                const auto fpsKey = static_cast<int>(std::round(format.framesPerSecond * 1000.0));
                if (seenFrameRates.insert(fpsKey).second) {
                    frameRateKeys.push_back(fpsKey);
                }
                if ((defaultFormatIndex < 0 && formatIsPreferredDefault(format)) ||
                    (defaultFormatIndex >= 0 &&
                     formatIsPreferredDefault(format) &&
                     pixelFormatPreference(format.pixelFormat) < pixelFormatPreference(selectedDevice_.formats[defaultFormatIndex].pixelFormat))) {
                    defaultFormatIndex = formatIndex;
                }
            }
            std::sort(frameRateKeys.begin(), frameRateKeys.end(), std::greater<>());

            for (const auto fpsKey : frameRateKeys) {
                const auto fps = static_cast<double>(fpsKey) / 1000.0;
                auto *frameRateMenu = resolutionMenu->addMenu(frameRateLabel(fps));

                std::vector<int> formatIndices;
                for (auto formatIndex = 0; formatIndex < static_cast<int>(selectedDevice_.formats.size()); ++formatIndex) {
                    const auto &format = selectedDevice_.formats[formatIndex];
                    if (format.width == width &&
                        format.height == height &&
                        static_cast<int>(std::round(format.framesPerSecond * 1000.0)) == fpsKey) {
                        formatIndices.push_back(formatIndex);
                    }
                }
                std::sort(formatIndices.begin(), formatIndices.end(), [this](const auto lhs, const auto rhs) {
                    const auto &lhsFormat = selectedDevice_.formats[lhs];
                    const auto &rhsFormat = selectedDevice_.formats[rhs];
                    const auto lhsPreference = pixelFormatPreference(lhsFormat.pixelFormat);
                    const auto rhsPreference = pixelFormatPreference(rhsFormat.pixelFormat);
                    if (lhsPreference != rhsPreference) {
                        return lhsPreference < rhsPreference;
                    }
                    return lhsFormat.pixelFormat < rhsFormat.pixelFormat;
                });

                for (const auto formatIndex : formatIndices) {
                    auto *action = frameRateMenu->addAction(formatLeafLabel(selectedDevice_.formats[formatIndex]));
                    connect(action, &QAction::triggered, this, [selectFormat, formatIndex]() { selectFormat(formatIndex); });
                }
            }
        }

        if (defaultFormatIndex < 0 && !selectedDevice_.formats.empty()) {
            defaultFormatIndex = 0;
        }

        if (auto *oldMenu = formatButton->menu(); oldMenu != nullptr) {
            formatButton->setMenu(nullptr);
            oldMenu->deleteLater();
        }
        formatButton->setMenu(menu);
        if (defaultFormatIndex >= 0) {
            selectFormat(defaultFormatIndex);
        } else {
            formatButton->setText(QStringLiteral("No Formats Advertised"));
            formatButton->setEnabled(false);
            playButton->setEnabled(false);
        }
    };
    connect(
        deviceCombo,
        &QComboBox::currentIndexChanged,
        this,
        [rebuildFormatMenu](int) {
            rebuildFormatMenu();
        });
    rebuildFormatMenu();
    if (hasDevices) {
        QTimer::singleShot(0, this, [this]() { preconfigureSelectedFormat(true); });
    }
    panelLayout->addWidget(playButton, 0, Qt::AlignCenter);

    rootLayout->addWidget(panel, 0, Qt::AlignHCenter);
    rootLayout->addStretch(1);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(14);
    buttonRow->addStretch();

    auto *settingsButton = makePillButton(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView),
        QStringLiteral("Settings"),
        root);

    auto *helpButton = makePillButton(
        style()->standardIcon(QStyle::SP_MessageBoxQuestion),
        QStringLiteral("Help"),
        root);
    auto *aboutButton = makePillButton(
        style()->standardIcon(QStyle::SP_MessageBoxInformation),
        QStringLiteral("About"),
        root);

    connect(settingsButton, &QPushButton::clicked, this, [this]() { showSettingsDialog(); });
    connect(helpButton, &QPushButton::clicked, this, [this]() { showHelpDialog(); });
    connect(aboutButton, &QPushButton::clicked, this, [this]() { showAboutDialog(); });

    buttonRow->addWidget(settingsButton);
    buttonRow->addWidget(helpButton);
    buttonRow->addWidget(aboutButton);
    rootLayout->addLayout(buttonRow);

    setCentralWidget(root);
    if (startupRefreshTimer_ != nullptr) {
        startupRefreshTimer_->start();
    }
}

void MainWindow::startPlayback()
{
    if (playbackStopping_.load(std::memory_order_acquire)) {
        return;
    }
    if (selectedDevice_.devicePath.isEmpty()) {
        return;
    }

    showConnectingState();
    inhibitScreenSaver();
    if (selectedDevice_.backend == capture::CaptureBackend::V4L2) {
        audioSession_ = std::make_unique<platform::linux::PipeWireAudioSession>();
        if (!audioSession_->start(selectedDevice_, settings_.volumePercent())) {
            audioSession_.reset();
        }
    }

    // OBS tears down the active V4L2 source before opening/configuring it again.
    // Do the same here before opening the selected device for the new session.
    if (captureSession_) {
        if (captureThread_ && captureThread_->isRunning()) {
            QMetaObject::invokeMethod(
                captureSession_.get(), &capture::CaptureSession::stop, Qt::BlockingQueuedConnection);
        } else {
            captureSession_->stop();
        }
        captureSession_.reset();
    }
    if (captureThread_) {
        captureThread_->quit();
        captureThread_->wait();
        delete captureThread_;
        captureThread_ = nullptr;
    }

    if (selectedDevice_.devicePath.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Capture Failed"),
            QStringLiteral("No capture device is selected."));
        stopPlayback();
        return;
    }

    captureSession_ = capture::CaptureBackendManager().createSession(selectedDevice_.backend);
    if (!captureSession_) {
        QMessageBox::warning(
            this,
            QStringLiteral("Capture Failed"),
            QStringLiteral("No capture backend is available for the selected device yet."));
        stopPlayback();
        return;
    }

    // Move the session to a dedicated thread so the blocking USB-reset recovery in start()
    // doesn't freeze the UI event loop. Signals from the session to main-thread slots are
    // automatically delivered as queued connections across the thread boundary.
    logCaptureStartup(
        "creating capture thread",
        QStringLiteral("%1 @ %2").arg(selectedDevice_.displayName, selectedFormat_.label));
    captureThread_ = new QThread();
    captureSession_->moveToThread(captureThread_);
    logCaptureStartup("session moved to capture thread");

    connect(
        captureSession_.get(),
        &capture::CaptureSession::frameReady,
        this,
        [this](capture::FrameHandle frame, const qint64 capturedAtNs) {
            if (playbackStopping_.load(std::memory_order_acquire)) {
                return;
            }
            if (!frame || frame->isNull()) {
                return;
            }
            if (!videoSurface_) {
                showPlaybackState(std::move(frame));
                return;
            }
            updateVideoFrame(std::move(frame), capturedAtNs);
        });
    connect(
        captureSession_.get(),
        &capture::CaptureSession::dmaFrameReady,
        this,
        [this](capture::DmaBufFrameHandle frame) {
            if (playbackStopping_.load(std::memory_order_acquire)) {
                return;
            }
            if (!frame) {
                return;
            }
            if (auto *surface = dmaVideoSurface_.load(std::memory_order_acquire)) {
                surface->submitDmaFrame(std::move(frame));
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, frame = std::move(frame)]() mutable {
                    if (playbackStopping_.load(std::memory_order_acquire)) {
                        return;
                    }
                    if (!videoSurface_) {
                        showPlaybackState({});
                    }
                    if (auto *surface = dmaVideoSurface_.load(std::memory_order_acquire)) {
                        surface->submitDmaFrame(std::move(frame));
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);
    connect(captureSession_.get(), &capture::CaptureSession::failed, this, [this](const QString &message) {
        if (playbackStopping_.load(std::memory_order_acquire)) {
            return;
        }
        logCaptureStartup("failed", message);
        if (!isExpectedDeviceRemovalMessage(message)) {
            QMessageBox::warning(this, QStringLiteral("Capture Failed"), message);
        }
        stopPlayback();
    });
    connect(captureSession_.get(), &capture::CaptureSession::logMessage, this, [](const QString &message) {
        logCaptureStartup("session", message);
    });
    connect(captureSession_.get(), &capture::CaptureSession::telemetryReady, this, [this](const capture::VideoTelemetrySnapshot &snapshot) {
        latestTelemetry_ = snapshot;
        refreshStatsOverlayCache();
    });

    const auto deviceSnapshot = selectedDevice_;
    const auto formatSnapshot = selectedFormat_;
    const auto dmaDisplayRequested = deviceSnapshot.backend == capture::CaptureBackend::V4L2 &&
        pixelFormatSupportsDmaDisplay(formatSnapshot.pixelFormat) && canCreateOpenGLContext();
    if (auto *v4l2 = dynamic_cast<platform::linux::V4L2CaptureSession *>(captureSession_.get())) {
        v4l2->setDmaBufDisplayRequested(dmaDisplayRequested);
    }
    logCaptureStartup(
        "queuing CaptureSession::start",
        QStringLiteral("%1 dma=%2").arg(
            QStringLiteral("%1 (%2)").arg(deviceSnapshot.devicePath, formatSnapshot.label),
            dmaDisplayRequested ? QStringLiteral("yes") : QStringLiteral("no")));
    QMetaObject::invokeMethod(
        captureSession_.get(),
        [this, deviceSnapshot, formatSnapshot, dmaDisplayRequested]() {
            logCaptureStartup(
                "CaptureSession::start on worker thread",
                QStringLiteral("thread=%1").arg(
                    QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16)));
            if (auto *v4l2 = qobject_cast<platform::linux::V4L2CaptureSession *>(captureSession_.get())) {
                v4l2->setDmaBufDisplayRequested(dmaDisplayRequested);
            }
            const auto started = captureSession_->start(deviceSnapshot, formatSnapshot);
            logCaptureStartup("CaptureSession::start finished", started ? QStringLiteral("ok") : QStringLiteral("failed"));
        },
        Qt::QueuedConnection);

    captureThread_->start();
    logCaptureStartup("capture thread started");

    auto *sessionPtr = captureSession_.get();
    QTimer::singleShot(10000, this, [this, sessionPtr]() {
        if (captureSession_.get() == sessionPtr && !videoSurface_) {
            logCaptureStartup("timed out waiting for first frame");
            QMessageBox::warning(
                this,
                QStringLiteral("Capture Failed"),
                QStringLiteral("Timed out waiting for capture frames."));
            stopPlayback();
        }
    });
    logCaptureStartup("started 10s first-frame timeout");
}

void MainWindow::showConnectingState()
{
    if (startupRefreshTimer_ != nullptr) {
        startupRefreshTimer_->stop();
    }
    playbackControls_.clear();
    statsOverlay_.clear();
    latestTelemetry_ = {};
    cachedStatsOverlayText_.clear();
    displayPath_ = capture::VideoDisplayPath::Unknown;
    uiFps_ = 0.0;
    paintFps_ = 0.0;
    if (statsOverlayTimer_ != nullptr) {
        statsOverlayTimer_->stop();
    }
    if (controlsHideTimer_ != nullptr) {
        controlsHideTimer_->stop();
    }

    auto *root = new QWidget(this);
    root->setStyleSheet(QStringLiteral("background-color: black;"));

    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(24, 24, 24, 24);

    auto *label = new QLabel(QStringLiteral("Connecting to Capture Card..."), root);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: #808080; font-size: 32px; font-weight: 500;"));

    layout->addWidget(label, 1);
    setCentralWidget(root);
}

void MainWindow::showStoppingState()
{
    if (startupRefreshTimer_ != nullptr) {
        startupRefreshTimer_->stop();
    }
    playbackControls_.clear();
    statsOverlay_.clear();
    dmaVideoSurface_.store(nullptr, std::memory_order_release);
    videoSurface_.clear();
    latestTelemetry_ = {};
    cachedStatsOverlayText_.clear();
    displayPath_ = capture::VideoDisplayPath::Unknown;
    uiFps_ = 0.0;
    paintFps_ = 0.0;
    if (statsOverlayTimer_ != nullptr) {
        statsOverlayTimer_->stop();
    }
    if (controlsHideTimer_ != nullptr) {
        controlsHideTimer_->stop();
    }

    auto *root = new QWidget(this);
    root->setStyleSheet(QStringLiteral("background-color: black;"));

    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(24, 24, 24, 24);

    auto *label = new QLabel(QStringLiteral("Stopping Playback..."), root);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: #808080; font-size: 32px; font-weight: 500;"));

    layout->addWidget(label, 1);
    setCentralWidget(root);
}

void MainWindow::showPlaybackState(capture::FrameHandle firstFrame)
{
    if (startupRefreshTimer_ != nullptr) {
        startupRefreshTimer_->stop();
    }
    auto *root = new QWidget(this);
    root->setStyleSheet(QStringLiteral("background-color: black;"));

    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *video = new VideoSurface(root);
    video->setStartupOverlayVisible(!firstFrame || firstFrame->isNull());
    videoSurface_ = video;
    dmaVideoSurface_.store(video, std::memory_order_release);
    video->setFramePresentedHandler([this](const qint64 latencyNs) { recordPresentLatency(latencyNs); });
    video->setDmaCpuFallbackHandler([this](capture::DmaBufFrameHandle frame) {
        if (!captureSession_ || !captureThread_ || !captureThread_->isRunning() || !frame) {
            return;
        }
        auto *session = captureSession_.get();
        QMetaObject::invokeMethod(
            session,
            [session, frame = std::move(frame)]() mutable {
                if (auto *v4l2 = qobject_cast<platform::linux::V4L2CaptureSession *>(session)) {
                    v4l2->finishDmaFrameAsCpu(std::move(frame));
                }
            },
            Qt::BlockingQueuedConnection);
    });

    auto *statsOverlay = new QLabel(video);
    statsOverlay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    statsOverlay->setWordWrap(false);
    statsOverlay->setMargin(10);
    statsOverlay->setStyleSheet(QStringLiteral(
        "QLabel { color: white; background-color: rgba(0, 0, 0, 170); "
        "border: 1px solid rgba(255, 255, 255, 80); border-radius: 6px; "
        "font-family: monospace; font-size: 13px; }"));
    statsOverlay->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    statsOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    statsOverlay_ = statsOverlay;
    video->setOverlay(statsOverlay);
    updateStatsOverlay();

    layout->addWidget(video, 1);

    auto *controls = new QFrame(video);
    controls->setFixedHeight(58);
    controls->setStyleSheet(QStringLiteral(
        "QFrame { background-color: rgba(34, 34, 34, 221); "
        "border: 1px solid rgba(68, 68, 68, 238); border-radius: 29px; }"));

    auto *controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(12, 4, 12, 4);
    controlsLayout->setSpacing(10);

    const auto whiteIcon = QColor(Qt::white);
    const auto powerIcon = renderIconPixmap(QStringLiteral(":/icons/power.svg"), QColor(QStringLiteral("#ff453a")), 24);
    const auto volumeOnIcon = renderIconPixmap(QStringLiteral(":/icons/volume-2.svg"), whiteIcon, 24);
    const auto volumeOffIcon = renderIconPixmap(QStringLiteral(":/icons/volume-off.svg"), whiteIcon, 24);
    const auto zoomOutIcon = renderIconPixmap(QStringLiteral(":/icons/zoom-out.svg"), whiteIcon, 22);
    const auto zoomInIcon = renderIconPixmap(QStringLiteral(":/icons/zoom-in.svg"), whiteIcon, 22);
    const auto settingsIcon = renderIconPixmap(QStringLiteral(":/icons/settings.svg"), whiteIcon, 24);

    auto *powerButton = makePlaybackCircleButton(powerIcon, controls);
    auto *volumeButton = makePlaybackCircleButton(volumeOnIcon, controls);
    auto *zoomOut = makePlaybackIconLabel(zoomOutIcon, controls);
    auto *zoomIn = makePlaybackIconLabel(zoomInIcon, controls);
    auto *settingsButton = makePlaybackCircleButton(settingsIcon, controls);

    auto *volumeSlider = makePlaybackSlider(controls);
    volumeSlider->setValue(settings_.volumePercent());
    auto *zoomSlider = makePlaybackSlider(controls);
    zoomSlider->setValue(0);

    connect(powerButton, &QPushButton::clicked, this, [this, powerButton]() {
        powerButton->setEnabled(false);
        stopPlaybackAsync();
    });
    connect(settingsButton, &QPushButton::clicked, this, [this]() { showSettingsDialog(); });
    connect(volumeSlider, &QSlider::valueChanged, this, [this](const int value) {
        settings_.setVolumePercent(value);
        if (audioSession_) {
            audioSession_->setVolumePercent(value);
        }
        resetPlaybackControlsTimer();
    });
    connect(volumeSlider, &QSlider::valueChanged, volumeButton, [volumeButton, volumeOnIcon, volumeOffIcon](const int value) {
        setPlaybackButtonPixmap(volumeButton, value <= 0 ? volumeOffIcon : volumeOnIcon);
    });
    setPlaybackButtonPixmap(volumeButton, volumeSlider->value() <= 0 ? volumeOffIcon : volumeOnIcon);

    controlsLayout->addWidget(powerButton);
    controlsLayout->addWidget(makeBarDivider(controls));
    controlsLayout->addWidget(volumeButton);
    controlsLayout->addWidget(volumeSlider);
    controlsLayout->addWidget(makeBarDivider(controls));
    controlsLayout->addWidget(zoomOut);
    controlsLayout->addWidget(zoomSlider);
    controlsLayout->addWidget(zoomIn);
    controlsLayout->addWidget(makeBarDivider(controls));
    controlsLayout->addWidget(settingsButton);

    playbackControls_ = controls;
    video->setControlsOverlay(controls);
    enableMouseTrackingTree(root);
    setCentralWidget(root);
    if (firstFrame && !firstFrame->isNull()) {
        updateVideoFrame(std::move(firstFrame));
    }
    if (statsOverlayTimer_ != nullptr && showVideoStatsOverlay) {
        statsOverlayTimer_->start();
    }
    showPlaybackControls();
    resetPlaybackControlsTimer();
}

void MainWindow::updateVideoFrame(capture::FrameHandle frame, const qint64 capturedAtNs)
{
    if (playbackStopping_.load(std::memory_order_acquire)) {
        return;
    }
    if (!videoSurface_ || !frame || frame->isNull()) {
        return;
    }

    videoSurface_->setPendingFrame(std::move(frame), capturedAtNs);
}

void MainWindow::recordPresentLatency(const qint64 latencyNs)
{
    if (latencyNs <= 0) {
        return;
    }

    constexpr qint64 windowNs = 500'000'000;
    const auto nowNs = capture::monotonicClockNs();
    if (presentLagWindowStartNs_ == 0) {
        presentLagWindowStartNs_ = nowNs;
    }

    ++presentLagSampleCount_;
    presentLagTotalNs_ += latencyNs;

    const auto elapsedNs = nowNs - presentLagWindowStartNs_;
    if (elapsedNs < windowNs) {
        return;
    }

    presentLagAvgMs_ = presentLagSampleCount_ > 0
        ? static_cast<double>(presentLagTotalNs_) / static_cast<double>(presentLagSampleCount_) / 1'000'000.0
        : 0.0;
    presentLagWindowStartNs_ = nowNs;
    presentLagSampleCount_ = 0;
    presentLagTotalNs_ = 0;
    refreshStatsOverlayCache();
}

void MainWindow::updateStatsOverlay()
{
    if (!statsOverlay_) {
        return;
    }

    uiFps_ = videoSurface_ ? videoSurface_->takeUiFrameCount() * 2.0 : 0.0;
    paintFps_ = videoSurface_ ? videoSurface_->takePaintCount() * 2.0 : 0.0;

    int dmaPresents = 0;
    int cpuPresents = 0;
    if (videoSurface_ != nullptr) {
        videoSurface_->takePresentPathCounts(dmaPresents, cpuPresents);
    }
    if (latestTelemetry_.dmaFramesInWindow > 0 && latestTelemetry_.cpuFramesInWindow == 0) {
        displayPath_ = capture::VideoDisplayPath::Gpu;
    } else if (latestTelemetry_.cpuFramesInWindow > 0 && latestTelemetry_.dmaFramesInWindow == 0) {
        displayPath_ = capture::VideoDisplayPath::Cpu;
    } else if (latestTelemetry_.dmaFramesInWindow > 0 && latestTelemetry_.cpuFramesInWindow > 0) {
        displayPath_ = capture::VideoDisplayPath::Mixed;
    } else if (dmaPresents > 0 && cpuPresents == 0) {
        displayPath_ = capture::VideoDisplayPath::Gpu;
    } else if (cpuPresents > 0 && dmaPresents == 0) {
        displayPath_ = capture::VideoDisplayPath::Cpu;
    } else if (dmaPresents > 0 && cpuPresents > 0) {
        displayPath_ = capture::VideoDisplayPath::Mixed;
    } else if (latestTelemetry_.dmaCapturePathEnabled && latestTelemetry_.dmaFramesInWindow > 0) {
        displayPath_ = capture::VideoDisplayPath::Gpu;
    }

    refreshStatsOverlayCache();

    if (!showVideoStatsOverlay) {
        statsOverlay_->hide();
        return;
    }

    if (statsOverlay_->text() != cachedStatsOverlayText_) {
        statsOverlay_->setText(cachedStatsOverlayText_);
        statsOverlay_->adjustSize();
        if (videoSurface_) {
            videoSurface_->setOverlay(statsOverlay_);
        }
    }
    statsOverlay_->show();
}

void MainWindow::refreshStatsOverlayCache()
{
    const auto nextText = formatStatsOverlayText();
    if (nextText == cachedStatsOverlayText_) {
        return;
    }
    cachedStatsOverlayText_ = nextText;
}

QString MainWindow::formatStatsOverlayText() const
{
    const auto width = latestTelemetry_.width > 0 ? latestTelemetry_.width : selectedFormat_.width;
    const auto height = latestTelemetry_.height > 0 ? latestTelemetry_.height : selectedFormat_.height;
    const auto configuredFps = latestTelemetry_.configuredFps > 0.0 ? latestTelemetry_.configuredFps : selectedFormat_.framesPerSecond;

    auto pixelFourcc = latestTelemetry_.pixelFormatFourcc;
    if (pixelFourcc == 0) {
        pixelFourcc = stringToFourCc(selectedFormat_.pixelFormat);
    }

    const auto pixelFormat =
        pixelFourcc != 0 ? capture::fourCcToString(pixelFourcc) : selectedFormat_.pixelFormat;

    const auto frameMemory = resolveFrameMemoryLabel(latestTelemetry_);

    QStringList fields {
        QStringLiteral("%1x%2/%3").arg(width).arg(height).arg(QString::number(configuredFps, 'f', 0)),
        pixelFormat,
        QStringLiteral("FPS:%1").arg(QString::number(latestTelemetry_.decodedFps, 'f', 0)),
        QStringLiteral("Lag:%1").arg(QString::number(presentLagAvgMs_, 'f', 1)),
        QStringLiteral("Mem:%1").arg(frameMemory),
        QStringLiteral("Disp:%1").arg(capture::displayPathLabel(displayPath_)),
    };

    if (!showAdvancedVideoStats) {
        return fields.join(QStringLiteral(" | "));
    }

    fields += QStringList {
        QStringLiteral("UI:%1").arg(QString::number(uiFps_, 'f', 0)),
        QStringLiteral("Paint:%1").arg(QString::number(paintFps_, 'f', 0)),
        QStringLiteral("Cnv:%1").arg(QString::number(latestTelemetry_.decodeAvgMs, 'f', 1)),
        QStringLiteral("Cad:%1").arg(configuredFps > 0.0 ? QString::number(1000.0 / configuredFps, 'f', 1) : QStringLiteral("0.0")),
        QStringLiteral("Buf:%1").arg(latestTelemetry_.bufferCount),
        QStringLiteral("Payload:%1KiB").arg(QString::number(latestTelemetry_.payloadAvgKb, 'f', 0)),
    };
    return fields.join(QStringLiteral(" | "));
}

void MainWindow::preconfigureSelectedFormat(const bool force)
{
    if (selectedDevice_.backend != capture::CaptureBackend::V4L2 ||
        selectedDevice_.devicePath.isEmpty() ||
        selectedFormat_.width <= 0 ||
        selectedFormat_.height <= 0 ||
        selectedFormat_.pixelFormat.isEmpty()) {
        return;
    }

    const auto key = QStringLiteral("%1|%2x%3|%4|%5")
                         .arg(selectedDevice_.devicePath)
                         .arg(selectedFormat_.width)
                         .arg(selectedFormat_.height)
                         .arg(selectedFormat_.pixelFormat)
                         .arg(selectedFormat_.framesPerSecond, 0, 'f', 2);
    if (!force && preconfiguredFormatKey_ == key) {
        return;
    }
    preconfiguredFormatKey_ = key;

    const auto requestedPixelFormat = stringToFourCc(selectedFormat_.pixelFormat);
    if (requestedPixelFormat == 0) {
        return;
    }

    logCaptureStartup("preconfiguring selected capture format", selectedFormat_.label);
    const int fd = ::open(selectedDevice_.devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        logCaptureStartup("preconfigure open failed", QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }

    v4l2_format current {};
    current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &current) != 0) {
        logCaptureStartup("preconfigure G_FMT failed", QString::fromLocal8Bit(std::strerror(errno)));
        ::close(fd);
        return;
    }

    const auto alreadySelected =
        static_cast<int>(current.fmt.pix.width) == selectedFormat_.width &&
        static_cast<int>(current.fmt.pix.height) == selectedFormat_.height &&
        current.fmt.pix.pixelformat == requestedPixelFormat;
    if (!alreadySelected) {
        current.fmt.pix.width = static_cast<__u32>(selectedFormat_.width);
        current.fmt.pix.height = static_cast<__u32>(selectedFormat_.height);
        current.fmt.pix.pixelformat = requestedPixelFormat;
        if (xioctl(fd, VIDIOC_S_FMT, &current) != 0) {
            logCaptureStartup("preconfigure S_FMT failed", QString::fromLocal8Bit(std::strerror(errno)));
        } else {
            logCaptureStartup("preconfigure S_FMT applied; device may reset before Play");
        }
    } else {
        logCaptureStartup("preconfigure skipped; selected format already active");
    }

    ::close(fd);
}

void MainWindow::refreshStartupDevices()
{
    if (captureSession_ || playbackControls_ || videoSurface_ ||
        playbackStopping_.load(std::memory_order_acquire)) {
        return;
    }

    auto refreshedDevices = capture::CaptureBackendManager().enumerateDevices();
    if (deviceListSignature(refreshedDevices) == deviceListSignature(devices_)) {
        return;
    }

    devices_ = std::move(refreshedDevices);
    buildStoppedState();
}

void MainWindow::stopPlayback()
{
    uninhibitScreenSaver();
    playbackStopping_.store(false, std::memory_order_release);
    dmaVideoSurface_.store(nullptr, std::memory_order_release);
    if (audioSession_) {
        audioSession_->stop();
        audioSession_.reset();
    }

    if (videoSurface_) {
        videoSurface_->releasePlaybackFrames();
    }

    auto *session = captureSession_.release();
    auto *thread = captureThread_;
    captureThread_ = nullptr;

    if (session != nullptr) {
        if (thread != nullptr && thread->isRunning()) {
            QMetaObject::invokeMethod(
                session,
                [session, thread]() {
                    session->stop();
                    delete session;
                    thread->quit();
                },
                Qt::BlockingQueuedConnection);
            thread->wait();
            delete thread;
        } else {
            session->stop();
            delete session;
            if (thread != nullptr) {
                delete thread;
            }
        }
    } else if (thread != nullptr) {
        thread->quit();
        thread->wait();
        delete thread;
    }

    if (statsOverlayTimer_ != nullptr) {
        statsOverlayTimer_->stop();
    }
    presentLagAvgMs_ = 0.0;
    presentLagWindowStartNs_ = 0;
    presentLagSampleCount_ = 0;
    presentLagTotalNs_ = 0;
    statsOverlay_.clear();
    dmaVideoSurface_.store(nullptr, std::memory_order_release);
    videoSurface_.clear();
    buildStoppedState();
}

void MainWindow::stopPlaybackAsync()
{
    if (playbackStopping_.load(std::memory_order_acquire)) {
        return;
    }
    playbackStopping_.store(true, std::memory_order_release);
    dmaVideoSurface_.store(nullptr, std::memory_order_release);
    uninhibitScreenSaver();
    if (playbackControls_) {
        playbackControls_->setEnabled(false);
    }
    if (videoSurface_) {
        videoSurface_->setStartupOverlayVisible(false);
    }
    if (audioSession_) {
        audioSession_->stop();
        audioSession_.reset();
    }

    if (videoSurface_) {
        videoSurface_->releasePlaybackFrames();
    }
    showStoppingState();

    auto *session = captureSession_.release();
    auto *thread = captureThread_;

    if (session != nullptr) {
        QObject::disconnect(session, nullptr, this, nullptr);
        if (thread != nullptr && thread->isRunning()) {
            connect(thread, &QThread::finished, this, [this, thread]() {
                if (captureThread_ == thread) {
                    captureThread_ = nullptr;
                }
                thread->wait();
                delete thread;
                finishPlaybackStopped();
            });
            QMetaObject::invokeMethod(
                session,
                [session, thread]() {
                    session->stop();
                    delete session;
                    thread->quit();
                },
                Qt::QueuedConnection);
            return;
        } else {
            session->stop();
            delete session;
            if (thread != nullptr) {
                if (captureThread_ == thread) {
                    captureThread_ = nullptr;
                }
                delete thread;
            }
        }
    } else if (thread != nullptr) {
        thread->quit();
        thread->wait();
        if (captureThread_ == thread) {
            captureThread_ = nullptr;
        }
        delete thread;
    }

    dmaVideoSurface_.store(nullptr, std::memory_order_release);
    videoSurface_.clear();
    finishPlaybackStopped();
}

void MainWindow::finishPlaybackStopped()
{
    playbackStopping_.store(false, std::memory_order_release);
    dmaVideoSurface_.store(nullptr, std::memory_order_release);
    captureThread_ = nullptr;
    captureSession_.reset();
    buildStoppedState();
}

void MainWindow::showPlaybackControls()
{
    if (playbackControls_) {
        playbackControls_->show();
    }
}

void MainWindow::hidePlaybackControls()
{
    if (playbackControls_) {
        playbackControls_->hide();
    }
}

void MainWindow::resetPlaybackControlsTimer()
{
    if (controlsHideTimer_ != nullptr && playbackControls_) {
        controlsHideTimer_->start();
    }
}

void MainWindow::inhibitScreenSaver()
{
    if (!screenInhibitor_) {
        screenInhibitor_ = std::make_unique<ScreenInhibitor>();
    }
    screenInhibitor_->inhibit(windowHandle());
}

void MainWindow::uninhibitScreenSaver()
{
    if (screenInhibitor_) {
        screenInhibitor_->uninhibit();
    }
}

void MainWindow::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Settings"));
    dialog.setStyleSheet(QString::fromUtf8(dialogStyle));
    dialog.resize(580, 420);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("Settings"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 800;"));
    layout->addWidget(title);
    layout->addWidget(makeDivider(&dialog));

    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("S"),
        QStringLiteral("Playback settings"),
        QStringLiteral("Video stats, low-frame-rate warnings, flip, rotation, and other playback preferences will be added with the mock playback UX."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("V"),
        QStringLiteral("Volume"),
        QStringLiteral("The app volume preference is already backed by QSettings and will be wired to the playback controls."));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addStretch();
    layout->addWidget(buttons);

    dialog.exec();
}

void MainWindow::showHelpDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Help"));
    dialog.setStyleSheet(QString::fromUtf8(dialogStyle));
    dialog.resize(680, 520);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("Help"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 800;"));
    layout->addWidget(title);
    layout->addWidget(makeDivider(&dialog));

    addInfoRow(
        layout,
        &dialog,
        QStringLiteral(">"),
        QStringLiteral("Getting started"),
        QStringLiteral("Connect a UVC capture card, choose the device and capture format, then press Play."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("FPS"),
        QStringLiteral("Frame rate"),
        QStringLiteral("Higher frame rates feel better for games, but require capture-card and USB bandwidth support."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("[]"),
        QStringLiteral("Video controls"),
        QStringLiteral("During playback, controls will provide stop, app volume, zoom, pan, and settings access."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("A"),
        QStringLiteral("Audio"),
        QStringLiteral("Consolation controls app playback volume independently of the system volume."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("USB"),
        QStringLiteral("Device support"),
        QStringLiteral("Any Linux-supported UVC capture device should work once the real backend is implemented."));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttons);

    dialog.exec();
}

void MainWindow::showAboutDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("About Consolation"));
    dialog.setStyleSheet(QString::fromUtf8(dialogStyle));
    dialog.resize(660, 610);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(16);

    auto *header = new QHBoxLayout();
    header->setSpacing(14);
    header->addWidget(makePlaceholderIcon(&dialog));

    auto *titleBlock = new QVBoxLayout();
    auto *title = new QLabel(
        QStringLiteral("Consolation %1").arg(QString::fromUtf8(consolation::app::BuildInfo::releaseVersion)),
        &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 800;"));
    auto *subtitle = new QLabel(QStringLiteral("Copyright Centennial OSS Inc."), &dialog);
    subtitle->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180);"));
    titleBlock->addWidget(title);
    titleBlock->addWidget(subtitle);
    header->addLayout(titleBlock, 1);
    layout->addLayout(header);

    auto *body = new QLabel(
        QStringLiteral(
            "Consolation is a no-frills UVC capture viewer for using a Linux workstation "
            "as a display for consoles, Raspberry Pis, and other HDMI devices through a capture card.\n\n"
            "No recording, streaming, analytics, or network access is part of the app design. "
            "Audio and video stay local and transient while you are watching."),
        &dialog);
    body->setWordWrap(true);
    body->setStyleSheet(QStringLiteral("font-size: 14px;"));
    layout->addWidget(body);

    layout->addWidget(makeDivider(&dialog));

    auto *buildInfo = new QTextEdit(&dialog);
    buildInfo->setReadOnly(true);
    buildInfo->setPlainText(consolation::app::BuildInfo::copyableBlob());
    buildInfo->setFixedHeight(120);
    layout->addWidget(buildInfo);

    auto *actions = new QHBoxLayout();
    auto *githubButton = new QPushButton(QStringLiteral("GitHub"), &dialog);
    auto *privacyButton = new QPushButton(QStringLiteral("Privacy Policy"), &dialog);
    auto *copyButton = new QPushButton(QStringLiteral("Copy Build Info"), &dialog);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), &dialog);

    connect(githubButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QString::fromUtf8(consolation::app::AppMetadata::websiteUrl)));
    });
    connect(privacyButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QString::fromUtf8(consolation::app::AppMetadata::privacyUrl)));
    });
    connect(copyButton, &QPushButton::clicked, this, []() {
        QApplication::clipboard()->setText(consolation::app::BuildInfo::copyableBlob());
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    actions->addWidget(githubButton);
    actions->addWidget(privacyButton);
    actions->addStretch();
    actions->addWidget(copyButton);
    actions->addWidget(closeButton);
    layout->addLayout(actions);

    dialog.exec();
}

} // namespace consolation::ui

#include "MainWindow.moc"
