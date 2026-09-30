QT += core gui widgets
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = slidertest
SOURCES += main.cpp

# MSVC: 源码为 UTF-8 编码，必须加 /utf-8，否则带中文的 QStringLiteral 在 GBK 代码页下会报 C2447/C2001
msvc: QMAKE_CXXFLAGS += /utf-8
