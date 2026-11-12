#include "backup/core/backup_filter.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"

#include <QCoreApplication>
#include <functional>
#include <iostream>
#include <type_traits>

using namespace backup::core;

namespace
{
void rejects(const std::function<void()>& operation)
{
    try
    {
        operation();
    }
    catch (const Error&)
    {
        return;
    }
    throw Error("Invalid filter accepted");
}

SourceScope scope(const QJsonArray& include, const QJsonArray& exclude = {})
{
    return SourceScope(
        {{"path", "/source"},
         {"filters", QJsonObject{{"include", include}, {"exclude", exclude}}}});
}
} // 匿名命名空间

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    static_assert(std::is_abstract_v<IBackupFilter>);
    static_assert(std::has_virtual_destructor_v<IBackupFilter>);
    try
    {
        // 直接通过基类执行六种实现，避免只验证组合器的分支。
        const FilterEntry entry{"sub/文.txt", "file", 10, 1000, -1};
        for (const auto& rule : QJsonArray{
                 QJsonObject{{"category", "path"}, {"pattern", "sub/"}},
                 QJsonObject{{"category", "name"}, {"pattern", "?.txt"}},
                 QJsonObject{{"category", "type"}, {"value", "file"}},
                 QJsonObject{
                     {"category", "size"}, {"min", "10"}, {"max", "10"}},
                 QJsonObject{{"category", "user"}, {"value", "1000"}},
                 QJsonObject{
                     {"category", "mtime"}, {"min", "-1"}, {"max", "-1"}}})
        {
            const std::shared_ptr<const IBackupFilter> filter =
                makeBackupFilter(rule.toObject());
            require(filter->matches(entry), "Virtual filter call failed");
        }
        const auto names =
            scope({QJsonObject{{"category", "name"}, {"pattern", "*.txt"}}});
        require(names.matches("a\n.txt", "file", 0, 0, 0) &&
                    !names.matches("a.txt\n", "file", 0, 0, 0) &&
                    !names.matches("a.TXT", "file", 0, 0, 0),
                "Name matching is not exact or case sensitive");
        const auto paths = scope(
            {QJsonObject{{"category", "path"}, {"pattern", "sub/*.txt"}}});
        require(paths.matches("sub/a.txt", "file", 0, 0, 0) &&
                    !paths.matches("sub/nested/a.txt", "file", 0, 0, 0),
                "Wildcard crossed a path separator");
        const auto sizes = scope(
            {QJsonObject{{"category", "size"}, {"min", "10"}, {"max", "20"}}});
        require(sizes.matches("empty", "directory", 0, 0, 0) &&
                    !sizes.matches("link", "symlink", 10, 0, 0) &&
                    sizes.matches("a", "hardlink", 20, 0, 0) &&
                    !sizes.matches("a", "file", 21, 0, 0),
                "Size applicability or inclusive boundary failed");
        const auto users =
            scope({QJsonObject{{"category", "user"}, {"value", "1001"}},
                   QJsonObject{{"category", "user"}, {"value", "1002"}},
                   QJsonObject{{"category", "name"}, {"pattern", "a"}}},
                  {QJsonObject{{"category", "user"}, {"value", "1002"}}});
        require(users.matches("a", "file", 0, 1001, 0) &&
                    !users.matches("b", "file", 0, 1001, 0) &&
                    !users.matches("a", "file", 0, 1002, 0) &&
                    !users.matches("a", "file", 0, 1000, 0),
                "OR, AND or exclusion precedence failed");
        const auto excluded = scope(
            {}, {QJsonObject{{"category", "name"}, {"pattern", "cache"}}});
        require(excluded.excludesDirectorySubtree("sub/cache", 1000, 0),
                "Directory exclusion did not prune");
        for (const auto& rule : QJsonArray{
                 QJsonObject{{"category", "type"}, {"value", "directory"}},
                 QJsonObject{{"category", "user"}, {"value", "1000"}},
                 QJsonObject{{"category", "mtime"}, {"max", "0"}}})
        {
            require(scope({}, {rule}).excludesDirectorySubtree("sub", 1000, 0),
                    "Non-path directory exclusion did not prune");
        }
        const auto maximum = makeBackupFilter(
            {{"category", "size"}, {"min", "9223372036854775807"}});
        require(maximum->matches({"large", "file", INT64_MAX, 0, 0}),
                "64-bit size lost precision");
        for (const auto& rule : QJsonArray{
                 QJsonObject{{"category", "path"}, {"pattern", "../bad"}},
                 QJsonObject{{"category", "path"}, {"pattern", "a//b"}},
                 QJsonObject{{"category", "name"}, {"pattern", "a/b"}},
                 QJsonObject{{"category", "name"}, {"pattern", ""}},
                 QJsonObject{{"category", "type"}, {"value", "socket"}},
                 QJsonObject{{"category", "user"}, {"value", "4294967296"}},
                 QJsonObject{{"category", "size"}, {"min", "-1"}},
                 QJsonObject{
                     {"category", "size"}, {"min", "21"}, {"max", "20"}},
                 QJsonObject{{"category", "mtime"}, {"min", "1.5"}},
                 QJsonObject{{"category", "mtime"}},
                 QJsonObject{
                     {"category", "name"}, {"pattern", "*"}, {"extra", true}}})
        {
            rejects([&] { makeBackupFilter(rule.toObject()); });
        }
        std::cout << "PASS: six virtual filters, exact patterns, ranges, "
                     "combination and validation\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
