#include "settings/AppSettings.h"

#include "audio/AudioDefaults.h"

#include <algorithm>

namespace consolation::settings {

namespace {
constexpr auto volumePercentKey = "playback/volumePercent";
constexpr auto lastSelectedDeviceIdKey = "capture/lastSelectedDeviceId";
} // namespace

AppSettings::AppSettings()
    : settings_("Centennial OSS", "Consolation")
{
}

AppSettings::AppSettings(const QString &settingsFilePath)
    : settings_(settingsFilePath, QSettings::IniFormat)
{
}

int AppSettings::volumePercent() const
{
    if (!settings_.contains(volumePercentKey)) {
        return audio::defaultPlaybackVolumePercent;
    }
    const auto value = settings_.value(volumePercentKey).toInt();
    return std::clamp(value, 0, 100);
}

void AppSettings::setVolumePercent(const int volumePercent)
{
    settings_.setValue(volumePercentKey, std::clamp(volumePercent, 0, 100));
}

QString AppSettings::lastSelectedDeviceId() const
{
    return settings_.value(lastSelectedDeviceIdKey).toString();
}

void AppSettings::setLastSelectedDeviceId(const QString &deviceId)
{
    settings_.setValue(lastSelectedDeviceIdKey, deviceId);
}

} // namespace consolation::settings
