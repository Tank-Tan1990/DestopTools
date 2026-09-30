QT += core gui widgets winextras network concurrent

CONFIG += c++17 console
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tagprobe

PRODUCTDIR = $$PWD/../../DestopTools

INCLUDEPATH += $$PRODUCTDIR

SOURCES += main.cpp

# ⚠️ 关键：把产品【已编译的 .obj】全部链进来（只去掉 main.obj），
#    跑的是产品真实代码（真实 IconGridWindow / ThemeManager / SettingsManager），不是替身。
PRODUCT_OBJS = $$files($$PRODUCTDIR/release/*.obj)
PRODUCT_OBJS -= $$PRODUCTDIR/release/main.obj
LIBS += $$PRODUCT_OBJS
LIBS += shell32.lib ole32.lib uuid.lib user32.lib comctl32.lib gdi32.lib shlwapi.lib advapi32.lib dwmapi.lib winmm.lib

MOC_DIR = $$PWD
OBJECTS_DIR = $$PWD/release
RCC_DIR = $$PWD/release
UI_DIR = $$PWD

msvc: QMAKE_CXXFLAGS += /utf-8
