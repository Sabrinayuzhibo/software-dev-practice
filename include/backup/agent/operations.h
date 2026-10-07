#pragma once

// Worker-only data operations. Progress carries phase, path and byte counters.
#include <QJsonObject>
#include <functional>

namespace backup::agent
{
using Progress = std::function<void(const QJsonObject&)>;
QJsonObject scan(const QString& source, const Progress& progress);
QJsonObject backup(const QJsonObject& task, const QJsonObject& target,
                   const QString& operationId, const Progress& progress);
QJsonObject restore(const QJsonObject& args, const QJsonObject& target,
                    const QJsonArray& tasks, const QString& stateDirectory,
                    const Progress& progress);
} // namespace backup::agent
