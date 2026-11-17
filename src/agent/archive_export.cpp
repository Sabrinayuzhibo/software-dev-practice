#include "archive_io.h"
#include "backup/agent/channel.h"
#include "backup/agent/worker_support.h"
#include "backup/core/archive.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <fcntl.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
void checkExportDestination(const QString& destination,
                            const QJsonObject& summary,
                            const QString& repository,
                            const QString& stateDirectory,
                            const QJsonArray& tasks, const QJsonArray& targets)
{
    require(!overlaps(destination, canonicalPath(repository)) &&
                !overlaps(destination, canonicalPath(stateDirectory)),
            "Archive destination overlaps repository or Agent state");
    require(!SourceScope(summary, "source").overlaps(destination),
            "Archive destination overlaps the saved source");
    for (const auto& task : tasks)
    {
        require(!SourceScope(task.toObject()).overlaps(destination),
                "Archive destination overlaps a configured source");
    }
    for (const auto& value : targets)
    {
        for (const auto* field : {"data_root", "repository_path"})
        {
            const auto path =
                value.toObject().value(QLatin1String(field)).toString();
            require(path.isEmpty() ||
                        !overlaps(destination, canonicalPath(path)),
                    "Archive destination overlaps a configured repository");
        }
    }
}

void exportWarnings(Channel& channel, const QString& id, ArchiveWriter& writer)
{
    qint64 offset = 0;
    do
    {
        checkOperationCancelled();
        const auto page = channel.call(
            "version_warnings", {{"version_id", id}, {"offset", offset}});
        require(page.value("warnings").isArray(),
                "Invalid version warning page");
        for (const auto& value : page.value("warnings").toArray())
        {
            require(value.isObject(), "Invalid version warning");
            writer.write(
                ArchiveRecord::Warning,
                QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
            ++offset;
        }
        if (page.value("next_offset").isNull())
        {
            break;
        }
        require(number(page, "next_offset") == offset &&
                    !page.value("warnings").toArray().isEmpty(),
                "Version warning cursor did not advance");
    } while (true);
}

void exportContent(Channel& channel, const QString& id,
                   const QJsonObject& entry, ArchiveWriter& writer,
                   QJsonObject& status, const Progress& progress)
{
    qint64 offset = 0;
    const auto storedSize = entry.contains("stored_size")
                                ? number(entry, "stored_size")
                                : number(entry, "size");
    while (true)
    {
        checkOperationCancelled();
        const auto reply =
            channel.call("download", {{"version_id", id},
                                      {"index", entry.value("index")},
                                      {"offset", QString::number(offset)}});
        require(reply.value("data").isString(),
                "Invalid archive download data");
        const auto encoded = reply.value("data").toString();
        require(encoded.size() <= kChunkBytes * 4 / 3 + 4,
                "Archive download block is too large");
        const auto bytes = QByteArray::fromBase64Encoding(
            encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        require(bool(bytes) && bytes.decoded.size() <= kChunkBytes &&
                    bytes.decoded.size() <= storedSize - offset &&
                    number(reply, "offset") == offset &&
                    reply.value("done").isBool(),
                "Invalid archive download block");
        if (!bytes.decoded.isEmpty())
        {
            writer.write(ArchiveRecord::Data, bytes.decoded);
            offset += bytes.decoded.size();
            status.insert("bytes",
                          number(status, "bytes") + bytes.decoded.size());
            progress(status);
        }
        if (reply.value("done").toBool())
        {
            break;
        }
        require(!bytes.decoded.isEmpty(),
                "Empty non-final archive download block");
    }
    require(offset == storedSize, "Incomplete archive content");
    writer.write(ArchiveRecord::FileEnd, {});
}

void exportEntries(Channel& channel, const QString& id,
                   const QJsonObject& summary, ArchiveWriter& writer,
                   QJsonObject& status, const Progress& progress)
{
    qint64 offset = 0;
    qint64 count = 0;
    while (true)
    {
        checkOperationCancelled();
        const auto page =
            channel.call("entries", {{"version_id", id},
                                     {"offset", QString::number(offset)}});
        require(page.value("entries").isArray() && page.value("done").isBool(),
                "Invalid manifest page");
        for (const auto& value : page.value("entries").toArray())
        {
            require(value.isObject() && count < number(summary, "entries"),
                    "Invalid archive entry count");
            const auto entry = value.toObject();
            status.insert("path", text(entry, "path"));
            writer.write(ArchiveRecord::Entry,
                         QJsonDocument(entry).toJson(QJsonDocument::Compact));
            if (entry.value("type") == "file")
            {
                exportContent(channel, id, entry, writer, status, progress);
            }
            ++count;
            if (entry.value("type") == "file" ||
                entry.value("type") == "hardlink")
            {
                status.insert("files", number(status, "files") + 1);
            }
            progress(status);
        }
        const auto next = number(page, "offset");
        if (page.value("done").toBool())
        {
            break;
        }
        require(next > offset, "Manifest cursor did not advance");
        offset = next;
    }
    require(count == number(summary, "entries"), "Incomplete archive manifest");
}
} // 匿名命名空间

QJsonObject exportArchive(const QJsonObject& args, const QJsonObject& target,
                          const QJsonArray& tasks, const QJsonArray& targets,
                          const QString& stateDirectory,
                          const Progress& progress)
{
    const auto destination = archivePath(text(args, "archive_path"));
    require(!QFileInfo::exists(destination),
            "Archive destination already exists");
    Channel channel(target);
    channel.authenticate();
    const auto id = text(args, "version_id");
    const auto summary = channel.call("version", {{"version_id", id}});
    checkExportDestination(destination, summary,
                           text(channel.health(), "data_root"), stateDirectory,
                           tasks, targets);
    Descriptor filesystem(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const auto directory = QFileInfo(destination).absolutePath();
    Descriptor parent(directory == "/"
                          ? ::dup(filesystem.get())
                          : openBelow(filesystem.get(), directory.mid(1),
                                      O_RDONLY | O_DIRECTORY));
    const auto leaf = QFileInfo(destination).fileName().toUtf8();
    require(!leaf.isEmpty(), "Invalid archive filename");
    QTemporaryFile temporary("/proc/self/fd/" + QString::number(parent.get()) +
                             "/.backup-archive-XXXXXX");
    require(temporary.open(), "Cannot create temporary archive");
    ArchiveWriter writer(temporary);
    writer.write(ArchiveRecord::Summary,
                 QJsonDocument(summary).toJson(QJsonDocument::Compact));
    exportWarnings(channel, id, writer);
    QJsonObject status{{"stage", "archive_export"}, {"files", 0}, {"bytes", 0}};
    exportEntries(channel, id, summary, writer, status, progress);
    writer.finish();
    require(temporary.flush() && temporary.seek(0),
            "Cannot verify exported archive");
    QJsonArray warnings;
    ArchiveCallbacks callbacks;
    callbacks.checkpoint = [&] { checkOperationCancelled(); };
    callbacks.warning = [&](const QJsonObject& warning)
    { warnings.append(warning); };
    progress({{"stage", "archive_verify"}});
    const auto info = verifyArchive(temporary, callbacks);
    syncFile(temporary);
    checkOperationCancelled();
    const auto pinned =
        "/proc/self/fd/" + QByteArray::number(temporary.handle());
    require(::linkat(AT_FDCWD, pinned.constData(), parent.get(),
                     leaf.constData(), AT_SYMLINK_FOLLOW) == 0,
            "Cannot publish archive without overwriting: " + destination);
    require(temporary.remove() && ::fsync(parent.get()) == 0,
            "Cannot persist exported archive: " + destination);
    return {{"archive_path", destination},
            {"archive_bytes", QString::number(info.bytes)},
            {"files", number(summary, "files")},
            {"bytes", number(summary, "bytes")},
            {"warnings", warnings},
            {"version_id", id}};
}
} // 命名空间 backup::agent
