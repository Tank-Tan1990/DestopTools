# -*- coding: utf-8 -*-
"""功能完整性断言（回退基线版 · 只读）。

用途：在**回退到 2026-09-24 13:28 基线**之后，一次性核对
      ① 全部既有功能的界面文案/设置键/资源确实编进了 exe；
      ② 已被回退的「6 条修复」痕迹确实**不在** exe 里（反向断言）。

用法： python.exe tools/assert_features.py
判据： QStringLiteral / 资源名 → UTF-16LE；QSettings 键 / qDebug 埋点 → ASCII(UTF-8)。
说明： 本脚本不修改任何程序代码与产物，只读 exe。
"""
import os
import struct
import sys

EXE = r"F:\Desktop\DestopTools\DestopTools\release\DestopTools.exe"

# ─────────────── 正向断言：既有功能必须都在 ───────────────
PRESENT = [
    # 关于我们页
    ("关于页 版本号",            "当前版本：0.1.0",              "utf16"),
    ("关于页 作者",              "作者：谭征",                    "utf16"),
    ("关于页 联系方式",          "联系方式：QQ 906548370",        "utf16"),
    ("关于页 用户体验改善计划",   "用户体验改善计划",              "utf16"),
    ("关于页 检查更新",          "检查更新",                      "utf16"),
    ("关于页 许可使用协议",       "许可使用协议",                  "utf16"),
    ("关于页 隐私保护说明",       "隐私保护说明",                  "utf16"),
    # 设置中心七页
    ("设置页 常规设置",          "常规设置",                      "utf16"),
    ("设置页 桌面整理",          "桌面整理",                      "utf16"),
    ("设置页 外观设置",          "外观设置",                      "utf16"),
    ("设置页 桌面备份",          "桌面备份",                      "utf16"),
    ("设置页 快捷操作",          "快捷操作",                      "utf16"),
    ("设置页 小工具",            "小工具",                        "utf16"),
    ("设置页 关于我们",          "关于我们",                      "utf16"),
    # 外观项
    ("外观 闹铃开关",            "闹铃",                          "utf16"),
    ("外观 盒子显示边框",        "盒子显示边框",                  "utf16"),
    ("外观 盒子使用圆角",        "盒子使用圆角",                  "utf16"),
    ("外观 快捷方式箭头",        "在快捷方式图标上显示箭头",        "utf16"),
    ("外观 提醒声音",            "提醒声音",                      "utf16"),
    ("外观 桌面字体设置",        "桌面字体设置",                  "utf16"),
    # 自动对齐（12:57 版：顶部留白 10 / 网格 44）
    ("对齐 顶部留白10文案",      "距桌面顶部始终留出 10 逻辑像素", "utf16"),
    ("对齐 网格步长文案",        "吸附到网格（每格约 44 逻辑像素", "utf16"),
    ("对齐 Alt 逃生口文案",      "按住 Alt 拖动可临时关闭自动对齐", "utf16"),
    # 快捷操作（拖框建盒）
    ("快捷操作 拉框提示",        "松开即创建收纳盒",              "utf16"),
    ("快捷操作 建盒按钮",        "创建收纳盒",                    "utf16"),
    ("快捷操作 默认盒名",        "收纳盒%1",                      "utf16"),
    ("快捷操作 取消按钮",        "取消",                          "utf16"),
    # 桌面备份
    ("备份 添加备份",            "添加备份",                      "utf16"),
    ("备份 应用备份",            "应用备份",                      "utf16"),
    ("备份 删除备份",            "删除备份",                      "utf16"),
    ("备份 页面标题",            "桌面布局备份",                  "utf16"),
    # 待办提醒
    ("待办 提醒弹框标题",        "待办提醒",                      "utf16"),
    ("待办 确认按钮",            "知道了",                        "utf16"),
    # 14 个实用小工具
    ("小工具 桌面整理",          "桌面整理",                      "utf16"),
    ("小工具 快速搜索",          "快速搜索",                      "utf16"),
    ("小工具 文件管理",          "文件管理",                      "utf16"),
    ("小工具 定时关机",          "定时关机",                      "utf16"),
    ("小工具 截屏",              "截屏",                          "utf16"),
    ("小工具 记事本",            "记事本",                        "utf16"),
    ("小工具 壁纸",              "壁纸",                          "utf16"),
    ("小工具 锁屏",              "锁屏",                          "utf16"),
    ("小工具 上网",              "上网",                          "utf16"),
    ("小工具 计算器",            "计算器",                        "utf16"),
    ("小工具 注册表",            "注册表",                        "utf16"),
    ("小工具 命令行",            "命令行",                        "utf16"),
    ("小工具 磁盘清理",          "磁盘清理",                      "utf16"),
    ("小工具 工具管理",          "工具管理",                      "utf16"),
    # 资源 / 模块 / 设置键（ASCII）
    ("资源 喇叭图标",            "speaker_1x.png",                 "ascii"),
    ("资源 喇叭图标2x",          "speaker_2x.png",                 "ascii"),
    ("模块 单实例服务名",        "DestopTools-SingleInstance",     "ascii"),
    ("模块 主配置名",            "DestopTools.ini",                "ascii"),
    ("模块 分类映射库",          "categories.ini",                 "ascii"),
    ("模块 Raw Input 通道",      "RawInput",                       "ascii"),
    ("设置键 闹铃",              "Appearance/remindSoundEnabled",  "ascii"),
    ("设置键 双击隐藏图标",      "QuickActions/hideIconsOnDoubleClick", "ascii"),
    ("设置键 空白拉框建盒",      "QuickActions/drawBoxOnBlank",    "ascii"),
    ("设置键 主题色",            "Appearance/accent",              "ascii"),
    ("设置键 透明度",            "Appearance/transparency",        "ascii"),
    ("信号 建盒区域",            "boxRegionDrawn",                 "ascii"),
    ("信号 请求刷新",            "requestRefresh",                 "ascii"),
    ("埋点 刷新进入",            "refresh:enter",                  "ascii"),
    ("埋点 刷新完成",            "refresh:done",                   "ascii"),
    ("埋点 取图路径1",           "loadIcon:p1-IExtractIcon",       "ascii"),
    ("埋点 取图路径2",           "loadIcon:p2-SHDefExtractIcon",   "ascii"),
    ("埋点 取图路径3",           "loadIcon:p3-imageListIndex",     "ascii"),
]

