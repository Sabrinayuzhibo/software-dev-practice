#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>

namespace backup::core
{
inline constexpr int kPlainVersionFormat = 4;
inline constexpr int kVersionFormat = 5;
inline constexpr qsizetype kLinkTargetBytes = 4096;

bool supportedEntryType(const QString& type);
bool supportedVersionFormat(int format);
// 旧版本可不含元数据；一旦提供就必须完整、合法，否则抛出 Error。
void validateMetadata(const QJsonObject& entry);
// 返回受支持属性的规范表示；缺失元数据时返回空对象，非法字段抛出 Error。
QJsonObject metadataFields(const QJsonObject& entry);
// Targets are raw bytes, independent of UTF-8 filenames. Rejects
// NUL/truncation.
QByteArray symlinkTarget(const QJsonObject& entry);

// Validates ordered entries before publication or any restore writes. A link
// group is named by its first ordinary file; aliases may only refer backwards
// to that group's verified content in this manifest. Invalid data throws Error.
class Manifest
{
  public:
    void checkPath(const QString& path) const;
    void append(const QJsonObject& entry, int format = kPlainVersionFormat);
    QJsonObject totals() const;
    qint64 count() const;
    bool contains(const QString& path) const;
    // 每个目录必须包含入选条目；allowedEmpty 中的目录允许为空，
    // 并可为其补齐必要的结构父目录。不符合要求时抛出 Error。
    void
    requirePopulatedDirectories(const QSet<QString>& allowedEmpty = {}) const;

  private:
    QSet<QString> paths_;
    QSet<QString> directories_;
    QSet<QString> contentParents_;
    QHash<QString, QJsonObject> groups_;
    qint64 files_ = 0;
    qint64 bytes_ = 0;
    qint64 storedBytes_ = 0;
    qint64 symlinks_ = 0;
    qint64 hardlinks_ = 0;
    qint64 fifos_ = 0;
    qint64 characterDevices_ = 0;
    qint64 blockDevices_ = 0;
    qint64 sockets_ = 0;
};
} // namespace backup::core
