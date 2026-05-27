#include "AppMetadata.h"
#include "app/BuildInfo.h"
#include "audio/AudioDefaults.h"
#include "platform/linux/V4L2DeviceDiscovery.h"
#include "settings/AppSettings.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <cassert>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Centennial OSS");
    QCoreApplication::setOrganizationDomain("centennialoss.org");
    QCoreApplication::setApplicationName(consolation::app::AppMetadata::displayName);

    assert(QString::fromUtf8(consolation::app::AppMetadata::displayName) == QStringLiteral("Consolation"));
    assert(QString::fromUtf8(consolation::app::AppMetadata::appId) == QStringLiteral("org.centennialoss.consolation"));
    assert(QString::fromUtf8(consolation::app::BuildInfo::releaseVersion) == QStringLiteral("localdev"));
    assert(consolation::app::BuildInfo::copyableBlob().contains(QStringLiteral("Release Version: localdev")));

    assert(consolation::audio::defaultPlaybackVolumePercent == 75);

    const auto clampVolume = [](const int percent) { return std::clamp(percent, 0, 100); };
    assert(clampVolume(125) == 100);
    assert(clampVolume(-10) == 0);

    // Never write test values into ~/.config — use an isolated temporary INI file.
    QTemporaryDir tempDir;
    assert(tempDir.isValid());
    const auto settingsPath = tempDir.filePath(QStringLiteral("consolation_smoke_settings.ini"));
    consolation::settings::AppSettings isolatedSettings(settingsPath);
    assert(isolatedSettings.volumePercent() == consolation::audio::defaultPlaybackVolumePercent);
    isolatedSettings.setVolumePercent(100);
    assert(isolatedSettings.volumePercent() == 100);
    isolatedSettings.setVolumePercent(125);
    assert(isolatedSettings.volumePercent() == 100);
    isolatedSettings.setVolumePercent(-10);
    assert(isolatedSettings.volumePercent() == 0);

    const auto devices = consolation::platform::linux::V4L2DeviceDiscovery().enumerateDevices();
    for (const auto &device : devices) {
        assert(!device.devicePath.isEmpty());
        assert(!device.displayName.isEmpty());
        assert(!device.formats.empty());
    }

    return 0;
}
