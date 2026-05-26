#include "AppMetadata.h"
#include "capture/CaptureTypes.h"
#include "ui/AppIcon.h"
#include "capture/DmaBufFrame.h"
#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    qRegisterMetaType<consolation::capture::FrameHandle>();
    qRegisterMetaType<consolation::capture::DmaBufFrameHandle>();

    QApplication app(argc, argv);
    QApplication::setApplicationName(consolation::app::AppMetadata::displayName);
    QApplication::setApplicationDisplayName(consolation::app::AppMetadata::displayName);
    QApplication::setApplicationVersion(consolation::app::AppMetadata::version);
    QApplication::setOrganizationName("Centennial OSS");
    QApplication::setOrganizationDomain("centennialoss.org");
    QApplication::setDesktopFileName(consolation::app::AppMetadata::appId);
    QApplication::setWindowIcon(consolation::ui::createAppIcon());

    consolation::ui::MainWindow window;
    window.showMaximized();

    return QApplication::exec();
}
