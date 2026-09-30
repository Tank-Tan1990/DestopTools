QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

TEMPLATE = app
TARGET = d1layer

SOURCES += main.cpp

# MSVC: 源码为 UTF-8，必须加 /utf-8，否则带中文的字面量在 GBK 代码页下会触发 C2001/C1057
msvc: QMAKE_CXXFLAGS += /utf-8
