#pragma once

#include <QString>

namespace consolation::app {

struct BuildInfo {
    static constexpr auto releaseVersion = "localdev";
    static constexpr auto buildDate = "localdev";
    static constexpr auto buildPlatform = "localdev";
    static constexpr auto buildArchitecture = "localdev";
    static constexpr auto gitCommit = "localdev";

    [[nodiscard]] static QString copyableBlob()
    {
        return QStringLiteral(
            "Release Version: %1 (%2)\n"
            "Build Date: %3\n"
            "Build Platform: %4\n"
            "Build Architecture: %5\n"
            "Git Commit: %6")
            .arg(
                QString::fromUtf8(releaseVersion),
                QStringLiteral("%1, %2")
                    .arg(QString::fromUtf8(buildPlatform), QString::fromUtf8(buildArchitecture)),
                QString::fromUtf8(buildDate),
                QString::fromUtf8(buildPlatform),
                QString::fromUtf8(buildArchitecture),
                QString::fromUtf8(gitCommit));
    }
};

} // namespace consolation::app
