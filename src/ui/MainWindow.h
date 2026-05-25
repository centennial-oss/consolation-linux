#pragma once

#include "capture/CaptureSession.h"
#include "capture/CaptureTypes.h"
#include "settings/AppSettings.h"

#include <QImage>
#include <QMainWindow>
#include <QPointer>

#include <memory>
#include <vector>

class QFrame;
class QLabel;
class QThread;
class QTimer;

namespace consolation::ui {

class VideoSurface;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildStoppedState();
    void startPlayback();
    void showConnectingState();
    void showPlaybackState(const QImage &firstFrame = {});
    void updateVideoFrame(const QImage &frame);
    void scheduleVideoFrame(const QImage &frame);
    void renderLatestVideoFrame();
    void updateStatsOverlay();
    QString buildStatsOverlayText() const;
    void stopPlayback();
    void stopPlaybackAsync();
    void preconfigureSelectedFormat(bool force = false);
    void showPlaybackControls();
    void hidePlaybackControls();
    void resetPlaybackControlsTimer();
    void showSettingsDialog();
    void showHelpDialog();
    void showAboutDialog();

    settings::AppSettings settings_;
    std::vector<capture::CaptureDevice> devices_;
    capture::CaptureDevice selectedDevice_;
    capture::CaptureFormat selectedFormat_;
    QString preconfiguredFormatKey_;
    QPointer<VideoSurface> videoSurface_;
    QPointer<QLabel> statsOverlay_;
    QPointer<QFrame> playbackControls_;
    QTimer *controlsHideTimer_ = nullptr;
    QTimer *videoRenderTimer_ = nullptr;
    QTimer *statsOverlayTimer_ = nullptr;
    QImage latestVideoFrame_;
    capture::VideoTelemetrySnapshot latestTelemetry_;
    int displayedFramesSinceStats_ = 0;
    double displayedFps_ = 0.0;
    std::unique_ptr<capture::CaptureSession> captureSession_;
    QThread *captureThread_ = nullptr;
};

} // namespace consolation::ui
