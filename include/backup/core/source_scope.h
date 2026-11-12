#pragma once

#include "backup/core/backup_filter.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <vector>

namespace backup::core
{
inline constexpr qsizetype kMaxSelectedSources = 100;
inline constexpr qsizetype kSourceScopeBytes = 16 * 1024;

// Missing selection means the complete directory (legacy tasks). A selection
// contains disjoint relative paths with fixed types, never pattern matching.
// Validates persisted or wire data without requiring source files to exist.
class SourceScope
{
  public:
    explicit SourceScope(const QJsonObject& value, const char* field = "path");
    const QString& root() const;
    const QJsonArray& selection() const;
    const QJsonArray& fileTypes() const;
    bool hasEmptyDirectoryRule() const;
    bool preservesEmptyDirectories() const;
    bool includes(const QString& type) const;
    bool hasFilters() const;
    // 对可信 lstat 数据应用选入/排除组合；尺寸与时间边界均包含端点。
    // 目录忽略尺寸选入，结构父目录可忽略选入条件，但不能绕过排除。
    bool matches(const QString& path, const QString& type, qint64 size,
                 qint64 uid, qint64 mtimeSeconds) const;
    // 任意排除条件命中目录时返回 true，遍历者须停止访问全部后代。
    bool excludesDirectorySubtree(const QString& path, qint64 uid,
                                  qint64 mtimeSeconds) const;
    // 校验传入清单的来源范围及规则；带筛选的条目缺少元数据时抛出 Error。
    bool allows(const QJsonObject& entry) const;
    const QJsonObject& filters() const;
    QStringList paths() const;
    QJsonObject json(const char* field = "path") const;
    bool allows(const QString& path, const QString& type) const;
    // Resolves parent directories, but preserves a selected symlink itself.
    bool overlaps(const QString& directory) const;

  private:
    QString root_;
    QJsonArray selection_;
    QJsonArray fileTypes_;
    bool hasEmptyDirectoryRule_ = false;
    bool preserveEmptyDirectories_ = true;
    QJsonObject filters_;
    std::vector<std::shared_ptr<const IBackupFilter>> includeFilters_;
    std::vector<std::shared_ptr<const IBackupFilter>> excludeFilters_;
};
} // namespace backup::core
