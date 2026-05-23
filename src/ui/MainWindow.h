#pragma once

#include "settings/AppSettings.h"

#include <QMainWindow>

class QLabel;

namespace consolation::ui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void buildStoppedState();
    void showSettingsDialog();
    void showHelpDialog();
    void showAboutDialog();

    settings::AppSettings settings_;
};

} // namespace consolation::ui