# ─────────────── 反向断言：已回退的「6 条修复」不得残留 ───────────────
ABSENT = [
    ("已回退 全屏闸门头文件痕迹", "foregroundgate",                      "ascii"),
    ("已回退 廉价门埋点",         "refresh:cheap-skip",                   "ascii"),
    ("已回退 图标队列告警",       "setDockButtonIcon: null icon",         "ascii"),
]


def pe_machine(path):
    """读 PE 头，返回 (机器码, 说明)。0x8664=x64，0x14c=x86。"""
    with open(path, "rb") as f:
        if f.read(2) != b"MZ":
            return 0, "非 PE"
        f.seek(0x3C)
        off = struct.unpack("<I", f.read(4))[0]
        f.seek(off)
        if f.read(4) != b"PE\0\0":
            return 0, "PE 签名错误"
        m = struct.unpack("<H", f.read(2))[0]
    return m, {0x8664: "PE32+ x64", 0x14C: "PE32 x86"}.get(m, hex(m))


def main():
    if not os.path.isfile(EXE):
        print("FAIL 找不到产物：%s" % EXE)
        return 2
    st = os.stat(EXE)
    import datetime
    mach, desc = pe_machine(EXE)
    print("EXE  %d B  %s  %s" % (st.st_size, datetime.datetime.fromtimestamp(st.st_mtime), desc))
    print("-" * 78)
    data = open(EXE, "rb").read()
    failed = []

    def check(desc_, s, enc, want):
        # 编码无关：QStringLiteral 落 UTF-16LE、qDebug/资源名/信号名可能是 ASCII，
        # moc 信号名与 qrc 资源名还有各自形态 —— 任一编码命中即视为"存在于 exe"。
        found = (data.find(s.encode("utf-16-le")) >= 0) or (data.find(s.encode("utf-8")) >= 0)
        ok = found if want else (not found)
        tag = "PASS" if ok else "FAIL"
        if not ok:
            failed.append(desc_)
        print("%-4s %-3s %-30s [%s] %s" % (tag, "有" if want else "无", desc_, enc, s))

    print("### 正向：既有功能必须存在 ###")
    for d, s, e in PRESENT:
        check(d, s, e, True)
    print()
    print("### 反向：已回退内容不得残留 ###")
    for d, s, e in ABSENT:
        check(d, s, e, False)

    print("-" * 78)
    print("正向 %d 条 + 反向 %d 条；失败 %d 条"
          % (len(PRESENT), len(ABSENT), len(failed)))
    if failed:
        print("失败项：" + "、".join(failed))
    print("---- %d/%d passed ----" % (len(PRESENT) + len(ABSENT) - len(failed),
                                       len(PRESENT) + len(ABSENT)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
