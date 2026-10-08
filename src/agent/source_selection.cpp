#include "backup/agent/source_selection.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>

namespace backup::agent
{
using namespace core;
namespace
{
QString selectedType(const QString& path)
{
    struct stat info
    {
    };
    const auto result = ::lstat(QFile::encodeName(path).constData(), &info);
    const auto error = errno;
    require(result == 0, "Cannot inspect source: " + path + ": " +
                             QString::fromLocal8Bit(strerror(error)));
    if (S_ISDIR(info.st_mode))
    {
        return "directory";
    }
    if (S_ISREG(info.st_mode))
    {
        return "file";
    }
    if (S_ISLNK(info.st_mode))
    {
        return "symlink";
    }
    if (S_ISFIFO(info.st_mode))
    {
        return "fifo";
    }
    if (S_ISCHR(info.st_mode))
    {
        return "character_device";
    }
    if (S_ISBLK(info.st_mode))
    {
        return "block_device";
    }
    if (S_ISSOCK(info.st_mode))
    {
        return "socket";
    }
    throw Error("Unsupported source type: " + path);
}
} // namespace

QJsonObject prepareSources(const QJsonArray& paths,
                           const QJsonValue& fileTypes,
                           const QJsonValue& preserveEmptyDirs)
{
    require(!paths.isEmpty() && paths.size() <= kMaxSelectedSources,
            "Select between 1 and 100 sources");
    QJsonObject filterSpecification{{"path", "/"}};
    if (!fileTypes.isUndefined())
    {
        filterSpecification.insert("file_types", fileTypes);
    }
    if (!preserveEmptyDirs.isUndefined())
    {
        filterSpecification.insert("preserve_empty_dirs", preserveEmptyDirs);
    }
    const SourceScope filter(filterSpecification);
    QMap<QString, QString> selected;
    for (const auto& value : paths)
    {
        require(value.isString() && !value.toString().isEmpty() &&
                    !value.toString().contains(QChar(0)),
                "Invalid source path");
        const auto input =
            QDir::cleanPath(QFileInfo(value.toString()).absoluteFilePath());
        const auto path =
            input == "/" ? input
                         : QDir(canonicalPath(QFileInfo(input).absolutePath()))
                               .filePath(QFileInfo(input).fileName());
        require(path == "/" || validPath(path.mid(1)),
                "Invalid source path: " + path);
        const auto type = selectedType(path);
        require(type == "directory" || filter.includes(type),
                "Selected source does not match file type filter: " + path);
        selected.insert(path, type);
    }
    QMap<QString, QString> roots;
    for (auto item = selected.begin(); item != selected.end(); ++item)
    {
        bool covered = false;
        for (auto existing = roots.begin(); existing != roots.end(); ++existing)
        {
            covered |= existing.value() == "directory" &&
                       overlaps(existing.key(), item.key());
        }
        if (!covered)
        {
            roots.insert(item.key(), item.value());
        }
    }
    const bool directory = roots.size() == 1 && roots.first() == "directory";
    QString root = directory ? roots.firstKey()
                             : QFileInfo(roots.firstKey()).absolutePath();
    if (!directory)
    {
        for (auto item = roots.begin(); item != roots.end(); ++item)
        {
            while (root != "/" && !item.key().startsWith(root + '/'))
            {
                root = QFileInfo(root).absolutePath();
            }
        }
    }
    QJsonArray selection;
    QJsonArray items;
    for (auto item = roots.begin(); item != roots.end(); ++item)
    {
        const auto relative =
            directory ? "." : QDir(root).relativeFilePath(item.key());
        items.append(QJsonObject{{"path", item.key()},
                                 {"type", item.value()},
                                 {"restore_path", relative}});
        if (!directory)
        {
            selection.append(
                QJsonObject{{"path", relative}, {"type", item.value()}});
        }
    }
    QJsonObject scope{{"path", root}};
    if (!selection.isEmpty())
    {
        scope.insert("selection", selection);
    }
    if (!filter.fileTypes().isEmpty())
    {
        scope.insert("file_types", filter.fileTypes());
    }
    if (filter.hasEmptyDirectoryRule())
    {
        scope.insert("preserve_empty_dirs",
                     filter.preservesEmptyDirectories());
    }
    const SourceScope checked(scope);
    return {{"scope", checked.json()},
            {"items", items},
            {"merged_count", paths.size() - roots.size()}};
}
} // namespace backup::agent
