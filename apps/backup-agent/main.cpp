#include "backup/agent/control_service.h"
#include "backup/agent/operation_dispatch.h"
#include "backup/agent/realtime.h"
#include "backup/agent/remote_session.h"
#include "backup/agent/vault.h"
#include <QDir>
// 控制线程拥有配置和调度；单个工作线程执行备份并保存进度、结果。
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QThread>
#include <cstdio>
#include <memory>
#include <unistd.h>

#include "backup/agent/channel.h"
#include "backup/agent/history.h"
#include "backup/agent/operations.h"
#include "backup/agent/restore_journal.h"
#include "backup/agent/state.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"

namespace
{
using namespace backup::core;
using backup::agent::Channel;
using backup::agent::ChannelError;
using backup::agent::State;

bool isVaultCommand(const QString& action)
{
    return action == "initialize_encryption" || action == "unlock_repository" ||
           action == "lock_repository";
}

class CommandReader final : public QObject
{
  public:
    CommandReader(QCoreApplication* app, const QString& directory, bool daemon)
        : QObject(app), state_(directory),
          control_(directory, daemon,
                   [this](const QByteArray& line) { handle(line); }),
          realtime_([this] { return busy_; },
                    [this](const QString& id) {
                        start({}, "start_backup",
                              {{"task_id", id}, {"trigger", "realtime"}});
                    })
    {
        const auto config = state_.config();
        realtime_.configure(config);
        for (const auto& value : config.value("targets").toArray())
        {
            const auto id =
                value.toObject().value("repository_encryption_id").toString();
            if (!id.isEmpty())
            {
                realtime_.setRepositoryLocked(config, id, true);
            }
        }
        // 崩溃后的不确定提交必须先确认原版本，不能直接重新备份。
        for (const auto& name : QDir(state_.directory() + "/operations")
                                    .entryList({"*.json"}, QDir::Files))
        {
            const auto record =
                readObject(state_.directory() + "/operations/" + name);
            if (record.value("action") == "backup" &&
                record.value("state") == "WAITING")
            {
                realtime_.completed(record);
            }
        }
    }

    ~CommandReader() override
    {
        if (worker_)
        {
            worker_->requestInterruption();
            worker_->wait();
        }
    }

  private:
    void reply(const QString& id, bool ok, const QJsonObject& value)
    {
        control_.reply(id, ok, value);
    }

    void handle(const QByteArray& line)
    {
        QString id;
        try
        {
            require(line.size() <= kMaxJsonBytes, "Command too long");
            const auto command = parseObject(line);
            id = text(command, "id");
            const QString action = text(command, "action");
            require(command.value("payload").isObject(), "Missing payload");
            const auto args = command.value("payload").toObject();
            if (action == "config")
            {
                auto config = state_.config();
                config.insert("realtime_status", realtime_.status());
                config.insert("agent_busy", busy_);
                config.insert("agent_pid", QCoreApplication::applicationPid());
                reply(id, true, config);
            }
            else if (action == "operations")
            {
                reply(id, true, state_.operations(args));
            }
            else if (action == "operation")
            {
                const auto requested = text(args, "id");
                const auto record = lastRecord_.value("id") == requested
                                        ? lastRecord_
                                        : state_.operation(requested);
                reply(id, true,
                      args.value("summary").toBool()
                          ? backup::agent::operationSummary(record)
                          : backup::agent::operationDetail(record));
            }
            else if (action == "operation_warnings")
            {
                reply(id, true, state_.operationWarnings(args));
            }
            else if (action == "preview_sources")
            {
                reply(id, true, state_.prepareTask(args));
            }
            else if (action == "restore_entries")
            {
                const auto record = state_.operation(text(args, "id"));
                require(record.value("action") == "restore",
                        "Not a restore operation");
                reply(id, true,
                      backup::agent::restoreEntries(state_.directory(), args));
            }
            else if (action == "add_task" || action == "remove_task" ||
                     action == "update_task_filters" ||
                     action == "set_task_realtime" || action == "save_target" ||
                     action == "import_tasks")
            {
                require(!busy_ || action == "save_target" ||
                            action == "set_task_realtime",
                        "Agent is busy; wait before changing configuration");
                const auto result = state_.configure(action, args);
                realtime_.configure(state_.config());
                reply(id, true, result);
            }
            else
            {
                start(id, action, args);
            }
        }
        catch (const std::exception& error)
        {
            reply(id, false, {{"message", QString::fromUtf8(error.what())}});
        }
    }

