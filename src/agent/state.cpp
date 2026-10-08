#include "backup/agent/state.h"
#include "backup/agent/source_selection.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace backup::agent
{
using namespace core;
namespace
{
QJsonObject find(const QJsonArray& rows, const QString& id)
{
    for (const auto& row : rows)
    {
        if (row.toObject().value("id").toString() == id)
        {
            return row.toObject();
        }
    }
    throw Error("Configuration item not found: " + id);
}

bool sameRepository(const QJsonObject& first, const QJsonObject& second)
{
    if (first.value("host") == second.value("host") &&
        number(first, "port") == number(second, "port"))
    {
        return true;
    }
    const auto root = [](const QJsonObject& target)
    {
        return target.value("repository_path")
            .toString(target.value("data_root").toString());
    };
    const auto left = root(first);
    const auto right = root(second);
    return !left.isEmpty() && !right.isEmpty() &&
           canonicalPath(left) == canonicalPath(right);
}
} // namespace

State::State(const QString& directory) : directory_(canonicalPath(directory))
{
    makeDirectory(directory_ + "/operations");
    lock_ = std::make_unique<QLockFile>(directory_ + "/agent.lock");
    require(lock_->tryLock(), "Agent state is already in use");
    const QString path = directory_ + "/config.json";
    if (QFileInfo::exists(path))
    {
        config_ = readObject(path);
        require((config_.value("format").toInt() >= 1 &&
                 config_.value("format").toInt() <= 5) &&
                    config_.value("tasks").isArray() &&
                    config_.value("targets").isArray(),
                "Invalid Agent configuration; original file preserved");
        QSet<QString> targetIds;
        for (const auto& value : config_.value("targets").toArray())
        {
            const auto item = value.toObject();
            const QString id = text(item, "id");
            require(validId(id) && !targetIds.contains(id) &&
                        text(item, "host") == "127.0.0.1" &&
                        number(item, "port") > 0 &&
                        number(item, "port") <= 65535,
                    "Invalid saved target");
            targetIds.insert(id);
        }
        QSet<QString> taskIds;
        for (const auto& value : config_.value("tasks").toArray())
        {
            const auto item = value.toObject();
            const QString id = text(item, "id");
            require(validId(id) && !taskIds.contains(id) &&
                        QDir(text(item, "path")).isAbsolute() &&
                        targetIds.contains(text(item, "target_id")),
                    "Invalid saved task");
            taskIds.insert(id);
            const SourceScope scope(item);
            require(scope.selection().isEmpty() ||
                        config_.value("format").toInt() >= 2,
                    "Selected sources require configuration format 2");
            require(scope.fileTypes().isEmpty() ||
                        config_.value("format").toInt() >= 3,
                    "File type filters require configuration format 3");
            require(!scope.hasEmptyDirectoryRule() ||
                        config_.value("format").toInt() >= 4,
                    "Empty directory rules require configuration format 4");
            require((!scope.fileTypes().contains("character_device") &&
                     !scope.fileTypes().contains("block_device") &&
                     !scope.fileTypes().contains("socket")) ||
                        config_.value("format").toInt() >= 5,
                    "Device and socket filters require configuration format 5");
            require(!item.contains("name") ||
                        (item.value("name").isString() &&
                         item.value("name").toString().size() <= 120),
                    "Invalid saved task name");
        }
    }
    else
    {
        save({{"format", 1},
              {"tasks", QJsonArray{}},
              {"targets", QJsonArray{QJsonObject{{"id", "local"},
                                                 {"name", "Local repository"},
                                                 {"host", "127.0.0.1"},
                                                 {"port", 9000}}}}});
    }
    for (const auto& name :
         QDir(directory_ + "/operations").entryList({"*.json"}, QDir::Files))
    {
        auto record = readObject(directory_ + "/operations/" + name);
        if (record.value("state") == "RUNNING")
        {
            const bool pendingCommit = record.value("action") == "backup" &&
                                       (record.value("stage") == "commit" ||
                                        record.value("stage") == "confirm");
            record.insert("state", pendingCommit ? "WAITING" : "INTERRUPTED");
            record.insert(
                "error",
                pendingCommit
                    ? "Backup commit may have completed; confirm this operation"
                    : "Agent exited before completion; restore may contain "
                      "partial files");
            record.insert("finished_at", now());
            if (record.value("action") == "restore" &&
                QFileInfo::exists(directory_ + "/restores/" +
                                  text(record, "id") + ".jsonl"))
            {
                record.insert("restore_journal", true);
            }
            saveOperation(record);
        }
    }
    compactOperationWarnings();
}

QJsonObject State::config() const
{
    return config_;
}
QString State::directory() const
{
    return directory_;
}
QJsonObject State::task(const QString& id) const
{
    return find(config_.value("tasks").toArray(), id);
}
QJsonObject State::target(const QString& id) const
{
    return find(config_.value("targets").toArray(), id);
}

void State::rememberTargetRoot(const QString& id, const QString& root)
{
    auto targets = config_.value("targets").toArray();
    for (qsizetype index = 0; index < targets.size(); ++index)
    {
        auto item = targets[index].toObject();
        if (item.value("id") == id)
        {
            item.insert("data_root", canonicalPath(root));
            targets[index] = item;
            auto next = config_;
            next.insert("targets", targets);
            save(next);
            return;
        }
    }
}

void State::save(const QJsonObject& next)
{
    writeObject(directory_ + "/config.json", next);
    config_ = next;
}

QJsonObject State::prepareTask(const QJsonObject& args) const
{
    require(args.value("sources").isArray(), "Invalid source list");
    const auto plan = prepareSources(args.value("sources").toArray(),
                                     args.value("file_types"),
                                     args.value("preserve_empty_dirs"));
    const SourceScope scope(plan.value("scope").toObject());
    target(text(args, "target_id"));
    require(!scope.overlaps(directory_), "Source overlaps Agent state");
    for (const auto& value : config_.value("targets").toArray())
    {
        const auto repository = value.toObject();
        for (const auto* field : {"data_root", "repository_path"})
        {
            if (repository.contains(QLatin1String(field)))
            {
                require(!scope.overlaps(canonicalPath(text(repository, field))),
                        "Source overlaps repository");
            }
        }
    }
    return plan;
}

QJsonObject State::addTask(const QJsonObject& args)
{
    auto input = args;
    if (!args.contains("sources"))
    {
        const auto path = canonicalPath(text(args, "path"));
        require(QFileInfo(path).isDir(), "Source directory does not exist");
        input.insert("sources", QJsonArray{path});
    }
    const auto planned = prepareTask(input).value("scope").toObject();
    const SourceScope scope(planned);
    const QString targetId = text(args, "target_id");
    const auto destination = target(targetId);
    require(!args.contains("name") ||
                (args.value("name").isString() &&
                 args.value("name").toString().size() <= 120),
            "Task name must be at most 120 characters");
    auto tasks = config_.value("tasks").toArray();
    for (const auto& item : tasks)
    {
        const auto existing = item.toObject();
        if (SourceScope(existing).json() == scope.json() &&
            sameRepository(target(text(existing, "target_id")), destination))
        {
            require(existing.value("target_id") == targetId,
                    "Source already configured for this repository under "
                    "another target");
            return existing;
        }
    }
    auto created = scope.json();
    created.insert("id", newId());
    created.insert("target_id", targetId);
    created.insert("createdAt", now());
    const auto name = args.value("name").toString().trimmed();
    if (!name.isEmpty())
    {
        created.insert("name", name);
    }
    tasks.append(created);
    auto next = config_;
    next.insert("tasks", tasks);
    if (scope.fileTypes().contains("character_device") ||
        scope.fileTypes().contains("block_device") ||
        scope.fileTypes().contains("socket"))
    {
        next.insert("format", 5);
    }
    else if (scope.hasEmptyDirectoryRule() &&
             next.value("format").toInt() < 4)
    {
        next.insert("format", 4);
    }
    else if (!scope.fileTypes().isEmpty() &&
             next.value("format").toInt() < 3)
    {
        next.insert("format", 3);
    }
    else if (!scope.selection().isEmpty() &&
             next.value("format").toInt() < 2)
    {
        next.insert("format", 2);
    }
    save(next);
    return created;
}

QJsonObject State::importTasks(const QJsonArray& imported)
{
    auto tasks = config_.value("tasks").toArray();
    for (const auto& value : imported)
    {
        auto item = value.toObject();
        require(!item.contains("selection") && !item.contains("file_types") &&
                    !item.contains("preserve_empty_dirs"),
                "Legacy tasks must use whole directories; original preserved");
        const QString id = text(item, "id");
        require(validId(id), "Invalid legacy task identifier");
        const QString source = canonicalPath(text(item, "path"));
        text(item, "createdAt");
        bool present = false;
        for (const auto& savedValue : tasks)
        {
            const auto saved = savedValue.toObject();
            if (saved.value("id") == id)
            {
                require(saved.value("path") == source &&
                            !saved.contains("selection"),
                        "Conflicting legacy task; original preserved");
                present = true;
            }
            else
            {
                require(saved.contains("selection") ||
                            saved.value("path") != source ||
                            saved.value("target_id") != "local",
                        "Duplicate legacy source; original preserved");
            }
        }
        if (!present)
        {
            item.insert("path", source);
            item.insert("target_id", "local");
            tasks.append(item);
        }
    }
    auto next = config_;
    next.insert("tasks", tasks);
    next.insert("legacy_imported", true);
    save(next);
    return {{"tasks", tasks}};
}

QJsonObject State::configure(const QString& action, const QJsonObject& args)
{
    if (action == "add_task")
    {
        return addTask(args);
    }
    if (action == "import_tasks")
    {
        if (config_.value("legacy_imported").toBool())
        {
            return {};
        }
        require(args.value("tasks").isArray(), "Invalid legacy task list");
        return importTasks(args.value("tasks").toArray());
    }
    auto next = config_;
    if (action == "save_target")
    {
        require(!args.contains("mode") || args.value("mode") == "local",
                "Only local targets are supported");
        require(!args.contains("repository_path") ||
                    args.value("repository_path").isString(),
                "Invalid repository path");
        const QString id = args.value("id").toString(newId());
        require(validId(id), "Invalid target identifier");
        require(text(args, "host") == "127.0.0.1" && number(args, "port") > 0 &&
                    number(args, "port") <= 65535,
                "Only local targets are supported");
        const auto name = text(args, "name");
        auto targets = config_.value("targets").toArray();
        QJsonObject value{{"id", id},
                          {"name", name},
                          {"mode", "local"},
                          {"host", "127.0.0.1"},
                          {"port", args.value("port")}};
        const auto repository = args.value("repository_path").toString();
        if (!repository.isEmpty())
        {
            require(QDir(repository).isAbsolute(),
                    "Repository path must be absolute");
            const auto root = canonicalPath(repository);
            for (const auto& task : config_.value("tasks").toArray())
            {
                require(!SourceScope(task.toObject()).overlaps(root),
                        "Repository overlaps a configured source");
            }
            value.insert("repository_path", root);
        }
        const auto tasks = config_.value("tasks").toArray();
        for (const auto& first : tasks)
        {
            const auto task = first.toObject();
            if (task.value("target_id") != id)
            {
                continue;
            }
            for (const auto& second : tasks)
            {
                const auto other = second.toObject();
                if (other.value("target_id") != id &&
                    SourceScope(other).json() == SourceScope(task).json())
                {
                    require(!sameRepository(value,
                                            target(text(other, "target_id"))),
                            "Target change would duplicate an existing source "
                            "and repository");
                }
            }
        }
        bool replaced = false;
        for (qsizetype index = 0; index < targets.size(); ++index)
        {
            if (targets[index].toObject().value("id") == id)
            {
                const auto previous = targets[index].toObject();
                if (previous.value("host") == value.value("host") &&
                    number(previous, "port") == number(value, "port") &&
                    previous.contains("data_root") &&
                    (repository.isEmpty() || value.value("repository_path") ==
                                                 previous.value("data_root")))
                {
                    value.insert("data_root", previous.value("data_root"));
                }
                targets[index] = value;
                replaced = true;
            }
        }
        if (!replaced)
        {
            targets.append(value);
        }
        next.insert("targets", targets);
    }
    else if (action == "remove_task")
    {
        const QString id = text(args, "id");
        task(id);
        QJsonArray tasks;
        for (const auto& item : config_.value("tasks").toArray())
        {
            if (item.toObject().value("id") != id)
            {
                tasks.append(item);
            }
        }
        next.insert("tasks", tasks);
    }
    else
    {
        throw Error("Unknown configuration command");
    }
    save(next);
    return config_;
}

} // namespace backup::agent
