#pragma once

#include "capture/CaptureSession.h"
#include "capture/CaptureTypes.h"
#include "capture/DmaBufFrame.h"
#include "settings/AppSettings.h"

#include <QImage>
#include <QMainWindow>
#include <QPointer>

#include <atomic>
#include <memory>
#include <vector>

class QFrame;
class QLabel;
class QThread;
class QTimer;

namespace consolation::audio {
class AudioSession;
}

namespace consolation::ui {
class ScreenInhibitor;
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
    void showStoppingState();
    void showPlaybackState(capture::FrameHandle firstFrame = {});
    void updateVideoFrame(capture::FrameHandle frame, qint64 capturedAtNs = 0);
    void updateVideoDmaFrame(capture::DmaBufFrameHandle frame);
    void recordPresentLatency(qint64 latencyNs);
    void updateStatsOverlay();
    void refreshStatsOverlayCache();
    [[nodiscard]] QString formatStatsOverlayText() const;
    void stopPlayback();
    void stopPlaybackAsync();
    void finishPlaybackStopped();
    void preconfigureSelectedFormat(bool force = false);
    void refreshStartupDevices();
    void showPlaybackControls();
    void hidePlaybackControls();
    void resetPlaybackControlsTimer();
    void inhibitScreenSaver();
    void uninhibitScreenSaver();
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
    QTimer *statsOverlayTimer_ = nullptr;
    QTimer *startupRefreshTimer_ = nullptr;
    capture::VideoTelemetrySnapshot latestTelemetry_;
    QString cachedStatsOverlayText_;
    double uiFps_ = 0.0;
    double paintFps_ = 0.0;
    double presentLagAvgMs_ = 0.0;
    qint64 presentLagWindowStartNs_ = 0;
    int presentLagSampleCount_ = 0;
    qint64 presentLagTotalNs_ = 0;
    capture::VideoDisplayPath displayPath_ = capture::VideoDisplayPath::Unknown;
    std::atomic<bool> playbackStopping_{false};
    std::unique_ptr<audio::AudioSession> audioSession_;
    std::unique_ptr<capture::CaptureSession> captureSession_;
    std::unique_ptr<ScreenInhibitor> screenInhibitor_;
    QThread *captureThread_ = nullptr;
};

} // namespace consolation::ui
