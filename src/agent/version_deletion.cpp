#include "backup/agent/channel.h"
#include "backup/agent/operations.h"
#include "backup/core/io.h"

#include <QThread>
#include <memory>

namespace backup::agent
{
using namespace core;

QJsonObject deleteVersion(const QJsonObject& args, const QJsonObject& target,
                          const Progress& progress)
{
    const auto id = text(args, "version_id");
    require(validId(id), "Invalid version identifier");
    auto channel = std::make_unique<Channel>(target);
    channel->authenticate();
    const auto root = canonicalPath(text(channel->health(), "data_root"));
    const auto expected = args.value("repository_path").toString();
    require(expected.isEmpty() || canonicalPath(expected) == root,
            "Repository changed; select the version again");
    // Pin the repository for both reconnection and subsequent record retries.
    auto pinnedTarget = target;
    pinnedTarget.insert("repository_path", root);
    QJsonObject status{{"stage", "delete"},
                       {"version_id", id},
                       {"target", pinnedTarget},
                       {"files", 0},
                       {"bytes", 0}};
    progress(status);
    while (true)
    {
        require(!QThread::currentThread()->isInterruptionRequested(),
                "DELETE_UNCERTAIN: Deletion interrupted; retry this operation");
        QJsonObject result;
        try
        {
            result = channel->call("delete_version", {{"version_id", id}});
        }
        catch (const ServerError&)
        {
            throw;
        }
        catch (const std::exception&)
        {
            // The first request may have committed. Reuse the version ID;
            // the Server's durable receipt makes this retry idempotent.
            try
            {
                channel = std::make_unique<Channel>(pinnedTarget);
                channel->authenticate();
                result = channel->call("delete_version", {{"version_id", id}});
            }
            catch (const std::exception& failure)
            {
                throw Error("DELETE_UNCERTAIN: " +
                            QString::fromUtf8(failure.what()));
            }
        }
        require(result.value("version_id") == id &&
                    result.value("version").toObject().value("id") == id &&
                    result.value("done").isBool(),
                "DELETE_UNCERTAIN: Invalid deletion receipt");
        const auto version = result.value("version").toObject();
        for (const auto* field :
             {"source", "source_name", "task_id", "selection"})
        {
            if (version.contains(QLatin1String(field)))
            {
                status.insert(QLatin1String(field), version.value(field));
            }
        }
        if (result.value("done").toBool())
        {
            status.insert("files", version.value("files"));
            status.insert("bytes", version.value("bytes"));
            progress(status);
            return status;
        }
        progress(status);
    }
}
} // namespace backup::agent
