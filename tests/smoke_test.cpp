#include "AppMetadata.h"
#include "app/BuildInfo.h"
#include "settings/AppSettings.h"

#include <QCoreApplication>

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

    consolation::settings::AppSettings settings;
    settings.setVolumePercent(125);
    assert(settings.volumePercent() == 100);
    settings.setVolumePercent(-10);
    assert(settings.volumePercent() == 0);

    return 0;
}
