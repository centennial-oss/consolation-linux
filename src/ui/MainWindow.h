#pragma once

#include "settings/AppSettings.h"

#include <QMainWindow>
#include <QPointer>

class QFrame;
class QLabel;
class QTimer;

namespace consolation::ui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildStoppedState();
    void startMockPlayback();
    void showConnectingState();
    void showPlaybackState();
    void stopPlayback();
    void showPlaybackControls();
    void hidePlaybackControls();
    void resetPlaybackControlsTimer();
    void showSettingsDialog();
    void showHelpDialog();
    void showAboutDialog();

    settings::AppSettings settings_;
    QPointer<QFrame> playbackControls_;
    QTimer *controlsHideTimer_ = nullptr;
};

} // namespace consolation::ui
