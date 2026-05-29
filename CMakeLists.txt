cmake_minimum_required(VERSION 3.22)

project(
    Consolation
    VERSION 0.1.0
    DESCRIPTION "A no-frills UVC capture viewer for Linux"
    HOMEPAGE_URL "https://github.com/centennial-oss/consolation-linux"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core DBus Gui OpenGLWidgets Svg Widgets)
find_package(JPEG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBVA libva libva-drm IMPORTED_TARGET)

qt_standard_project_setup()

set(UNIT_TEST OFF CACHE BOOL "Disable libyuv unit tests" FORCE)
add_subdirectory(third_party/libyuv EXCLUDE_FROM_ALL)

set(CONSOLATION_APP_ID "org.centennialoss.consolation")
set(CONSOLATION_BUILD_DATE "unknown" CACHE STRING "Build date embedded in the app")
set(CONSOLATION_GIT_COMMIT "unknown" CACHE STRING "Git commit embedded in the app")

configure_file(
    src/app/AppMetadata.h.in
    generated/AppMetadata.h
    @ONLY
)

add_library(consolation_core STATIC
    src/audio/AudioRingBuffer.cpp
    src/audio/AudioRingBuffer.h
    src/audio/AudioSession.h
    src/app/BuildInfo.h
    src/capture/CaptureBackendManager.cpp
    src/capture/CaptureBackendManager.h
    src/capture/CaptureSession.h
    src/capture/CaptureTypes.h
    src/capture/DmaBufFrame.h
    src/capture/FrameBufferPool.cpp
    src/capture/FrameBufferPool.h
    src/platform/linux/Nv12DmaBufGl.cpp
    src/platform/linux/Nv12DmaBufGl.h
    src/platform/linux/P010DmaBufGl.cpp
    src/platform/linux/P010DmaBufGl.h
    src/platform/linux/RgbDmaBufGl.cpp
    src/platform/linux/RgbDmaBufGl.h
    src/platform/linux/YuyvDmaBufGl.cpp
    src/platform/linux/YuyvDmaBufGl.h
    src/platform/linux/I420DmaBufGl.cpp
    src/platform/linux/I420DmaBufGl.h
    src/platform/linux/MjpegDecoder.cpp
    src/platform/linux/MjpegDecoder.h
    src/platform/linux/MjpegDecoderBackends.h
    src/platform/linux/MjpegDecoderJpegParse.cpp
    src/platform/linux/MjpegDecoderJpegParse.h
    src/platform/linux/MjpegDecoderV4l2M2m.cpp
    src/platform/linux/MjpegPerfLog.cpp
    src/platform/linux/MjpegPerfLog.h
    src/platform/linux/MjpegDecoderVaapi.cpp
    src/platform/linux/V4L2CaptureSession.cpp
    src/platform/linux/V4L2CaptureSession.h
    src/platform/linux/V4L2DeviceDiscovery.cpp
    src/platform/linux/V4L2DeviceDiscovery.h
    src/platform/linux/PipeWireAudioSession.cpp
    src/platform/linux/PipeWireAudioSession.h
    src/settings/AppSettings.cpp
    src/settings/AppSettings.h
)

target_include_directories(consolation_core
    PUBLIC
        src
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PRIVATE
        third_party/libyuv/include
)

target_link_libraries(consolation_core
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PRIVATE
        PkgConfig::PIPEWIRE
        yuv
)

if(JPEG_FOUND)
    target_compile_definitions(consolation_core PRIVATE HAVE_JPEG)
    if(TARGET JPEG::JPEG)
        target_link_libraries(consolation_core PRIVATE JPEG::JPEG)
    else()
        target_link_libraries(consolation_core PRIVATE ${JPEG_LIBRARIES})
    endif()
endif()

if(LIBVA_FOUND)
    target_compile_definitions(consolation_core PRIVATE CONSOLATION_HAVE_LIBVA)
    target_link_libraries(consolation_core PRIVATE PkgConfig::LIBVA)
