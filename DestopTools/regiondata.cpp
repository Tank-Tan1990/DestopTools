/*
 * @file regiondata.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "regiondata.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QCoreApplication>

namespace {
    QJsonObject s_root;       // 省 -> (市 -> [区县...])
    bool s_loaded = false;

    QStringList readArray(const QJsonArray& arr) {
        QStringList result;
        result.reserve(arr.size());
        for (const QJsonValue& v : arr) {
            result.append(v.toString());
        }
        return result;
    }
}

// / 其次尝试可执行文件所在目录、当前工作目录。
// 作者：谭征
bool RegionData::load(const QString& preferredPath) {
    QStringList candidates;
    if (!preferredPath.isEmpty()) {
        candidates.append(preferredPath);
    }
    // 可执行文件所在目录（发布形态 exe 旁）
    QString appDir = QCoreApplication::applicationDirPath();
    candidates.append(QDir(appDir).filePath(QStringLiteral("region.json")));
    // 当前工作目录
    candidates.append(QDir::currentPath() + QStringLiteral("/region.json"));
    // 源码 / 构建目录（开发调试期，源码与 exe 不在同目录时兜底）
    candidates.append(QDir(appDir).filePath(QStringLiteral("../region.json")));
    candidates.append(QDir(appDir).filePath(QStringLiteral("../../region.json")));

    for (const QString& path : candidates) {
        QFile f(path);
        if (!f.exists()) {
            continue;
        }
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        QByteArray data = f.readAll();
        f.close();

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        s_root = doc.object();
        s_loaded = true;
        return true;
    }

    s_loaded = false;
    return false;
}

// / 是否已成功加载数据。
// 作者：谭征
bool RegionData::isLoaded() {
    return s_loaded;
}

// / 全国省级行政区名称列表（按 region.json 中顺序）。
// 作者：谭征
QStringList RegionData::provinces() {
    if (!s_loaded) {
        return QStringList();
    }
    return s_root.keys();
}

// / 指定省下的所有地级行政区名称（含直辖市的市辖区聚合项）。
// 作者：谭征
QStringList RegionData::cities(const QString& province) {
    if (!s_loaded) {
        return QStringList();
    }
    QJsonValue pv = s_root.value(province);
    if (!pv.isObject()) {
        return QStringList();
    }
    return pv.toObject().keys();
}

// / 指定省、市下的所有区/县/旗名称；找不到返回空列表。
// 作者：谭征
QStringList RegionData::districts(const QString& province, const QString& city) {
    if (!s_loaded) {
        return QStringList();
    }
    QJsonValue pv = s_root.value(province);
    if (!pv.isObject()) {
        return QStringList();
    }
    QJsonValue cv = pv.toObject().value(city);
    // 第三级直接是数组（省 -> 市 -> [区县...]）
    if (cv.isArray()) {
        return readArray(cv.toArray());
    }
    // 兼容对象包装（市 -> {"__list": [...]}）
    if (cv.isObject()) {
        QJsonValue arr = cv.toObject().value(QStringLiteral("__list"));
        if (arr.isArray()) {
            return readArray(arr.toArray());
        }
    }
    return QStringList();
}
