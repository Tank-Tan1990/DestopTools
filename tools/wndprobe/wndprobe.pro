QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

TEMPLATE = app
TARGET = wndprobe

SOURCES += main.cpp

MOC_DIR = $$PWD
OBJECTS_DIR = $$PWD/release
RCC_DIR = $$PWD/release
UI_DIR = $$PWD

msvc: QMAKE_CXXFLAGS += /utf-8

# GetWindowLongPtrW / WS_EX_LAYERED 需要 user32
win32: LIBS += -luser32
