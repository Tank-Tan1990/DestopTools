QT += core gui

CONFIG += c++17 console
CONFIG -= app_bundle

TEMPLATE = app
TARGET = alphatest

INCLUDEPATH += $$PWD/../../DestopTools

SOURCES += main.cpp
HEADERS += ../../DestopTools/thememanager.h

# moc 产物放在本测试目录，绝不写进产品源码目录
MOC_DIR = $$PWD
OBJECTS_DIR = $$PWD/release
RCC_DIR = $$PWD/release
UI_DIR = $$PWD

# MSVC: 源码为 UTF-8，必须加 /utf-8，否则带中文的字面量在 GBK 代码页下会触发 C2001/C1057
msvc: QMAKE_CXXFLAGS += /utf-8
