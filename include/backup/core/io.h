#pragma once

// Checked filesystem and JSON operations shared by Agent and Server.
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <stdexcept>
#include <sys/stat.h>

namespace backup::core
{
inline constexpr qint64 kChunkBytes = 256 * 1024;
inline constexpr qint64 kMaxJsonBytes = 1024 * 1024;
inline constexpr int kPageSize = 100;

class Error : public std::runtime_error
{
  public:
    explicit Error(const QString& message);
};

// Throws Error when a runtime precondition or I/O operation fails.
void require(bool condition, const QString& message);
QString newId();
QString now();
QString text(const QJsonObject& object, const char* field);
qint64 number(const QJsonObject& object, const char* field);
bool validId(const QString& value);
bool validPath(const QString& value);
QString canonicalPath(const QString& path);
bool overlaps(const QString& first, const QString& second);
void makeDirectory(const QString& path);
QJsonObject parseObject(const QByteArray& bytes);
QJsonObject readObject(const QString& path);
void writeObject(const QString& path, const QJsonObject& value);
void writeAll(QFile& file, const QByteArray& bytes);
void syncFile(QFile& file);
void syncDirectory(const QString& path);

class Descriptor
{
  public:
    explicit Descriptor(int value = -1);
    ~Descriptor();
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const;
    int release();

  private:
    int value_;
};

// Opens each path component without following links; caller owns returned fd.
int openBelow(int root, const QString& path, int flags, int mode = 0600);
bool isEmptyDirectory(int descriptor);
// Inspects a path without opening device/FIFO streams or following a link.
struct stat statBelow(int root, const QString& path);
bool sameFileState(const struct stat& first, const struct stat& second);
} // namespace backup::core
