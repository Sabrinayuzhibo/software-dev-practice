#pragma once

#include <QJsonObject>

namespace backup::agent
{
inline constexpr qint64 kHistoryPageBytes = 64 * 1024;
inline constexpr qint64 kAgentReplyBytes = 2 * 1024 * 1024;

// Strip warning arrays from old and new records without reading shared data.
QJsonObject operationDetail(QJsonObject record);
QJsonObject operationSummary(const QJsonObject& record);
} // namespace backup::agent
