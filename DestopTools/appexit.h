/*
 * @file appexit.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef APPEXIT_H
#define APPEXIT_H

#include <QString>

// 「应用备份」跨模块编排（2026-09-24）
// 需求：设置中心「桌面备份」页点「应用备份」，把该备份里的 dock、收纳盒等
// 全部配置恢复生效。因程序有单实例锁（QLocalServer）且运行期各窗口会持续
// 向 INI 落盘，恢复必须在【旧实例完全退出之后】进行：
// 1) 设置中心置位 g_pendingBackupDir / g_restartAfterQuit 后调用 qApp->quit()；
// 2) main() 的事件循环退出、MainWindow 已析构（运行期落盘全部结束）后，
// 用备份目录里的 DestopTools.ini 覆盖现行配置，再 startDetached 拉起新实例。
// 这样退出路径上的任何 saveValue 都不可能把恢复好的配置再盖回去。
// 定义在 main.cpp，设置中心（settingcenterdialog.cpp）只写不读。

extern QString g_pendingBackupDir;   // 待应用的备份目录（含 DestopTools.ini / thumb.png / meta.ini）
extern bool g_restartAfterQuit;      // true = 事件循环退出后应用备份并重启

#endif // APPEXIT_H
