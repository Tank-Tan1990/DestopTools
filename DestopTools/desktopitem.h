/*
 * @file desktopitem.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef DESKTOPITEM_H
#define DESKTOPITEM_H

#include <QString>
#include <QIcon>
#include <QUuid>

struct DesktopItem {
    QUuid id;                 // 唯一标识，用于持久化和拖拽识别
    QString displayName;      // 显示名称
    QString sourcePath;       // 原始路径（.lnk 文件本身或普通文件）
    QString targetPath;       // .lnk 解析后的真实目标路径；非快捷方式等于 sourcePath
    QString category;         // 当前所属分类
    QIcon icon;
    bool isShortcut = false;
    bool isExecutable = false;
    bool isSpecial = false;        // 特殊命名空间项(我的电脑/回收站/网络),无真实文件路径
    QString launchCommand;         // 特殊项启动命令(如 explorer.exe ::{CLSID});普通项为空

    // —— 以下字段供「全屏 Dock 镜像层」使用 ——
    int systemImageIndex = -1;     // 真实桌面 SysListView32 每项自带的系统镜像列表索引，
                                   // 用该索引取图可与 Windows 桌面绘制同一图标像素级一致。
    QString shellPath;             // 启动/调原生上下文菜单用的 shell 路径：
                                   // 普通文件=绝对路径；特殊项="::{CLSID}"。由 Dock 镜像层赋值。

    DesktopItem() { id = QUuid::createUuid(); }

    QString launchPath() const { return isShortcut && !targetPath.isEmpty() ? targetPath : sourcePath; }
};

#endif // DESKTOPITEM_H
