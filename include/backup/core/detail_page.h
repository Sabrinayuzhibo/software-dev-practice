#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace backup::core
{
inline constexpr qint64 kDetailPageBytes = 64 * 1024;

// Pages use item offsets, a byte limit and a count limit. Invalid or damaged
// input throws Error; callers rename "items" for their wire interface.
QJsonObject detailPage(const QJsonArray& items, qint64 offset);
QJsonObject readDetailPage(const QString& path, qint64 offset, qint64 count,
                           const QString& sha256);
QByteArray detailLine(const QJsonObject& item);
} // namespace backup::core
