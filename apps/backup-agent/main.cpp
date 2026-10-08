// JSON-line control bridge. The event thread owns configuration and stdout;
// one worker performs data operations and persists their progress/results.
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
using backup::agent::State;

void reply(const QString& id, bool ok, const QJsonObject& value)
{
    const QJsonObject object{
        {"id", id}, {"ok", ok}, {ok ? "result" : "error", value}};
    auto line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (line.size() > backup::agent::kAgentReplyBytes)
    {
        const QJsonObject failure{
            {"id", id},
            {"ok", false},
            {"error", QJsonObject{{"message",
                                   "Response too large; use a paged query"}}}};
        line = QJsonDocument(failure).toJson(QJsonDocument::Compact) + '\n';
    }
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

class CommandReader final : public QObject
{
  public:
    CommandReader(QCoreApplication* app, const QString& directory)
        : QObject(app), state_(directory),
          notifier_(STDIN_FILENO, QSocketNotifier::Read, this)
    {
        connect(&notifier_, &QSocketNotifier::activated, this,
                [this] { readAvailable(); });
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
    void readAvailable()
    {
        char chunk[8192];
        const auto count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count <= 0)
        {
            if (count < 0 && errno == EINTR)
            {
                return;
            }
            notifier_.setEnabled(false);
            QCoreApplication::quit();
            return;
        }
        buffer_.append(chunk, static_cast<qsizetype>(count));
        qsizetype end = 0;
        while ((end = buffer_.indexOf('\n')) >= 0)
        {
            const auto line = buffer_.left(end);
            buffer_.remove(0, end + 1);
            handle(line);
        }
        if (buffer_.size() > kMaxJsonBytes)
        {
            reply({}, false, {{"message", "Command too long"}});
            notifier_.setEnabled(false);
            QCoreApplication::quit();
        }
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
                reply(id, true, state_.config());
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
                     action == "save_target" || action == "import_tasks")
            {
                require(!busy_ || action == "save_target",
                        "Agent is busy; wait before changing configuration");
                reply(id, true, state_.configure(action, args));
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
        channel.authenticate();
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
        const bool asynchronous = action.startsWith("start_");
        if (asynchronous)
        {
            action = action.mid(6);
            require(action == "scan" || action == "backup" ||
                        action == "restore",
                    "Only data operations have an asynchronous start command");
        }
        require(action == "ping" || action == "versions" ||
                    action == "version_warnings" || action == "scan" ||
                    action == "backup" || action == "restore" ||
                    action == "confirm",
                "Unsupported command");
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
            args.insert("target_id", task.value("target_id"));
        }
        auto target = args.contains("target_id")
                          ? state_.target(text(args, "target_id"))
                          : args;
        if (action == "confirm")
        {
            target =
                state_.operation(text(args, "id")).value("target").toObject();
        }
        const auto tasks = state_.config().value("tasks").toArray();
        const auto targets = state_.config().value("targets").toArray();
        const QString stateDirectory = state_.directory();
        const bool recorded =
            action == "scan" || action == "backup" || action == "restore";
        QJsonObject record{{"id", newId()},
                           {"action", action},
                           {"state", "RUNNING"},
                           {"started_at", now()},
                           {"stage", "prepare"},
                           {"files", 0},
                           {"bytes", 0},
                           {"task_id", args.value("task_id")},
                           {"target", target},
                           {"source", args.value("path")},
                           {"destination", args.value("destination")},
                           {"version_id", args.value("version_id")}};
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
                    if (action == "scan")
                    {
                        auto source = args;
                        if (task.isEmpty())
                        {
                            source.insert("path",
                                          canonicalPath(text(args, "path")));
                        }
                        require(!SourceScope(source).overlaps(stateDirectory),
                                "Source overlaps Agent state");
                        result = backup::agent::scan(source, progress);
                    }
                    else if (action == "backup")
                    {
                        require(!SourceScope(task).overlaps(stateDirectory),
                                "Source overlaps Agent state");
                        result = backup::agent::backup(
                            task, target, text(record, "id"), progress);
                    }
                    else if (action == "restore")
                    {
                        result =
                            backup::agent::restore(args, target, tasks, targets,
                                                   stateDirectory, progress);
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
                    record.insert("state", error.startsWith("COMMIT_UNCERTAIN")
                                               ? "WAITING"
                                               : "FAILED");
                    record.insert("error", error);
                }
                record.insert("finished_at", now());
                QMetaObject::invokeMethod(
                    this,
                    [this, requestId, action, target, result, error,
                     asynchronous, recorded, record]() mutable
                    {
                        worker_->wait();
                        if (action == "ping" && error.isEmpty() &&
                            target.contains("id") &&
                            state_.target(text(target, "id")) == target)
                        {
                            try
                            {
                                state_.rememberTargetRoot(
                                    text(target, "id"),
                                    text(result, "data_root"));
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
    QSocketNotifier notifier_;
    QByteArray buffer_;
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
    parser.process(app);
    try
    {
        CommandReader reader(&app, parser.value("state-dir"));
        return app.exec();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "AGENT_STATE_ERROR: %s\n", error.what());
        return 2;
    }
}
