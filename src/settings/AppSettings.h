#pragma once

#include <QSettings>
#include <QString>

namespace consolation::settings {

class AppSettings {
public:
    AppSettings();

    [[nodiscard]] int volumePercent() const;
    void setVolumePercent(int volumePercent);

    [[nodiscard]] QString lastSelectedDeviceId() const;
    void setLastSelectedDeviceId(const QString &deviceId);

private:
    QSettings settings_;
};

} // namespace consolation::settings