    QJsonObject query(const QString& action, const QJsonObject& args,
                      const QJsonObject& target)
    {
        Channel channel(target);
        if (action == "login_remote")
        {
            auto result = remoteSessions_.login(channel, target, args);
            const auto connected = channel.call("health");
            result.insert("data_root", connected.value("data_root"));
            result.insert("repository_encryption_id",
                          connected.value("repository_encryption_id"));
            return result;
        }
        channel.authenticate();
        if (isVaultCommand(action))
        {
            return keys_.configure(action, args, channel);
        }
        if (action == "ping")
        {
            return channel.call("health");
        }
        if (action != "confirm")
        {
            return channel.call(action, args);
        }
        auto pending = state_.operation(text(args, "id"));
        require(pending.value("state") == "WAITING",
                "Operation is not waiting");
        const auto version =
            channel.call("version", {{"version_id", text(args, "id")}});
        pending.insert("state", version.value("warning_count").toInteger() == 0
                                    ? "SUCCEEDED"
                                    : "SUCCEEDED_WITH_WARNINGS");
        pending.insert("error", QJsonValue());
        pending.insert("result", QJsonObject{{"version", version}});
        QJsonArray warnings;
        qint64 offset = 0;
        do
        {
            const auto page = channel.call(
                "version_warnings",
                {{"version_id", text(args, "id")}, {"offset", offset}});
            for (const auto& item : page.value("warnings").toArray())
            {
                warnings.append(item);
            }
            offset = page.value("next_offset").isNull()
                         ? -1
                         : number(page, "next_offset");
        } while (offset >= 0);
        pending.insert("warnings", warnings);
        pending.insert("finished_at", now());
        state_.saveOperation(pending);
        return backup::agent::operationDetail(pending);
    }

