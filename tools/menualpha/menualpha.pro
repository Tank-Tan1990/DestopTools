QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

TEMPLATE = app
TARGET = menualpha

INCLUDEPATH += $$PWD/../../DestopTools

SOURCES += main.cpp
HEADERS += ../../DestopTools/thememanager.h

MOC_DIR = $$PWD
OBJECTS_DIR = $$PWD/release
RCC_DIR = $$PWD/release
UI_DIR = $$PWD

msvc: QMAKE_CXXFLAGS += /utf-8
win32: LIBS += -luser32
