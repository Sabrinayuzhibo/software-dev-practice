#include "backup/core/source_scope.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace backup::core
{
namespace
{
QString wildcardExpression(const QString& pattern, bool path)
{
    QString result = "^";
    for (const auto character : pattern)
    {
        if (character == '*')
        {
            result += path ? "[^/]*" : ".*";
        }
        else if (character == '?')
        {
            result += path ? "[^/]" : ".";
        }
        else
        {
            result += QRegularExpression::escape(QString(character));
        }
    }
    result += '$';
    return result;
}

bool ruleMatches(const QJsonObject& rule, const QString& path,
                 const QString& type, qint64 size, qint64 uid,
                 qint64 mtimeSeconds)
{
    const auto category = text(rule, "category");
    if (category == "path")
    {
        return QRegularExpression(wildcardExpression(text(rule, "pattern"),
                                                     true))
            .match(path)
            .hasMatch();
    }
    if (category == "name")
    {
        const auto name = path.section('/', -1);
        return QRegularExpression(wildcardExpression(text(rule, "pattern"),
                                                     false))
            .match(name)
            .hasMatch();
    }
    if (category == "type")
    {
        return text(rule, "value") == type;
    }
    if (category == "user")
    {
        return number(rule, "value") == uid;
    }
    if (category == "mtime" || category == "size")
    {
        if (category == "size" && type != "file" && type != "hardlink")
        {
            return false;
        }
        const auto value = category == "size" ? size : mtimeSeconds;
        if (rule.contains("min"))
        {
            const auto minimum = category == "size"
                                     ? number(rule, "min")
                                     : signedNumber(rule, "min");
            if (value < minimum)
            {
                return false;
            }
        }
        if (rule.contains("max"))
        {
            const auto maximum = category == "size"
                                     ? number(rule, "max")
                                     : signedNumber(rule, "max");
            if (value > maximum)
            {
                return false;
            }
        }
        return true;
    }
    return false;
}
} // namespace

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
    if (value.contains("filters"))
    {
        require(value.value("filters").isObject(), "Invalid custom filters");
        filters_ = value.value("filters").toObject();
        const QSet<QString> categories{"path", "type", "name", "mtime",
                                       "size", "user"};
        for (const auto* polarity : {"include", "exclude"})
        {
            const auto rules = filters_.value(QLatin1String(polarity));
            require(rules.isArray() && rules.toArray().size() <= 100,
                    "Invalid custom filter list");
            for (const auto& value : rules.toArray())
            {
                require(value.isObject(), "Invalid custom filter rule");
                const auto rule = value.toObject();
                const auto category = text(rule, "category");
                require(categories.contains(category),
                        "Unsupported custom filter category");
                if (category == "path" || category == "name")
                {
                    const auto pattern = text(rule, "pattern");
                    require(pattern.size() <= 512 &&
                                (!pattern.startsWith('/') &&
                                 !pattern.contains(QChar(0))),
                            "Invalid custom filter pattern");
                    if (category == "path")
                    {
                        for (const auto& component : pattern.split('/'))
                        {
                            require(component != ".." && component != ".",
                                    "Invalid custom path filter");
                        }
                    }
                }
                else if (category == "type")
                {
                    const auto type = text(rule, "value");
                    require(supportedEntryType(type) && type != "unknown",
                            "Invalid custom type filter");
                }
                else if (category == "user")
                {
                    number(rule, "value");
                    require(number(rule, "value") <=
                                std::numeric_limits<uid_t>::max(),
                            "Invalid custom UID filter");
                }
                else
                {
                    require(rule.contains("min") || rule.contains("max"),
                            "Custom range needs a boundary");
                    if (category == "size")
                    {
                        if (rule.contains("min"))
                        {
                            number(rule, "min");
                        }
                        if (rule.contains("max"))
                        {
                            number(rule, "max");
                        }
                    }
                    else
                    {
                        if (rule.contains("min"))
                        {
                            signedNumber(rule, "min");
                        }
                        if (rule.contains("max"))
                        {
                            signedNumber(rule, "max");
                        }
                    }
                    require(!rule.contains("min") || !rule.contains("max") ||
                                (category == "size"
                                     ? number(rule, "min") <=
                                           number(rule, "max")
                                     : signedNumber(rule, "min") <=
                                           signedNumber(rule, "max")),
                            "Invalid custom filter range");
                }
            }
        }
        require(filters_.size() == 2 && filters_.contains("include") &&
                    filters_.contains("exclude"),
                "Invalid custom filter fields");
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

bool SourceScope::hasFilters() const
{
    return !filters_.isEmpty() &&
           (!filters_.value("include").toArray().isEmpty() ||
            !filters_.value("exclude").toArray().isEmpty());
}

bool SourceScope::matches(const QString& path, const QString& type, qint64 size,
                          qint64 uid, qint64 mtimeSeconds) const
{
    const auto includes = filters_.value("include").toArray();
    QHash<QString, bool> matchedCategories;
    QHash<QString, bool> requiredCategories;
    for (const auto& value : includes)
    {
        const auto rule = value.toObject();
        const auto category = text(rule, "category");
        requiredCategories.insert(category, true);
        if (ruleMatches(rule, path, type, size, uid, mtimeSeconds))
        {
            matchedCategories.insert(category, true);
        }
    }
    for (auto item = requiredCategories.begin();
         item != requiredCategories.end(); ++item)
    {
        if (!matchedCategories.value(item.key()))
        {
            return false;
        }
    }
    for (const auto& value : filters_.value("exclude").toArray())
    {
        if (ruleMatches(value.toObject(), path, type, size, uid,
                        mtimeSeconds))
        {
            return false;
        }
    }
    return true;
}

bool SourceScope::excludesDirectorySubtree(const QString& path, qint64 uid,
                                          qint64 mtimeSeconds) const
{
    for (const auto& value : filters_.value("exclude").toArray())
    {
        const auto rule = value.toObject();
        if (text(rule, "category") != "size" &&
            ruleMatches(rule, path, "directory", 0, uid, mtimeSeconds))
        {
            return true;
        }
    }
    return false;
}

bool SourceScope::allows(const QJsonObject& entry) const
{
    const auto path = text(entry, "path");
    const auto type = text(entry, "type");
    if (type != "directory" && !includes(type))
    {
        return false;
    }
    if (!selection_.isEmpty())
    {
        bool selected = false;
        for (const auto& value : selection_)
        {
            const auto item = value.toObject();
            const auto source = text(item, "path");
            if (source == path ||
                (type == "directory" && source.startsWith(path + '/')) ||
                (item.value("type") == "directory" &&
                 path.startsWith(source + '/')))
            {
                selected = true;
                break;
            }
        }
        if (!selected)
        {
            return false;
        }
    }
    if (!hasFilters())
    {
        return true;
    }
    if (type == "directory")
    {
        return !excludesDirectorySubtree(
            path, entry.contains("uid") ? number(entry, "uid") : 0,
            entry.contains("mtime_sec")
                ? signedNumber(entry, "mtime_sec")
                : 0);
    }
    auto ruleType = type;
    if (type == "file" && entry.contains("link_group"))
    {
        ruleType = "hardlink";
    }
    const auto size = (type == "file" || type == "hardlink")
                          ? number(entry, "size")
                          : 0;
    const auto uid = entry.contains("uid") ? number(entry, "uid") : 0;
    const auto mtime = entry.contains("mtime_sec")
                           ? signedNumber(entry, "mtime_sec")
                           : 0;
    return matches(path, ruleType, size, uid, mtime);
}

const QJsonObject& SourceScope::filters() const
{
    return filters_;
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
    if (!filters_.isEmpty())
    {
        result.insert("filters", filters_);
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
