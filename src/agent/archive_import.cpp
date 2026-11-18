#include "archive_io.h"
#include "backup/agent/channel.h"
#include "backup/agent/source_sink.h"
#include "backup/agent/worker_support.h"
#include "backup/core/archive.h"
#include "backup/core/compression.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"
#include "sealed_archive.h"

#include <QJsonArray>

namespace backup::agent
{
using namespace core;
namespace
{
QJsonObject beginImport(Channel& channel, const QJsonObject& summary,
                        const QJsonObject& target, const QString& operationId)
{
    require(channel.health().value("archive_format").toInt() == kArchiveFormat,
            "Server does not support archive imports; update the Server");
    const SourceScope scope(summary, "source");
    auto request = scope.json("source");
    const auto compression = compressionAlgorithm(target);
    request.insert("compression", compression);
    request.insert("operation_id", operationId);
    request.insert("task_id", text(summary, "task_id"));
    request.insert("imported_from",
                   QJsonObject{{"archive_format", kArchiveFormat},
                               {"source_format", summary.value("format")},
                               {"version_id", text(summary, "id")}});
    for (const auto* field : {"source_name", "root_metadata"})
    {
        if (summary.contains(QLatin1String(field)))
        {
            request.insert(QLatin1String(field),
                           summary.value(QLatin1String(field)));
        }
    }
    const auto receipt = channel.call("begin_import", request);
    require(receipt.value("version_id") == operationId &&
                receipt.value("compression") == compression,
            "Server did not accept archive import policy");
    for (const auto* field :
         {"selection", "file_types", "preserve_empty_dirs", "filters"})
    {
        const auto key = QLatin1String(field);
        require(!request.contains(key) ||
                    receipt.value(key) == request.value(key),
                "Server did not accept archive source rules");
    }
    return receipt;
}

QJsonObject commitImport(Channel& channel, const QJsonObject& target,
                         const QString& operationId, const Progress& progress)
{
    checkOperationCancelled();
    progress({{"stage", "commit"}});
    try
    {
        return channel.call("commit", {{"operation_id", operationId}});
    }
    catch (const std::exception&)
    {
        progress({{"stage", "confirm"}});
        try
        {
            Channel confirmation(target);
            confirmation.authenticate();
            return confirmation.call("version", {{"version_id", operationId}});
        }
        catch (const std::exception&)
        {
            throw Error("COMMIT_UNCERTAIN: archive import result pending "
                        "confirmation; query this operation before retrying");
        }
    }
}
} // 匿名命名空间

QJsonObject importArchive(const QJsonObject& args, const QJsonObject& target,
                          const Progress& progress)
{
    const auto path = archivePath(text(args, "archive_path"));
    QTemporaryFile snapshot;
    snapshotArchive(path, snapshot, progress);
    ArchiveCallbacks callbacks;
    callbacks.checkpoint = [&] { checkOperationCancelled(); };
    QJsonArray warnings;
    callbacks.warning = [&](const QJsonObject& warning)
    { warnings.append(warning); };
    progress({{"stage", "archive_verify"}});
    const bool encrypted = SealedArchiveInput::detects(snapshot);
    const auto password = args.value("archive_password").toString();
    require(encrypted || password.isEmpty(),
            "Selected archive is not encrypted");
    const auto access = [&]() -> VaultAccess
    {
        if (!encrypted || !password.isEmpty())
        {
            return {};
        }
        Channel metadata(target);
        metadata.authenticate();
        return VaultKeys::access(
            metadata.health().value("encryption").toObject());
    }();
    const auto read = [&]
    {
        if (!encrypted)
        {
            return verifyArchive(snapshot, callbacks);
        }
        SealedArchiveInput input(snapshot, password, access);
        return verifyArchive(input, callbacks);
    };
    const auto verified = read();
    require(snapshot.seek(0), "Cannot rewind verified archive");
    // 从完整验证后的私有快照上传，源文件后续变化不影响此次导入。
    Channel channel(target);
    channel.authenticate();
    require(!encrypted ||
                !channel.health().value("encryption").toObject().isEmpty() ||
                args.value("allow_plain_import") == true,
            "ENCRYPTED_IMPORT_CONFIRM: importing here stores unencrypted data");
    const auto operationId = text(args, "operation_id");
    beginImport(channel, verified.summary, target, operationId);
    qint64 warningOffset = 0;
    while (warningOffset < warnings.size())
    {
        checkOperationCancelled();
        const auto page = detailPage(warnings, warningOffset);
        const auto items = page.value("items").toArray();
        channel.call("warnings",
                     {{"offset", warningOffset}, {"warnings", items}});
        warningOffset += items.size();
    }
    UploadSourceSink sink(channel);
    qint64 offset = 0;
    QString digest;
    QJsonObject status{{"stage", "archive_import"},
                       {"files", 0},
                       {"bytes", 0},
                       {"source", verified.summary.value("source")},
                       {"source_name", verified.summary.value("source_name")}};
    callbacks.warning = {};
    callbacks.entry = [&](const QJsonObject& entry)
    {
        sink.entry(entry);
        status.insert("path", text(entry, "path"));
        if (entry.value("type") == "file" || entry.value("type") == "hardlink")
        {
            status.insert("files", number(status, "files") + 1);
        }
        if (entry.value("type") == "file")
        {
            offset = 0;
            digest = text(entry, "sha256");
        }
        if (entry.value("type") == "hardlink")
        {
            status.insert("bytes",
                          number(status, "bytes") + number(entry, "size"));
        }
        progress(status);
    };
    callbacks.content = [&](const QByteArray& bytes)
    {
        sink.chunk(offset, bytes);
        offset += bytes.size();
        status.insert("bytes", number(status, "bytes") + bytes.size());
        progress(status);
    };
    callbacks.fileEnd = [&] { sink.finishFile(digest); };
    const auto replayed = read();
    require(replayed.digest == verified.digest,
            "Private archive snapshot changed during import");
    channel.prepareCommit();
    const auto version = commitImport(channel, target, operationId, progress);
    return {{"archive_path", path},
            {"archive_bytes", QString::number(verified.bytes)},
            {"files", number(version, "files")},
            {"bytes", number(version, "bytes")},
            {"warnings", warnings},
            {"version", version}};
}
} // 命名空间 backup::agent
