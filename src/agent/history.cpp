#include "backup/agent/history.h"
#include "backup/agent/state.h"
#include "backup/core/io.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace backup::agent
{
using namespace core;
namespace
{
QJsonArray inlineWarnings(const QJsonObject& record)
{
    if (record.value("warnings").isArray())
    {
        return record.value("warnings").toArray();
    }
    const auto result = record.value("result").toObject();
    if (result.value("warnings").isArray())
    {
        return result.value("warnings").toArray();
    }
    return result.value("version").toObject().value("warnings").toArray();
}

bool hasInlineWarnings(const QJsonObject& record)
{
    const auto result = record.value("result").toObject();
    return record.contains("warnings") || result.contains("warnings") ||
           result.value("version").toObject().contains("warnings");
}

QList<QByteArray> warningLines(const QJsonArray& warnings)
{
    QList<QByteArray> lines;
    for (const auto& value : warnings)
    {
        require(value.isObject(), "Invalid warning entry");
        const auto item = value.toObject();
        QJsonObject normalized{{"path", text(item, "path")},
                               {"reason", text(item, "reason")}};
        if (item.value("severity") == "error")
        {
            normalized.insert("severity", "error");
        }
        if (item.contains("type"))
        {
            normalized.insert("type", text(item, "type"));
        }
        auto line = QJsonDocument(normalized).toJson(QJsonDocument::Compact);
        require(line.size() + 1 < kHistoryPageBytes / 2,
                "Warning entry exceeds supported size");
        lines.append(line + '\n');
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

// Read with bounded buffers and verify the whole immutable object. A page is
// returned only after its reference and total count have been checked.
QJsonArray readWarningPage(const QString& directory, const QString& reference,
                           qint64 total, qint64 offset)
{
    static const QRegularExpression digest("^[0-9a-f]{64}$");
    require(digest.match(reference).hasMatch(), "Invalid warning reference");
    QFile file(directory + "/warnings/" + reference + ".jsonl");
    require(file.open(QIODevice::ReadOnly), "Cannot read warning details");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QJsonArray page;
    qint64 count = 0;
    qint64 bytes = 0;
    bool full = false;
    while (!file.atEnd())
    {
        const auto line = file.readLine(kHistoryPageBytes);
        require(!line.isEmpty() && line.endsWith('\n'),
                "Invalid warning details");
        hash.addData(line);
        if (count >= offset && !full)
        {
            full = page.size() >= kPageSize ||
                   bytes + line.size() > kHistoryPageBytes;
            if (!full)
            {
                page.append(parseObject(line));
                bytes += line.size();
            }
        }
        ++count;
    }
    require(file.error() == QFileDevice::NoError && count == total &&
                QString::fromLatin1(hash.result().toHex()) == reference,
            "Warning details failed integrity verification");
    return page;
}

QString storeWarnings(const QString& directory, const QJsonObject& record)
{
    const auto lines = warningLines(inlineWarnings(record));
    const auto result = record.value("result").toObject();
    // Older records repeat these arrays. Refuse to discard conflicting data.
    for (const auto& object :
         {record, result, result.value("version").toObject()})
    {
        if (object.contains("warnings"))
        {
            require(object.value("warnings").isArray() &&
                        warningLines(object.value("warnings").toArray()) ==
                            lines,
                    "Conflicting warning details in execution record");
        }
    }
    if (lines.isEmpty())
    {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& line : lines)
    {
        hash.addData(line);
    }
    const auto reference = QString::fromLatin1(hash.result().toHex());
    const auto folder = directory + "/warnings";
    makeDirectory(folder);
    const auto path = folder + "/" + reference + ".jsonl";
    if (!QFile::exists(path))
    {
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        require(file.open(QIODevice::WriteOnly), "Cannot save warning details");
        require(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
                "Cannot protect warning details");
        for (const auto& line : lines)
        {
            require(file.write(line) == line.size(),
                    "Cannot write warning details");
        }
        require(file.commit(), "Cannot commit warning details");
        syncDirectory(folder);
    }
    readWarningPage(directory, reference, lines.size(), lines.size());
    return reference;
}

QString scanGroup(const QJsonObject& record)
{
    const QJsonArray identity{record.value("target").toObject().value("id"),
                              record.value("task_id"), record.value("source")};
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument(identity).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
            .toHex());
}

QJsonObject recordsPage(const QList<QJsonObject>& records, qint64 offset,
                        qsizetype executions)
{
    QJsonArray page;
    qint64 bytes = 0;
    for (qint64 index = offset;
         index < records.size() && page.size() < kPageSize; ++index)
    {
        const auto& item = records[index];
        const auto size =
            QJsonDocument(item).toJson(QJsonDocument::Compact).size();
        require(size < kHistoryPageBytes,
                "Execution summary exceeds supported size");
        if (bytes + size + 1 > kHistoryPageBytes)
        {
            break;
        }
        page.append(item);
        bytes += size + 1;
    }
    const auto next = offset + page.size();
    return {{"operations", page},
            {"total", records.size()},
            {"execution_total", executions},
            {"next_offset",
             next < records.size() ? QJsonValue(next) : QJsonValue()}};
}
} // namespace

QJsonObject operationDetail(QJsonObject record)
{
    const auto count = hasInlineWarnings(record)
                           ? inlineWarnings(record).size()
                           : record.value("warning_count").toInteger();
    record.remove("warnings");
    record.insert("warning_count", count);
    auto result = record.value("result").toObject();
    if (!result.isEmpty())
    {
        result.remove("warnings");
        result.insert("warning_count", count);
        auto version = result.value("version").toObject();
        if (!version.isEmpty())
        {
            version.remove("warnings");
            version.insert("warning_count", count);
            result.insert("version", version);
        }
        record.insert("result", result);
    }
    return record;
}

QJsonObject operationSummary(const QJsonObject& record)
{
    const auto detail = operationDetail(record);
    QJsonObject summary;
    for (const auto* key :
         {"id",          "action",          "state",         "started_at",
          "finished_at", "stage",           "files",         "bytes",
          "directories", "task_id",         "target",        "source",
          "selection",   "file_types",      "preserve_empty_dirs", "source_name",   "destination",   "path",
          "error",       "version_id",      "warning_count", "error_count",
          "complete",    "restore_journal", "symlinks",      "hardlinks",
          "fifos",       "character_devices", "block_devices", "sockets",
          "stored_bytes", "entries"})
    {
        if (detail.contains(QLatin1String(key)))
        {
            summary.insert(QLatin1String(key),
                           detail.value(QLatin1String(key)));
        }
    }
    if (summary.value("action") == "scan")
    {
        summary.insert("scan_group", scanGroup(summary));
    }
    return summary;
}

void State::saveOperation(const QJsonObject& record) const
{
    const QString id = text(record, "id");
    require(validId(id), "Invalid operation identifier");
    auto compact = operationDetail(record);
    if (hasInlineWarnings(record))
    {
        const auto reference = storeWarnings(directory_, record);
        if (reference.isEmpty())
        {
            compact.remove("warning_ref");
        }
        else
        {
            compact.insert("warning_ref", reference);
        }
    }
    // The shared data is durable and verified before replacing the record.
    writeObject(directory_ + "/operations/" + id + ".json", compact);
}

void State::compactOperationWarnings() const
{
    for (const auto& file :
         QDir(directory_ + "/operations").entryList({"*.json"}, QDir::Files))
    {
        try
        {
            const auto record = readObject(directory_ + "/operations/" + file);
            if (hasInlineWarnings(record))
            {
                saveOperation(record);
            }
        }
        catch (const std::exception& error)
        {
            // Compaction is optional: an unchanged legacy record remains
            // readable.
            qWarning().noquote()
                << "Execution record compaction deferred:" << file
                << error.what();
        }
    }
}

QJsonObject State::operation(const QString& id) const
{
    require(validId(id), "Invalid operation identifier");
    return readObject(directory_ + "/operations/" + id + ".json");
}

QJsonObject State::operationWarnings(const QJsonObject& args) const
{
    const auto record = operation(text(args, "id"));
    const auto offset = args.contains("offset") ? number(args, "offset") : 0;
    QJsonArray page;
    qint64 total = 0;
    if (hasInlineWarnings(record))
    {
        const auto lines = warningLines(inlineWarnings(record));
        total = lines.size();
        qint64 bytes = 0;
        for (qint64 index = offset; index < total && page.size() < kPageSize;
             ++index)
        {
            if (bytes + lines[index].size() > kHistoryPageBytes)
            {
                break;
            }
            page.append(parseObject(lines[index]));
            bytes += lines[index].size();
        }
    }
    else
    {
        total = record.value("warning_count").toInteger();
        if (total)
        {
            page = readWarningPage(directory_, text(record, "warning_ref"),
                                   total, offset);
        }
    }
    const auto next = offset + page.size();
    return {{"warnings", page},
            {"total", total},
            {"next_offset", next < total ? QJsonValue(next) : QJsonValue()}};
}

QJsonObject State::operations(const QJsonObject& args) const
{
    QList<QJsonObject> records;
    const auto filter = args.value("scan_group").toString();
    for (const auto& file :
         QDir(directory_ + "/operations").entryList({"*.json"}, QDir::Files))
    {
        auto record =
            operationSummary(readObject(directory_ + "/operations/" + file));
        if (filter.isEmpty() || record.value("scan_group") == filter)
        {
            records.append(record);
        }
    }
    std::sort(records.begin(), records.end(),
              [](const auto& left, const auto& right)
              {
                  const auto first = left.value("started_at").toString();
                  const auto second = right.value("started_at").toString();
                  return first == second ? left.value("id").toString() >
                                               right.value("id").toString()
                                         : first > second;
              });
    const auto executions = records.size();
    if (args.value("group_scans").toBool() && filter.isEmpty())
    {
        QList<QJsonObject> groups;
        QMap<QString, qsizetype> positions;
        for (const auto& record : records)
        {
            if (record.value("action") != "scan")
            {
                groups.append(record);
                continue;
            }
            const auto key = text(record, "scan_group");
            if (!positions.contains(key))
            {
                positions.insert(key, groups.size());
                groups.append(record);
            }
            auto& group = groups[positions.value(key)];
            group.insert("scan_count", group.value("scan_count").toInt() + 1);
            const auto state = record.value("state").toString();
            group.insert("failure_count",
                         group.value("failure_count").toInt() +
                             (state == "FAILED" || state == "INTERRUPTED"));
            group.insert("warning_runs",
                         group.value("warning_runs").toInt() +
                             (record.value("warning_count").toInteger() > 0));
        }
        records = groups;
    }
    const auto offset = args.contains("offset") ? number(args, "offset") : 0;
    return recordsPage(records, offset, executions);
}
} // namespace backup::agent
