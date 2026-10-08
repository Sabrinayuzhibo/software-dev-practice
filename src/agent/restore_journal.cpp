#include "backup/agent/restore_journal.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"

#include <QHash>
#include <QStringList>

namespace backup::agent
{
using namespace core;
namespace
{
QString journalPath(const QString& directory, const QString& id)
{
    require(validId(id), "Invalid restore operation identifier");
    return directory + "/restores/" + id + ".jsonl";
}
} // namespace

RestoreJournal::RestoreJournal(const QString& directory,
                               const QString& operationId)
    : file_(journalPath(directory, operationId))
{
    makeDirectory(directory + "/restores");
    require(file_.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            "Cannot create restore journal");
    require(file_.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
            "Cannot protect restore journal");
    syncFile(file_);
    syncDirectory(directory + "/restores");
    syncDirectory(directory);
}

void RestoreJournal::record(const QJsonObject& entry, const QString& state)
{
    QJsonObject event{{"path", entry.value("path")},
                      {"type", entry.value("type")},
                      {"state", state}};
    if (entry.contains("temporary_path"))
    {
        event.insert("temporary_path", entry.value("temporary_path"));
    }
    writeAll(file_, detailLine(event));
    syncFile(file_);
}

QJsonObject restoreEntries(const QString& directory, const QJsonObject& args)
{
    QFile file(journalPath(directory, text(args, "id")));
    require(file.open(QIODevice::ReadOnly), "Restore journal is unavailable");
    QHash<QString, QJsonObject> latest;
    QStringList order;
    bool incomplete = false;
    while (!file.atEnd())
    {
        const auto line = file.readLine(kDetailPageBytes);
        if (!line.endsWith('\n') && file.atEnd())
        {
            // A crash may leave a torn final event, never invent its outcome.
            incomplete = true;
            break;
        }
        require(line.endsWith('\n'), "Invalid restore journal entry");
        const auto item = parseObject(line);
        const auto path = text(item, "path");
        require(validPath(path) && supportedEntryType(text(item, "type")) &&
                    (!item.contains("temporary_path") ||
                     validPath(text(item, "temporary_path"))) &&
                    (item.value("state") == "pending" ||
                     item.value("state") == "written"),
                "Invalid restore journal data");
        if (!latest.contains(path))
        {
            order.append(path);
        }
        latest.insert(path, item);
    }
    require(file.error() == QFileDevice::NoError,
            "Cannot read restore journal");
    QJsonArray items;
    qint64 written = 0;
    for (const auto& path : order)
    {
        const auto item = latest.value(path);
        written += item.value("state") == "written";
        items.append(item);
    }
    auto page =
        detailPage(items, args.contains("offset") ? number(args, "offset") : 0);
    page.insert("entries", page.take("items"));
    page.insert("written", written);
    page.insert("uncertain", items.size() - written);
    page.insert("incomplete", incomplete);
    return page;
}
} // namespace backup::agent
