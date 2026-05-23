#include "settings/AppSettings.h"

#include <algorithm>

namespace consolation::settings {

namespace {
constexpr auto volumePercentKey = "playback/volumePercent";
constexpr auto lastSelectedDeviceIdKey = "capture/lastSelectedDeviceId";
constexpr auto defaultVolumePercent = 100;
} // namespace

AppSettings::AppSettings()
    : settings_("Centennial OSS", "Consolation")
{
}

int AppSettings::volumePercent() const
{
    const auto value = settings_.value(volumePercentKey, defaultVolumePercent).toInt();
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
