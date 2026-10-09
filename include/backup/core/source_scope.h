#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

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
    bool matches(const QString& path, const QString& type, qint64 size,
                 qint64 uid, qint64 mtimeSeconds) const;
    bool excludesDirectorySubtree(const QString& path, qint64 uid,
                                  qint64 mtimeSeconds) const;
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
};
} // namespace backup::core
