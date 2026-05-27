#include "settings/AppSettings.h"

#include "audio/AudioDefaults.h"

#include <algorithm>

namespace consolation::settings {

namespace {
constexpr auto volumePercentKey = "playback/volumePercent";
constexpr auto lastSelectedDeviceIdKey = "capture/lastSelectedDeviceId";
constexpr auto statsPositionKey = "playback/statsPosition";
constexpr auto lowFpsWarningsEnabledKey = "playback/lowFpsWarningsEnabled";
constexpr auto debugStatsEnabledKey = "playback/debugStatsEnabled";
constexpr auto rotationDegreesKey = "playback/rotationDegrees";
constexpr auto flipHorizontalKey = "playback/flipHorizontal";
constexpr auto flipVerticalKey = "playback/flipVertical";
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

int AppSettings::statsPosition() const
{
    const auto value = settings_.value(statsPositionKey, 0).toInt();
    return std::clamp(value, 0, 2);
}

void AppSettings::setStatsPosition(const int position)
{
    settings_.setValue(statsPositionKey, std::clamp(position, 0, 2));
}

bool AppSettings::lowFpsWarningsEnabled() const
{
    return settings_.value(lowFpsWarningsEnabledKey, true).toBool();
}

void AppSettings::setLowFpsWarningsEnabled(const bool enabled)
{
    settings_.setValue(lowFpsWarningsEnabledKey, enabled);
}

bool AppSettings::debugStatsEnabled() const
{
    return settings_.value(debugStatsEnabledKey, false).toBool();
}

void AppSettings::setDebugStatsEnabled(const bool enabled)
{
    settings_.setValue(debugStatsEnabledKey, enabled);
}

int AppSettings::rotationDegrees() const
{
    const auto value = settings_.value(rotationDegreesKey, 0).toInt();
    switch (value) {
    case 90:
    case 180:
    case 270:
        return value;
    default:
        return 0;
    }
}

void AppSettings::setRotationDegrees(const int rotationDegrees)
{
    switch (rotationDegrees) {
    case 90:
    case 180:
    case 270:
        settings_.setValue(rotationDegreesKey, rotationDegrees);
        return;
    default:
        settings_.setValue(rotationDegreesKey, 0);
        return;
    }
}

bool AppSettings::flipHorizontal() const
{
    return settings_.value(flipHorizontalKey, false).toBool();
}

void AppSettings::setFlipHorizontal(const bool enabled)
{
    settings_.setValue(flipHorizontalKey, enabled);
}

bool AppSettings::flipVertical() const
{
    return settings_.value(flipVerticalKey, false).toBool();
}

void AppSettings::setFlipVertical(const bool enabled)
{
    settings_.setValue(flipVerticalKey, enabled);
}

} // namespace consolation::settings
