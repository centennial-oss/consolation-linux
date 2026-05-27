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

private:
    QSettings settings_;
};

} // namespace consolation::settings