    void start(const QString& requestId, QString action, QJsonObject args)
    {
        require(!busy_, "Agent is busy");
        const bool filterPreview = action == "start_filter_preview";
        if (filterPreview)
        {
            require(args.value("filters").isObject(), "Invalid custom filters");
            action = "start_scan";
        }
        const auto previewFilters = args.value("filters");
        const bool asynchronous = action.startsWith("start_");
        if (asynchronous)
        {
            action = action.mid(6);
            require(backup::agent::isDataOperation(action),
                    "Only data operations have an asynchronous start command");
        }
        require(backup::agent::isDataOperation(action) || action == "ping" ||
                    action == "versions" || action == "version_warnings" ||
                    action == "confirm" || isVaultCommand(action) ||
                    action == "login_remote",
                "Unsupported command");
        require((!isVaultCommand(action) && action != "login_remote") ||
                    args.contains("target_id"),
                "Select a configured repository for password operations");
        QJsonObject task;
        if (action == "backup" ||
            (action == "scan" && args.contains("task_id")))
        {
            task = state_.task(text(args, "task_id"));
            args.insert("path", task.value("path"));
            if (task.contains("selection"))
            {
                args.insert("selection", task.value("selection"));
            }
            else
            {
                args.remove("selection");
            }
            if (task.contains("file_types"))
            {
                args.insert("file_types", task.value("file_types"));
            }
            else
            {
                args.remove("file_types");
            }
            if (task.contains("preserve_empty_dirs"))
            {
                args.insert("preserve_empty_dirs",
                            task.value("preserve_empty_dirs"));
            }
            else
            {
                args.remove("preserve_empty_dirs");
            }
            if (task.contains("filters"))
            {
                args.insert("filters", task.value("filters"));
            }
            else
            {
                args.remove("filters");
            }
            args.insert("target_id", task.value("target_id"));
        }
        if (filterPreview)
        {
            require(!task.isEmpty(),
                    "Filter preview requires an existing task");
            args.insert("filters", previewFilters);
            const SourceScope checked(args);
        }
        auto target = args.contains("target_id")
                          ? state_.target(text(args, "target_id"))
                          : args;
        if (action == "confirm")
        {
            target =
                state_.operation(text(args, "id")).value("target").toObject();
        }
        if (action == "delete_version")
        {
            if (args.contains("retry_operation_id"))
            {
                const auto previous =
                    state_.operation(text(args, "retry_operation_id"));
                require(previous.value("action") == "delete_version" &&
                            (previous.value("state") == "WAITING" ||
                             previous.value("state") == "INTERRUPTED" ||
                             previous.value("state") == "FAILED"),
                        "Deletion operation is not retryable");
                target = previous.value("target").toObject();
                args.insert("version_id", previous.value("version_id"));
                args.insert("repository_path", target.value("repository_path"));
            }
            require(validId(text(args, "version_id")),
                    "Invalid version identifier");
            if (!args.value("repository_path").toString().isEmpty())
            {
                target.insert("repository_path", args.value("repository_path"));
            }
        }
        const auto tasks = state_.config().value("tasks").toArray();
        const auto targets = state_.config().value("targets").toArray();
        const QString stateDirectory = state_.directory();
        const bool recorded = backup::agent::isDataOperation(action);
        QJsonObject record{
            {"id", newId()},
            {"action", filterPreview ? "filter_preview" : action},
            {"state", "RUNNING"},
            {"started_at", now()},
            {"stage", "prepare"},
            {"files", 0},
            {"bytes", 0},
            {"task_id", args.value("task_id")},
            {"trigger", args.value("trigger")},
            {"target", target},
            {"source", args.value("path")},
            {"destination", args.value("destination")},
            {"version_id", args.value("version_id")},
            {"archive_path", args.value("archive_path")}};
        if (args.contains("selection"))
        {
            record.insert("selection", args.value("selection"));
        }
        if (args.contains("file_types"))
        {
            record.insert("file_types", args.value("file_types"));
        }
        if (args.contains("preserve_empty_dirs"))
        {
            record.insert("preserve_empty_dirs",
                          args.value("preserve_empty_dirs"));
        }
        if (args.contains("filters"))
        {
            record.insert("filters", args.value("filters"));
        }
        if (task.contains("name"))
        {
            record.insert("source_name", task.value("name"));
        }
        args.insert("operation_id", record.value("id"));
        if (recorded)
        {
            state_.saveOperation(record);
        }
        busy_ = true;
        worker_.reset(QThread::create(
            [this, requestId, action, args, task, target, tasks, targets,
             stateDirectory, record, recorded, asynchronous]() mutable
            {
                backup::agent::VaultKeys::Scope keys(keys_);
                backup::agent::RemoteSessions::Scope sessions(remoteSessions_);
                QJsonObject result;
                QString error;
                QElapsedTimer interval;
                interval.start();
                auto progress = [&](const QJsonObject& update)
                {
                    const bool stageChanged =
                        update.contains("stage") &&
                        update.value("stage") != record.value("stage");
                    for (auto item = update.begin(); item != update.end();
                         ++item)
                    {
                        record.insert(item.key(), item.value());
                    }
                    if (recorded &&
                        (stageChanged || update.contains("restore_journal") ||
                         interval.elapsed() >= 200))
                    {
                        state_.saveOperation(record);
                        interval.restart();
                    }
                };
                try
                {
                    if (recorded)
                    {
                        const backup::agent::OperationContext context{
                            args, task, target, tasks, targets, stateDirectory};
                        result = backup::agent::executeDataOperation(
                            action, context, progress);
                    }
                    else
                    {
                        result = query(action, args, target);
                    }
                    record.insert("state",
                                  result.value("warnings").toArray().isEmpty()
                                      ? "SUCCEEDED"
                                      : "SUCCEEDED_WITH_WARNINGS");
                    record.insert("result", result);
                    if (action == "scan" && !result.value("complete").toBool())
                    {
                        record.insert("state", "FAILED");
                        record.insert("error",
                                      "Scan incomplete; see reported paths");
                    }
                    for (const auto* key :
                         {"files", "bytes", "directories", "warnings",
                          "error_count", "complete", "symlinks", "hardlinks",
                          "fifos", "stored_bytes", "entries"})
                    {
                        if (result.contains(QLatin1String(key)))
                        {
                            record.insert(QLatin1String(key),
                                          result.value(QLatin1String(key)));
                        }
                    }
                }
                catch (const std::exception& failure)
                {
                    error = QString::fromUtf8(failure.what());
                    record.insert("state",
                                  (error.startsWith("COMMIT_UNCERTAIN") ||
                                   error.startsWith("DELETE_UNCERTAIN"))
                                      ? "WAITING"
                                      : "FAILED");
                    record.insert("error", error);
                    if (const auto* connection =
                            dynamic_cast<const ChannelError*>(&failure))
                    {
                        record.insert("failure_kind", connection->code());
                    }
                }
                record.insert("finished_at", now());
                QMetaObject::invokeMethod(
                    this,
                    [this, requestId, action, target, result, error,
                     asynchronous, recorded, record]() mutable
                    {
                        worker_->wait();
                        if ((action == "ping" || isVaultCommand(action) ||
                             action == "login_remote") &&
                            error.isEmpty() && target.contains("id") &&
                            state_.target(text(target, "id")) == target)
                        {
                            try
                            {
                                state_.rememberTargetRoot(
                                    text(target, "id"),
                                    text(result, "data_root"),
                                    result.value("repository_encryption_id")
                                        .toString());
                            }
                            catch (const std::exception& failure)
                            {
                                error = QString::fromUtf8(failure.what());
                            }
                        }
                        if (recorded)
                        {
                            try
                            {
                                state_.saveOperation(record);
                                lastRecord_ = {};
                            }
                            catch (const std::exception& failure)
                            {
                                error = "Cannot persist execution result: " +
                                        QString::fromUtf8(failure.what());
                                record.insert("state", "FAILED");
                                record.insert("error", error);
                                lastRecord_ = record;
                            }
                        }
                        busy_ = false;
                        if (isVaultCommand(action) && error.isEmpty())
                        {
                            realtime_.setRepositoryLocked(
                                state_.config(),
                                result.value("repository_encryption_id")
                                    .toString(),
                                !result.value("unlocked").toBool());
                        }
                        if (action == "login_remote" && error.isEmpty())
                        {
                            realtime_.setAuthenticated(
                                target, number(result, "expires_in"));
                        }
                        if (record.value("failure_kind") == "AUTH_REQUIRED")
                        {
                            realtime_.setAuthenticated(target, 0);
                        }
                        if (action == "backup")
                        {
                            realtime_.completed(record);
                        }
                        if (action == "confirm" && error.isEmpty())
                        {
                            realtime_.completed(result);
                        }
                        if (!asynchronous)
                        {
                            reply(requestId, error.isEmpty(),
                                  error.isEmpty()
                                      ? result
                                      : QJsonObject{{"message", error}});
                        }
                    },
                    Qt::QueuedConnection);
            }));
        worker_->start();
        if (asynchronous)
        {
            reply(requestId, true, {{"operation_id", record.value("id")}});
        }
    }

    State state_;
    backup::agent::VaultKeys keys_;
    backup::agent::RemoteSessions remoteSessions_;
    backup::agent::ControlService control_;
    backup::agent::Realtime realtime_;
    bool busy_ = false;
    QJsonObject lastRecord_;
    std::unique_ptr<QThread> worker_;
};
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("backup-agent");
    QCommandLineParser parser;
    parser.setApplicationDescription("Backup Agent command service");
    parser.addHelpOption();
    parser.addOption(
        {"state-dir", "Agent configuration and execution directory", "path",
         QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
             "/backup-system"});
    parser.addOption({"daemon", "Run background local control service"});
    parser.addOption({"connect", "Connect to or start background Agent"});
    parser.process(app);
    try
    {
        if (parser.isSet("connect"))
        {
            return backup::agent::runControlProxy(parser.value("state-dir"));
        }
        CommandReader reader(&app, parser.value("state-dir"),
                             parser.isSet("daemon"));
        return app.exec();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "AGENT_STATE_ERROR: %s\n", error.what());
        return 2;
    }
}
