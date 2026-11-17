#pragma once

#include "backup/agent/operations.h"
#include "backup/agent/source_sink.h"
#include <QHash>
#include <QSet>
#include <map>
#include <sys/stat.h>

namespace backup::agent
{
// 内部条目编码器，借用根目录 fd、接收者和进度回调。
void incrementSourceCounter(QJsonObject& status, const char* field,
                            qint64 count = 1);
QString sourceEntryType(mode_t mode);
bool sameSourceDirectoryState(const struct stat& before,
                              const struct stat& after);

class SourceEntries
{
  public:
    SourceEntries(int root, ISourceSink& sink, const Progress& progress)
        : root_(root), sink_(sink), progress_(progress)
    {
    }
    QJsonObject process(const QString& path, const struct stat& info,
                        QJsonObject& status, bool structural = false);
    void verify() const;

  private:
    QJsonObject regular(const QString& path, const struct stat& info,
                        QJsonObject& status);
    int root_;
    ISourceSink& sink_;
    const Progress& progress_;
    std::map<std::pair<dev_t, ino_t>, QJsonObject> groups_;
    QHash<QString, struct stat> snapshots_;
    QSet<QString> structural_;
};

} // namespace backup::agent
