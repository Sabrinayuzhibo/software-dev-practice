#pragma once

// Worker-only data operations. Progress carries phase, path and byte counters.
#include <QJsonObject>
#include <functional>

namespace backup::agent
{
using Progress = std::function<void(const QJsonObject&)>;
QJsonObject scan(const QJsonObject& source, const Progress& progress);
QJsonObject backup(const QJsonObject& task, const QJsonObject& target,
                   const QString& operationId, const Progress& progress);
QJsonObject restore(const QJsonObject& args, const QJsonObject& target,
                    const QJsonArray& tasks, const QJsonArray& targets,
                    const QString& stateDirectory, const Progress& progress);
// 导出完整版本为独立归档；目标不存在，路径检查和发布均禁止覆盖。
QJsonObject exportArchive(const QJsonObject& args, const QJsonObject& target,
                          const QJsonArray& tasks, const QJsonArray& targets,
                          const QString& stateDirectory,
                          const Progress& progress);
// 先验证私有归档快照，再按目标策略导入为新版本；失败不发布半成品。
QJsonObject importArchive(const QJsonObject& args, const QJsonObject& target,
                          const Progress& progress);
// Delete only the selected version; uncertain transport outcomes are retryable.
QJsonObject deleteVersion(const QJsonObject& args, const QJsonObject& target,
                          const Progress& progress);
} // namespace backup::agent
