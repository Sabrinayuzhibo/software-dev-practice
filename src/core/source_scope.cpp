#include "backup/core/source_scope.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>

namespace backup::core
{
SourceScope::SourceScope(const QJsonObject& value, const char* field)
    : root_(text(value, field))
{
    require(QDir::isAbsolutePath(root_) && QDir::cleanPath(root_) == root_ &&
                (root_ == "/" || validPath(root_.mid(1))),
            "Invalid source root");
    hasEmptyDirectoryRule_ = value.contains("preserve_empty_dirs");
    if (hasEmptyDirectoryRule_)
    {
        require(value.value("preserve_empty_dirs").isBool(),
                "Invalid empty directory rule");
        preserveEmptyDirectories_ = value.value("preserve_empty_dirs").toBool();
        require(value.contains("file_types"),
                "File types required with empty directory rule");
    }
    if (value.contains("file_types"))
    {
        require(value.value("file_types").isArray(), "Invalid file type filter");
        const auto requested = value.value("file_types").toArray();
        require(!requested.isEmpty() &&
                    requested.size() <= (hasEmptyDirectoryRule_ ? 6 : 7),
                "Select at least one supported file type");
        QSet<QString> unique;
        for (const auto& item : requested)
        {
            require(item.isString() &&
                        (item == "file" || item == "symlink" ||
                         item == "fifo" ||
                         item == "character_device" ||
                         item == "block_device" || item == "socket" ||
                         (!hasEmptyDirectoryRule_ && item == "directory")) &&
                        !unique.contains(item.toString()),
                    "Invalid file type filter");
            unique.insert(item.toString());
        }
        const bool legacyAll =
            !hasEmptyDirectoryRule_ && unique.size() == 4 &&
            unique.contains("file") && unique.contains("directory") &&
            unique.contains("symlink") && unique.contains("fifo");
        if (!legacyAll)
        {
            for (const auto* type : {"file", "directory", "symlink", "fifo",
                                     "character_device", "block_device",
                                     "socket"})
            {
                if (unique.contains(QLatin1String(type)))
                {
                    fileTypes_.append(QLatin1String(type));
                }
            }
        }
    }
    if (value.contains("rules"))
    {
        require(value.value("rules").isObject(), "Invalid saved rules");
        const auto rules = value.value("rules").toObject();
        if (hasEmptyDirectoryRule_ || rules.contains("preserve_empty_dirs"))
        {
            require(hasEmptyDirectoryRule_ &&
                        rules.value("file_types") == fileTypes_ &&
                        rules.value("preserve_empty_dirs") ==
                            preserveEmptyDirectories_,
                    "Saved selection rules do not match version");
        }
    }
    if (!value.contains("selection"))
    {
        return;
    }
    require(value.value("selection").isArray(), "Invalid source selection");
    selection_ = value.value("selection").toArray();
    require(
        !selection_.isEmpty() && selection_.size() <= kMaxSelectedSources &&
            QJsonDocument(selection_).toJson(QJsonDocument::Compact).size() <=
                kSourceScopeBytes,
        "Source selection exceeds supported size");
    QStringList previous;
    for (const auto& item : selection_)
    {
        const auto entry = item.toObject();
        const auto path = text(entry, "path");
        const auto type = text(entry, "type");
        require(validPath(path) && supportedEntryType(type) &&
                    type != "hardlink",
                "Invalid selected source: " + path);
        require(type == "directory" || includes(type),
                "Selected source does not match file type filter: " + path);
        for (const auto& earlier : previous)
        {
            require(!core::overlaps(earlier, path),
                    "Overlapping selected sources: " + path);
        }
        previous.append(path);
    }
}

const QString& SourceScope::root() const
{
    return root_;
}

const QJsonArray& SourceScope::selection() const
{
    return selection_;
}

const QJsonArray& SourceScope::fileTypes() const
{
    return fileTypes_;
}

bool SourceScope::hasEmptyDirectoryRule() const
{
    return hasEmptyDirectoryRule_;
}

bool SourceScope::preservesEmptyDirectories() const
{
    return hasEmptyDirectoryRule_
               ? preserveEmptyDirectories_
               : fileTypes_.isEmpty() || fileTypes_.contains("directory");
}

bool SourceScope::includes(const QString& type) const
{
    if (type == "directory")
    {
        return preservesEmptyDirectories();
    }
    const auto normalized = type == "hardlink" ? "file" : type;
    if (type == "character_device" || type == "block_device" ||
        type == "socket")
    {
        return fileTypes_.contains(normalized);
    }
    return fileTypes_.isEmpty() || fileTypes_.contains(normalized);
}

QStringList SourceScope::paths() const
{
    if (selection_.isEmpty())
    {
        return {root_};
    }
    QStringList result;
    for (const auto& value : selection_)
    {
        result.append(QDir(root_).filePath(text(value.toObject(), "path")));
    }
    return result;
}

QJsonObject SourceScope::json(const char* field) const
{
    QJsonObject result{{QLatin1String(field), root_}};
    if (!selection_.isEmpty())
    {
        result.insert("selection", selection_);
    }
    if (!fileTypes_.isEmpty())
    {
        result.insert("file_types", fileTypes_);
    }
    if (hasEmptyDirectoryRule_)
    {
        result.insert("preserve_empty_dirs", preserveEmptyDirectories_);
    }
    return result;
}

bool SourceScope::allows(const QString& path, const QString& type) const
{
    if (type != "directory" && !includes(type))
    {
        return false;
    }
    if (selection_.isEmpty())
    {
        return true;
    }
    for (const auto& value : selection_)
    {
        const auto item = value.toObject();
        const auto selected = text(item, "path");
        if (selected == path)
        {
            return item.value("type") == type ||
                   (item.value("type") == "file" && type == "hardlink");
        }
        if ((type == "directory" && selected.startsWith(path + '/')) ||
            (item.value("type") == "directory" &&
             path.startsWith(selected + '/')))
        {
            return true;
        }
    }
    return false;
}

bool SourceScope::overlaps(const QString& directory) const
{
    for (const auto& path : paths())
    {
        const auto resolved =
            selection_.isEmpty() || path == "/"
                ? canonicalPath(path)
                : QDir(canonicalPath(QFileInfo(path).absolutePath()))
                      .filePath(QFileInfo(path).fileName());
        if (core::overlaps(resolved, directory))
        {
            return true;
        }
    }
    return false;
}
} // namespace backup::core
