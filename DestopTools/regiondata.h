/*
 * @file regiondata.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef REGIONDATA_H
#define REGIONDATA_H

#include <QStringList>

/**
 * @brief 行政区划数据访问封装。
 *
 * 数据来自 region.json（省 -> 市 -> 区县 三级结构），程序启动时加载一次。
 * 若 JSON 加载失败，province/city/district 查询会返回空列表，调用方需自行兜底。
 */
class RegionData {
public:
    // / 从候选路径加载 region.json；成功返回 true。preferredPath 优先，
    // / 其次尝试可执行文件所在目录、当前工作目录。
    static bool load(const QString& preferredPath = QString());

    // / 是否已成功加载数据。
    static bool isLoaded();

    // / 全国省级行政区名称列表（按 region.json 中顺序）。
    static QStringList provinces();

    // / 指定省下的所有地级行政区名称（含直辖市的市辖区聚合项）。
    static QStringList cities(const QString& province);

    // / 指定省、市下的所有区/县/旗名称；找不到返回空列表。
    static QStringList districts(const QString& province, const QString& city);
};

#endif // REGIONDATA_H
