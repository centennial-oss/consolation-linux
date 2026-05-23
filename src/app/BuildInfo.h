#pragma once

#include <QString>

namespace consolation::app {

struct BuildInfo {
    static constexpr auto releaseVersion = "localdev";
    static constexpr auto buildDate = "localdev";
    static constexpr auto buildPlatform = "localdev";
    static constexpr auto gitCommit = "localdev";

    [[nodiscard]] static QString copyableBlob()
    {
        return QStringLiteral(
            "Release Version: %1 (Linux)\n"
            "Build Date: %2\n"
            "Build Platform: %3\n"
            "Git Commit: %4")
            .arg(
                QString::fromUtf8(releaseVersion),
                QString::fromUtf8(buildDate),
                QString::fromUtf8(buildPlatform),
                QString::fromUtf8(gitCommit));
    }
};

} // namespace consolation::app
