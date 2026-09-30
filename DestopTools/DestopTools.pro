QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets network concurrent winextras

CONFIG += c++17

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    main.cpp \
    crashtrace.cpp \
    shellops.cpp \
    mainwindow.cpp \
    desktopscanner.cpp \
    desktopiconbutton.cpp \
    dockiconbutton.cpp \
    desktopmirrorwindow.cpp \
    desktopimmunityfilter.cpp \
    sidepanelwidget.cpp \
    settingsmanager.cpp \
    quicktoolswidget.cpp \
    icongridwindow.cpp \
    fencebox.cpp \
    assistantwindow.cpp \
    settingcenterdialog.cpp \
    updatedialog.cpp \
    feedbackdialog.cpp \
    backupdialog.cpp \
    shortcutedit.cpp \
    glassmessagebox.cpp \
    glassinputdialog.cpp \
    filesearchwidget.cpp \
    shutdowntask.cpp \
    shutdowntimerwindow.cpp \
    wallpaperwindow.cpp \
    todostore.cpp \
    todolistwindow.cpp \
    regiondata.cpp \
    thememanager.cpp \
    categorystore.cpp \
    windowsnap.cpp

HEADERS += \
    mainwindow.h \
    crashtrace.h \
    shellops.h \
    diagtrace.h \
    editorwatch.h \
    desktopscanner.h \
    desktopitem.h \
    desktopiconbutton.h \
    dockiconbutton.h \
    desktopmirrorwindow.h \
    desktopimmunityfilter.h \
    sidepanelwidget.h \
    settingsmanager.h \
    theme.h \
    quicktoolswidget.h \
    icongridwindow.h \
    fencebox.h \
    assistantwindow.h \
    settingcenterdialog.h \
    updatedialog.h \
    feedbackdialog.h \
    backupdialog.h \
    shortcutedit.h \
    glassmessagebox.h \
    glassinputdialog.h \
    filesearchwidget.h \
    filesearchworker.h \
    shutdowntask.h \
    shutdowntimerwindow.h \
    wallpaperwindow.h \
    todostore.h \
    todolistwindow.h \
    regiondata.h \
    thememanager.h \
    categorystore.h \
    windowsnap.h \
    rawmouse.h

RESOURCES += \
    resources.qrc \
    images/icons/icons.qrc

win32 {
    # 应用图标：编进 exe 的图标资源（appicon.ico 由 tools/make_icon.py 生成）
    RC_FILE = appicon.rc
    LIBS += -lshell32 -lole32 -luuid -luser32 -lcomctl32 -lgdi32 -lshlwapi -ladvapi32 -ldwmapi -lwinmm
    # MSVC: 源码为 UTF-8 编码，必须加 /utf-8 否则带中文 QStringLiteral 在 GBK 代码页下会触发 C2001/C1057
    msvc: QMAKE_CXXFLAGS += /utf-8
    # MSVC: 生成符号映射 release/DestopTools.map —— release 无 .pdb，这是崩溃调用栈反查函数名的
    # 唯一符号来源（配合 tools/addr2func.py 把「模块 + 偏移」转成「函数名 + 偏移」）。
    msvc: QMAKE_LFLAGS += /MAP
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
