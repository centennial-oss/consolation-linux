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
    void stopPlayback();
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
    QPointer<QLabel> videoSurface_;
    QPointer<QFrame> playbackControls_;
    QTimer *controlsHideTimer_ = nullptr;
    std::unique_ptr<capture::CaptureSession> captureSession_;
    QThread *captureThread_ = nullptr;
};

} // namespace consolation::ui
