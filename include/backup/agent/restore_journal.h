#pragma once

#include <QFile>
#include <QJsonObject>

namespace backup::agent
{
// Intent is durable before publishing; an interrupted intent remains explicitly
// uncertain. A completed entry is durable before reporting progress.
class RestoreJournal
{
  public:
    RestoreJournal(const QString& directory, const QString& operationId);
    void record(const QJsonObject& entry, const QString& state);

  private:
    QFile file_;
};

QJsonObject restoreEntries(const QString& directory, const QJsonObject& args);
} // namespace backup::agent
