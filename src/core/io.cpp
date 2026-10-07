#include "backup/core/io.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <cmath>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace backup::core
{
Error::Error(const QString& message) : std::runtime_error(message.toStdString())
{
}

void require(bool condition, const QString& message)
{
    if (!condition)
    {
        throw Error(message);
    }
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QString now()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString text(const QJsonObject& object, const char* field)
{
    const auto value = object.value(QLatin1String(field));
    require(value.isString() && !value.toString().isEmpty(),
            QStringLiteral("Missing or invalid field: %1").arg(field));
    return value.toString();
}

qint64 number(const QJsonObject& object, const char* field)
{
    const auto value = object.value(QLatin1String(field));
    bool ok = false;
    qint64 result = -1;
    if (value.isString())
    {
        result = value.toString().toLongLong(&ok);
    }
    else if (value.isDouble())
    {
        const double input = value.toDouble();
        ok = std::isfinite(input) && input >= 0 &&
             input <= 9007199254740991.0 && std::floor(input) == input;
        if (ok)
        {
            result = static_cast<qint64>(input);
        }
    }
    require(ok && result >= 0, QStringLiteral("Invalid number: %1").arg(field));
    return result;
}

bool validId(const QString& value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$"));
    return pattern.match(value).hasMatch();
}

bool validPath(const QString& value)
{
    if (value.isEmpty() || value.startsWith('/') || value.contains(QChar(0)) ||
        value.toUtf8().size() > 4096)
    {
        return false;
    }
    for (const auto& component : value.split('/'))
    {
        if (component.isEmpty() || component == "." || component == "..")
        {
            return false;
        }
    }
    return true;
}

QString canonicalPath(const QString& path)
{
    require(!path.isEmpty() && !path.contains(QChar(0)), "Invalid path");
    QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    QStringList missing;
    while (!info.exists())
    {
        require(!info.isSymLink(),
                "Dangling symbolic link: " + info.filePath());
        missing.prepend(info.fileName());
        info.setFile(info.absolutePath());
    }
    QString result = info.canonicalFilePath();
    require(!result.isEmpty(), "Cannot resolve path: " + path);
    for (const auto& component : missing)
    {
        result = QDir(result).filePath(component);
    }
    return QDir::cleanPath(result);
}

bool overlaps(const QString& first, const QString& second)
{
    const auto contains = [](const QString& parent, const QString& child) {
        return parent == child || parent == "/" ||
               child.startsWith(parent + '/');
    };
    return contains(first, second) || contains(second, first);
}

void makeDirectory(const QString& path)
{
    require(QDir().mkpath(path), "Cannot create directory: " + path);
}

QJsonObject parseObject(const QByteArray& bytes)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            "Invalid JSON data");
    return document.object();
}

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read: " + path);
    require(file.size() <= kMaxJsonBytes, "JSON file too large: " + path);
    return parseObject(file.readAll());
}

void writeObject(const QString& path, const QJsonObject& value)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly), "Cannot save: " + path);
    require(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
            "Cannot protect: " + path);
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    require(bytes.size() <= kMaxJsonBytes, "JSON state exceeds supported size");
    require(file.write(bytes) == bytes.size(), "Cannot write: " + path);
    require(file.commit(), "Cannot commit: " + path);
    syncDirectory(QFileInfo(path).absolutePath());
}

void writeAll(QFile& file, const QByteArray& bytes)
{
    qint64 offset = 0;
    while (offset < bytes.size())
    {
        const auto written =
            file.write(bytes.constData() + offset, bytes.size() - offset);
        require(written > 0,
                "Write failed: " + file.fileName() + ": " + file.errorString());
        offset += written;
    }
}

void syncFile(QFile& file)
{
    const auto flushed = file.flush();
    require(flushed,
            "Cannot flush: " + file.fileName() + ": " + file.errorString());
    const auto synced = ::fsync(file.handle());
    require(synced == 0, "Cannot persist: " + file.fileName() + ": " +
                             QString::fromLocal8Bit(strerror(errno)));
}

void syncDirectory(const QString& path)
{
    Descriptor descriptor(::open(QFile::encodeName(path).constData(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    require(::fsync(descriptor.get()) == 0, "Cannot sync directory: " + path);
}

Descriptor::Descriptor(int value) : value_(value)
{
    require(value >= 0, QString::fromLocal8Bit(strerror(errno)));
}
Descriptor::~Descriptor()
{
    if (value_ >= 0)
    {
        ::close(value_);
    }
}
int Descriptor::get() const
{
    return value_;
}
int Descriptor::release()
{
    const int result = value_;
    value_ = -1;
    return result;
}

int openBelow(int root, const QString& path, int flags, int mode)
{
    require(validPath(path), "Unsafe relative path: " + path);
    Descriptor current(::dup(root));
    const auto components = path.split('/');
    for (qsizetype index = 0; index < components.size(); ++index)
    {
        const bool last = index == components.size() - 1;
        const int options = last ? flags : O_RDONLY | O_DIRECTORY;
        Descriptor next(::openat(current.get(),
                                 components[index].toUtf8().constData(),
                                 options | O_NOFOLLOW | O_CLOEXEC, mode));
        if (last)
        {
            return next.release();
        }
        require(::dup2(next.get(), current.get()) >= 0,
                "Cannot open directory");
    }
    throw Error("Empty relative path");
}

bool isEmptyDirectory(int descriptor)
{
    Descriptor copy(::dup(descriptor));
    DIR* directory = ::fdopendir(copy.get());
    require(directory != nullptr, "Cannot inspect restore directory");
    copy.release();
    const auto closeDirectory = [](DIR* value) { ::closedir(value); };
    std::unique_ptr<DIR, decltype(closeDirectory)> owner(directory,
                                                         closeDirectory);
    while (true)
    {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry)
        {
            require(errno == 0, "Cannot read restore directory");
            return true;
        }
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
        {
            return false;
        }
    }
}
} // namespace backup::core
