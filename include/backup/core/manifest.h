#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>

namespace backup::core
{
inline constexpr int kVersionFormat = 4;
inline constexpr qsizetype kLinkTargetBytes = 4096;

bool supportedEntryType(const QString& type);
bool supportedVersionFormat(int format);
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
    void append(const QJsonObject& entry, int format = kVersionFormat);
    QJsonObject totals() const;
    qint64 count() const;
    bool contains(const QString& path) const;
    // Require every stored directory to lead to an included non-directory.
    void requirePopulatedDirectories() const;

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