endif()

if(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(consolation_core PRIVATE OpenGL::EGL OpenGL::GL)
endif()

add_executable(consolation
    src/app/main.cpp
    src/ui/AppIcon.cpp
    src/ui/AppIcon.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
)

qt_add_resources(consolation "consolation"
    PREFIX "/icons"
    BASE assets/icons
    FILES
        assets/icons/power.svg
        assets/icons/settings.svg
        assets/icons/app-window.svg
        assets/icons/maximize-2.svg
        assets/icons/minimize-2.svg
        assets/icons/zoom-out.svg
        assets/icons/zoom-in.svg
        assets/icons/play.svg
        assets/icons/circle-play.svg
        assets/icons/square-stack.svg
        assets/icons/fullscreen.svg
        assets/icons/volume-2.svg
        assets/icons/volume-off.svg
        assets/icons/usb.svg
        assets/icons/triangle-alert.svg
        assets/icons/shield.svg
        assets/icons/heart.svg
        assets/icons/file-text.svg
        assets/icons/info.svg
        assets/icons/circle-question-mark.svg
)

qt_add_resources(consolation "consolation_app_icons"
    PREFIX "/app"
    BASE assets/icons
    FILES
        assets/icons/app-icon-rounded.png
        assets/icons/app-icon-large-rounded.png
)

target_link_libraries(consolation
    PRIVATE
        consolation_core
        Qt6::DBus
        Qt6::Gui
        Qt6::OpenGLWidgets
        Qt6::Svg
        Qt6::Widgets
        PkgConfig::PIPEWIRE
        yuv
)

target_include_directories(consolation PRIVATE third_party/libyuv/include)

set_target_properties(consolation PROPERTIES
    OUTPUT_NAME "consolation"
)

include(GNUInstallDirs)

install(TARGETS consolation
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES packaging/linux/org.centennialoss.consolation.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

set(CONSOLATION_ICON_NAME org.centennialoss.consolation)
foreach(CONSOLATION_ICON_SIZE IN ITEMS 48 128)
    install(
        FILES assets/icons/app-icon-rounded.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${CONSOLATION_ICON_SIZE}x${CONSOLATION_ICON_SIZE}/apps
        RENAME ${CONSOLATION_ICON_NAME}.png
    )
endforeach()
install(
    FILES assets/icons/app-icon-large-rounded.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
    RENAME ${CONSOLATION_ICON_NAME}.png
)

set(CPACK_PACKAGE_NAME "consolation")
set(CPACK_PACKAGE_VENDOR "Centennial OSS Inc.")
set(CPACK_PACKAGE_CONTACT "Centennial OSS Inc.")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libpipewire-0.3-0, libqt6core6t64 | libqt6core6, libqt6dbus6t64 | libqt6dbus6, libqt6gui6t64 | libqt6gui6, libqt6openglwidgets6, libqt6svg6, libqt6widgets6t64 | libqt6widgets6, qt6-qpa-plugins, qt6-wayland"
)

set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_REQUIRES
    "pipewire-libs, qt6-qtbase, qt6-qtbase-gui, qt6-qtsvg, qt6-qtwayland"
)

include(CPack)

enable_testing()

add_executable(consolation_smoke_test
    tests/smoke_test.cpp
)

target_link_libraries(consolation_smoke_test
    PRIVATE
        consolation_core
)

add_test(NAME consolation_smoke_test COMMAND consolation_smoke_test)

add_executable(list_capture_devices
    tests/list_capture_devices.cpp
)

target_link_libraries(list_capture_devices
    PRIVATE
        consolation_core
)

add_executable(headless_capture
    tests/headless_capture.cpp
)

target_link_libraries(headless_capture
    PRIVATE
        consolation_core
)

add_executable(v4l2_config_probe
    tests/v4l2_config_probe.cpp
)

target_link_libraries(v4l2_config_probe
    PRIVATE
        Qt6::Core
)
