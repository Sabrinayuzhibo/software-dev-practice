#include "backup/agent/state.h"
#include "backup/core/io.h"

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
        require(config_.value("format").toInt() == 1 &&
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

QJsonObject State::addTask(const QJsonObject& args)
{
    const QString source = canonicalPath(text(args, "path"));
    require(QFileInfo(source).isDir(), "Source directory does not exist");
    require(!overlaps(source, directory_), "Source overlaps Agent state");
    const QString targetId = text(args, "target_id");
    const auto destination = target(targetId);
    if (destination.contains("data_root"))
    {
        require(
            !overlaps(source, canonicalPath(text(destination, "data_root"))),
            "Source overlaps repository");
    }
    auto tasks = config_.value("tasks").toArray();
    for (const auto& item : tasks)
    {
        const auto existing = item.toObject();
        if (existing.value("path") == source &&
            existing.value("target_id") == targetId)
        {
            return existing;
        }
    }
    const QJsonObject created{{"id", newId()},
                              {"path", source},
                              {"target_id", targetId},
                              {"createdAt", now()}};
    tasks.append(created);
    auto next = config_;
    next.insert("tasks", tasks);
    save(next);
    return created;
}

QJsonObject State::importTasks(const QJsonArray& imported)
{
    auto tasks = config_.value("tasks").toArray();
    for (const auto& value : imported)
    {
        auto item = value.toObject();
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
                require(saved.value("path") == source,
                        "Conflicting legacy task; original preserved");
                present = true;
            }
            else
            {
                require(saved.value("path") != source ||
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
        const QString id = args.value("id").toString(newId());
        require(validId(id), "Invalid target identifier");
        require(text(args, "host") == "127.0.0.1" && number(args, "port") > 0 &&
                    number(args, "port") <= 65535,
                "Only local targets are supported");
        const auto name = text(args, "name");
        auto targets = config_.value("targets").toArray();
        QJsonObject value{{"id", id},
                          {"name", name},
                          {"host", "127.0.0.1"},
                          {"port", args.value("port")}};
        bool replaced = false;
        for (qsizetype index = 0; index < targets.size(); ++index)
        {
            if (targets[index].toObject().value("id") == id)
            {
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
