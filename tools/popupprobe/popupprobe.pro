QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

TEMPLATE = app
TARGET = popupprobe

INCLUDEPATH += $$PWD/../../DestopTools

SOURCES += main.cpp
HEADERS += ../../DestopTools/thememanager.h

# GetWindowLongPtrW / WS_EX_LAYERED 需要 user32
win32: LIBS += -luser32

# moc 产物放在本测试目录，绝不写进产品源码目录
MOC_DIR = $$PWD
OBJECTS_DIR = $$PWD/release
RCC_DIR = $$PWD/release
UI_DIR = $$PWD

# MSVC: 源码为 UTF-8，必须加 /utf-8
msvc: QMAKE_CXXFLAGS += /utf-8
