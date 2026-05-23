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

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup()

set(CONSOLATION_APP_ID "org.centennialoss.consolation")
set(CONSOLATION_BUILD_DATE "unknown" CACHE STRING "Build date embedded in the app")
set(CONSOLATION_GIT_COMMIT "unknown" CACHE STRING "Git commit embedded in the app")

configure_file(
    src/app/AppMetadata.h.in
    generated/AppMetadata.h
    @ONLY
)

add_library(consolation_core STATIC
    src/app/BuildInfo.h
    src/settings/AppSettings.cpp
    src/settings/AppSettings.h
)

target_include_directories(consolation_core
    PUBLIC
        src
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(consolation_core
    PUBLIC
        Qt6::Core
)

add_executable(consolation
    src/app/main.cpp
    src/ui/AppIcon.cpp
    src/ui/AppIcon.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
)

target_link_libraries(consolation
    PRIVATE
        consolation_core
        Qt6::Gui
        Qt6::Widgets
)

set_target_properties(consolation PROPERTIES
    OUTPUT_NAME "consolation"
)

enable_testing()

add_executable(consolation_smoke_test
    tests/smoke_test.cpp
)

target_link_libraries(consolation_smoke_test
    PRIVATE
        consolation_core
)

add_test(NAME consolation_smoke_test COMMAND consolation_smoke_test)
