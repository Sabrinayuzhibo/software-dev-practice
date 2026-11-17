#include "backup/agent/operation_dispatch.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"

namespace backup::agent
{
using namespace core;

bool isDataOperation(const QString& action)
{
    return action == "scan" || action == "backup" || action == "restore" ||
           action == "delete_version" || action == "export_archive" ||
           action == "import_archive";
}

QJsonObject executeDataOperation(const QString& action,
                                 const OperationContext& context,
                                 const Progress& progress)
{
    if (action == "scan")
    {
        auto source = context.args;
        if (context.task.isEmpty())
        {
            source.insert("path", canonicalPath(text(context.args, "path")));
        }
        require(!SourceScope(source).overlaps(context.stateDirectory),
                "Source overlaps Agent state");
        return scan(source, progress);
    }
    if (action == "backup")
    {
        require(!SourceScope(context.task).overlaps(context.stateDirectory),
                "Source overlaps Agent state");
        return backup(context.task, context.target,
                      text(context.args, "operation_id"), progress);
    }
    if (action == "restore")
    {
        return restore(context.args, context.target, context.tasks,
                       context.targets, context.stateDirectory, progress);
    }
    if (action == "delete_version")
    {
        return deleteVersion(context.args, context.target, progress);
    }
    if (action == "export_archive")
    {
        return exportArchive(context.args, context.target, context.tasks,
                             context.targets, context.stateDirectory, progress);
    }
    if (action == "import_archive")
    {
        return importArchive(context.args, context.target, progress);
    }
    throw Error("Unsupported data operation: " + action);
}
} // 命名空间 backup::agent
