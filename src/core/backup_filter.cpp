#include "backup/core/backup_filter.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"

#include <QRegularExpression>
#include <QSet>
#include <limits>
#include <sys/types.h>

namespace backup::core
{
namespace
{
void requireFields(const QJsonObject& rule, const QSet<QString>& allowed)
{
    for (auto item = rule.begin(); item != rule.end(); ++item)
    {
        require(allowed.contains(item.key()), "Invalid custom filter fields");
    }
}

QRegularExpression wildcard(const QString& pattern, bool path)
{
    QString result = "\\A";
    for (const auto character : pattern)
    {
        if (character == '*')
        {
            result += path ? "[^/]*" : "[\\s\\S]*";
        }
        else if (character == '?')
        {
            result += path ? "[^/]" : "[\\s\\S]";
        }
        else
        {
            result += QRegularExpression::escape(QString(character));
        }
    }
    return QRegularExpression(result + "\\z",
                              QRegularExpression::UseUnicodePropertiesOption);
}

class PathFilter final : public IBackupFilter
{
  public:
    explicit PathFilter(const QString& pattern)
        : directory_(pattern.endsWith('/')),
          expression_(wildcard(directory_ ? pattern.chopped(1) : pattern, true))
    {
    }
    FilterCategory category() const noexcept override
    {
        return FilterCategory::Path;
    }
    bool matches(const FilterEntry& entry) const override
    {
        if ((!directory_ || entry.type == "directory") &&
            expression_.match(entry.path).hasMatch())
        {
            return true;
        }
        // 目录路径命中后包含其子树；通配符本身始终不跨越斜杠。
        auto slash = entry.path.indexOf('/');
        while (slash >= 0)
        {
            if (expression_.match(entry.path.left(slash)).hasMatch())
            {
                return true;
            }
            slash = entry.path.indexOf('/', slash + 1);
        }
        return false;
    }

  private:
    bool directory_;
    QRegularExpression expression_;
};

class NameFilter final : public IBackupFilter
{
  public:
    explicit NameFilter(const QString& pattern)
        : expression_(wildcard(pattern, false))
    {
    }
    FilterCategory category() const noexcept override
    {
        return FilterCategory::Name;
    }
    bool matches(const FilterEntry& entry) const override
    {
        return expression_.match(entry.path.section('/', -1)).hasMatch();
    }

  private:
    QRegularExpression expression_;
};

class TypeFilter final : public IBackupFilter
{
  public:
    explicit TypeFilter(QString type) : type_(std::move(type)) {}
    FilterCategory category() const noexcept override
    {
        return FilterCategory::Type;
    }
    bool matches(const FilterEntry& entry) const override
    {
        return type_ == entry.type;
    }

  private:
    QString type_;
};

class UserFilter final : public IBackupFilter
{
  public:
    explicit UserFilter(qint64 uid) : uid_(uid) {}
    FilterCategory category() const noexcept override
    {
        return FilterCategory::User;
    }
    bool matches(const FilterEntry& entry) const override
    {
        return uid_ == entry.uid;
    }

  private:
    qint64 uid_;
};

class TimeFilter final : public IBackupFilter
{
  public:
    TimeFilter(qint64 minimum, qint64 maximum)
        : minimum_(minimum), maximum_(maximum)
    {
    }
    FilterCategory category() const noexcept override
    {
        return FilterCategory::Mtime;
    }
    bool matches(const FilterEntry& entry) const override
    {
        return entry.mtimeSeconds >= minimum_ && entry.mtimeSeconds <= maximum_;
    }

  private:
    qint64 minimum_;
    qint64 maximum_;
};

class SizeFilter final : public IBackupFilter
{
  public:
    SizeFilter(qint64 minimum, qint64 maximum)
        : minimum_(minimum), maximum_(maximum)
    {
    }
    FilterCategory category() const noexcept override
    {
        return FilterCategory::Size;
    }
    bool matches(const FilterEntry& entry) const override
    {
        return (entry.type == "file" || entry.type == "hardlink") &&
               entry.size >= minimum_ && entry.size <= maximum_;
    }

  private:
    qint64 minimum_;
    qint64 maximum_;
};
} // 匿名命名空间

std::shared_ptr<const IBackupFilter> makeBackupFilter(const QJsonObject& rule)
{
    const auto category = text(rule, "category");
    if (category == "path" || category == "name")
    {
        requireFields(rule, {"category", "pattern"});
        const auto pattern = text(rule, "pattern");
        require(pattern.size() <= 512 && !pattern.contains(QChar(0)) &&
                    !pattern.startsWith('/'),
                "Invalid custom filter pattern");
        if (category == "name")
        {
            require(!pattern.contains('/'), "Invalid custom name filter");
            return std::make_shared<NameFilter>(pattern);
        }
        const auto relative =
            pattern.endsWith('/') ? pattern.chopped(1) : pattern;
        for (const auto& component : relative.split('/'))
        {
            require(!component.isEmpty() && component != ".." &&
                        component != ".",
                    "Invalid custom path filter");
        }
        return std::make_shared<PathFilter>(pattern);
    }
    if (category == "type")
    {
        requireFields(rule, {"category", "value"});
        const auto type = text(rule, "value");
        require(supportedEntryType(type) && type != "socket",
                "Invalid custom type filter");
        return std::make_shared<TypeFilter>(type);
    }
    if (category == "user")
    {
        requireFields(rule, {"category", "value"});
        const auto uid = number(rule, "value");
        require(uid <= std::numeric_limits<uid_t>::max(),
                "Invalid custom UID filter");
        return std::make_shared<UserFilter>(uid);
    }
    require(category == "size" || category == "mtime",
            "Unsupported custom filter category");
    requireFields(rule, {"category", "min", "max"});
    require(rule.contains("min") || rule.contains("max"),
            "Custom range needs a boundary");
    const auto read = [&](const char* field)
    {
        return category == "size" ? number(rule, field)
                                  : signedNumber(rule, field);
    };
    const auto minimum = rule.contains("min") ? read("min")
                         : category == "size"
                             ? 0
                             : std::numeric_limits<qint64>::min();
    const auto maximum =
        rule.contains("max") ? read("max") : std::numeric_limits<qint64>::max();
    require(minimum <= maximum, "Invalid custom filter range");
    if (category == "size")
    {
        return std::make_shared<SizeFilter>(minimum, maximum);
    }
    return std::make_shared<TimeFilter>(minimum, maximum);
}
} // 命名空间 backup::core
