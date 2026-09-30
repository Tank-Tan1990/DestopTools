cmake_minimum_required(VERSION 3.5)
project(DestopTools VERSION 1.0.0 LANGUAGES CXX)

# 本项目实际针对 Qt 5.14.x 构建（qmake 为官方入口，见 DestopTools.pro）。
# 此 CMake 文件作为备用方案，仅当安装了 CMake 与 Qt5 时使用。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
# 原生代码以纯 C++ 构建 UI，不使用 .ui，故无需 AUTOUIC

find_package(Qt5 REQUIRED COMPONENTS Core Widgets)

set(SOURCES
    main.cpp
    mainwindow.cpp
    desktopscanner.cpp
    iconboxwidget.cpp
    desktopiconbutton.cpp
    sidepanelwidget.cpp
    settingsmanager.cpp
)

add_executable(${PROJECT_NAME} WIN32
    ${SOURCES}
    resources.qrc
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Core
    Qt5::Widgets
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE shell32 ole32 uuid)
    target_compile_definitions(${PROJECT_NAME} PRIVATE UNICODE _UNICODE)
endif()
