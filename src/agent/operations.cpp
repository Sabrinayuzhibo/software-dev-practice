#include "backup/agent/operations.h"
#include "backup/agent/channel.h"
#include "backup/agent/source_tree.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"

#include <QThread>

namespace backup::agent
{
using namespace core;

QJsonObject scan(const QJsonObject& source, const Progress& progress)
{
    try
    {
        return sourceTree(source, nullptr, progress);
    }
    catch (const std::exception& failure)
    {
        require(!QThread::currentThread()->isInterruptionRequested(),
                "Operation interrupted");
        return {{"root", source.value("path")},
                {"files", 0},
                {"directories", 0},
                {"bytes", 0},
                {"complete", false},
                {"error_count", 1},
                {"sample", QJsonArray{}},
                {"warnings", QJsonArray{QJsonObject{
                                 {"path", "."},
                                 {"reason", QString::fromUtf8(failure.what())},
                                 {"severity", "error"}}}}};
    }
}

QJsonObject backup(const QJsonObject& task, const QJsonObject& target,
                   const QString& operationId, const Progress& progress)
{
    Channel channel(target);
    channel.authenticate();
    const SourceScope source(task);
    require(
        !source.overlaps(canonicalPath(text(channel.health(), "data_root"))),
        "Source overlaps repository");
    auto request = source.json("source");
    request.insert("operation_id", operationId);
    request.insert("task_id", text(task, "id"));
    if (task.contains("name"))
    {
        request.insert("source_name", task.value("name"));
    }
    const auto receipt = channel.call("begin", request);
    require(source.selection().isEmpty() ||
                receipt.value("selection") == source.selection(),
            "Server does not support selected sources; update the Server");
    require(source.fileTypes().isEmpty() ||
                receipt.value("file_types") == source.fileTypes(),
            "Server does not support file type filters; update the Server");
    require(!source.hasEmptyDirectoryRule() ||
                receipt.value("preserve_empty_dirs") ==
                    source.preservesEmptyDirectories(),
            "Server does not support empty directory rules; update the Server");
    auto result = sourceTree(task, &channel, progress);
    const auto warnings = result.value("warnings").toArray();
    qint64 warningOffset = 0;
    while (warningOffset < warnings.size())
    {
        const auto page = detailPage(warnings, warningOffset);
        const auto items = page.value("items").toArray();
        channel.call("warnings",
                     {{"offset", warningOffset}, {"warnings", items}});
        warningOffset += items.size();
    }
    progress({{"stage", "commit"},
              {"files", result.value("files")},
              {"bytes", result.value("bytes")}});
    QJsonObject version;
    try
    {
        version = channel.call("commit", {{"operation_id", operationId}});
    }
    catch (const std::exception&)
    {
        // A lost commit acknowledgement is resolved by the same operation ID.
        progress({{"stage", "confirm"}});
        try
        {
            Channel confirmation(target);
            confirmation.authenticate();
            version =
                confirmation.call("version", {{"version_id", operationId}});
        }
        catch (const std::exception&)
        {
            throw Error("COMMIT_UNCERTAIN: result pending confirmation; query "
                        "this operation before starting another backup");
        }
    }
    result.insert("version", version);
    return result;
}
} // namespace backup::agent
