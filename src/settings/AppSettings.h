#pragma once

#include <QSettings>
#include <QString>

namespace consolation::settings {

class AppSettings {
public:
    // Uses ~/.config/Centennial OSS/Consolation.conf (or platform equivalent).
    AppSettings();

    // For tests only: read/write an isolated INI file instead of the user config.
    explicit AppSettings(const QString &settingsFilePath);

    [[nodiscard]] int volumePercent() const;
    void setVolumePercent(int volumePercent);

    [[nodiscard]] QString lastSelectedDeviceId() const;
    void setLastSelectedDeviceId(const QString &deviceId);

    [[nodiscard]] int statsPosition() const;
    void setStatsPosition(int position);

    [[nodiscard]] bool lowFpsWarningsEnabled() const;
    void setLowFpsWarningsEnabled(bool enabled);

    [[nodiscard]] bool debugStatsEnabled() const;
    void setDebugStatsEnabled(bool enabled);

    [[nodiscard]] int rotationDegrees() const;
    void setRotationDegrees(int rotationDegrees);

    [[nodiscard]] bool flipHorizontal() const;
    void setFlipHorizontal(bool enabled);

    [[nodiscard]] bool flipVertical() const;
    void setFlipVertical(bool enabled);

    [[nodiscard]] bool disableGpu() const;
    void setDisableGpu(bool enabled);

private:
    QSettings settings_;
};

} // namespace consolation::settings
