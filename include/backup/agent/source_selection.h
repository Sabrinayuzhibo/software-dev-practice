#pragma once

#include <QJsonArray>
#include <QJsonObject>

namespace backup::agent
{
// Normalize explicit source paths, merge duplicates/covered descendants and
// return a scope plus rows describing restore paths. Invalid sources throw.
QJsonObject prepareSources(const QJsonArray& paths,
                           const QJsonValue& fileTypes,
                           const QJsonValue& preserveEmptyDirs);
} // namespace backup::agent
