#include "source_entries.h"
#include "backup/agent/worker_support.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"

#include <QCryptographicHash>
#include <fcntl.h>
#include <limits>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
void incrementSourceCounter(QJsonObject& status, const char* field,
                            qint64 count)
{
    const auto previous = number(status, field);
    require(count >= 0 &&
                count <= std::numeric_limits<qint64>::max() - previous,
            "Source statistics overflow");
    status.insert(QLatin1String(field), previous + count);
}

QString sourceEntryType(mode_t mode)
{
    if (S_ISREG(mode))
    {
        return "file";
    }
    if (S_ISDIR(mode))
    {
        return "directory";
    }
    if (S_ISLNK(mode))
    {
        return "symlink";
    }
    if (S_ISFIFO(mode))
    {
        return "fifo";
    }
    if (S_ISCHR(mode))
    {
        return "character_device";
    }
    if (S_ISBLK(mode))
    {
        return "block_device";
    }
    if (S_ISSOCK(mode))
    {
        return "socket";
    }
    return "unknown";
}

bool sameSourceDirectoryState(const struct stat& before,
                              const struct stat& after)
{
    return S_ISDIR(after.st_mode) && before.st_dev == after.st_dev &&
           before.st_ino == after.st_ino;
}

namespace
{
void addMetadata(QJsonObject& entry, const struct stat& info)
{
    entry.insert("mode", static_cast<qint64>(info.st_mode & 0777));
    entry.insert("uid", QString::number(info.st_uid));
    entry.insert("gid", QString::number(info.st_gid));
    entry.insert("mtime_sec", QString::number(info.st_mtim.tv_sec));
    entry.insert("mtime_nsec", QString::number(info.st_mtim.tv_nsec));
}

} // namespace

QJsonObject SourceEntries::regular(const QString& path, const struct stat& info,
                                   QJsonObject& status)
{
    Descriptor descriptor(openBelow(root_, path, O_RDONLY | O_NONBLOCK));
    struct stat before
    {
    };
    require(::fstat(descriptor.get(), &before) == 0 &&
                S_ISREG(before.st_mode) && sameFileState(info, before),
            "Source changed before read: " + path);
    const auto key = std::make_pair(info.st_dev, info.st_ino);
    const auto group = groups_.find(key);
    QJsonObject entry{{"path", path},
                      {"type", "file"},
                      {"size", QString::number(before.st_size)}};
    addMetadata(entry, before);
    if (group != groups_.end())
    {
        const auto anchorPath = text(group->second, "path");
        require(sameFileState(snapshots_.value(anchorPath), before) &&
                    sameFileState(before, statBelow(root_, anchorPath)),
                "Hard link group changed: " + path);
        entry.insert("type", "hardlink");
        entry.insert("link_to", anchorPath);
        entry.insert("sha256", group->second.value("sha256"));
        sink_.entry(entry);
        incrementSourceCounter(status, "bytes", before.st_size);
        incrementSourceCounter(status, "hardlinks");
        snapshots_.insert(path, before);
    }
    else
    {
        if (before.st_nlink > 1)
        {
            entry.insert("link_group", path);
        }
        sink_.entry(entry);
        QFile file;
        require(file.open(descriptor.get(), QIODevice::ReadOnly),
                "Cannot read: " + path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 offset = 0;
        while (true)
        {
            checkOperationCancelled();
            const auto chunk = file.read(kChunkBytes);
            require(file.error() == QFileDevice::NoError,
                    "Cannot read: " + path);
            if (chunk.isEmpty())
            {
                break;
            }
            require(chunk.size() <= before.st_size - offset,
                    "Source changed during read: " + path);
            hash.addData(chunk);
            sink_.chunk(offset, chunk);
            offset += chunk.size();
            incrementSourceCounter(status, "bytes", chunk.size());
            progress_(status);
        }
        require(offset == before.st_size &&
                    sameFileState(before, statBelow(root_, path)),
                "Source changed during read: " + path);
        const auto digest = QString::fromLatin1(hash.result().toHex());
        entry.insert("sha256", digest);
        sink_.finishFile(digest);
        incrementSourceCounter(status, "stored_bytes", before.st_size);
        if (before.st_nlink > 1)
        {
            groups_.emplace(key, entry);
            snapshots_.insert(path, before);
        }
    }
    incrementSourceCounter(status, "files");
    return entry;
}

QJsonObject SourceEntries::process(const QString& path, const struct stat& info,
                                   QJsonObject& status, bool structural)
{
    const auto type = sourceEntryType(info.st_mode);
    if (type == "file")
    {
        return regular(path, info, status);
    }
    QJsonObject entry{{"path", path}, {"type", type}};
    require(type != "socket", "Socket node backups are no longer supported");
    if (supportedEntryType(type))
    {
        addMetadata(entry, info);
    }
    if (type == "symlink")
    {
        // O_PATH 与空路径 readlinkat 固定链接本身，不跟随目录、悬空、
        // 绝对路径或非 UTF-8 的链接目标。
        Descriptor link(openBelow(root_, path, O_PATH));
        struct stat pinned
        {
        };
        require(::fstat(link.get(), &pinned) == 0 &&
                    sameFileState(info, pinned),
                "Symbolic link changed before read: " + path);
        QByteArray target(kLinkTargetBytes + 1, '\0');
        const auto count =
            ::readlinkat(link.get(), "", target.data(), target.size());
        require(count > 0 && count < target.size(),
                "Cannot read symbolic link: " + path);
        target.resize(count);
        entry.insert("target_base64", QString::fromLatin1(target.toBase64()));
        symlinkTarget(entry);
        require(sameFileState(info, statBelow(root_, path)),
                "Symbolic link changed: " + path);
    }
    if (type == "character_device" || type == "block_device")
    {
        entry.insert("device_major", QString::number(major(info.st_rdev)));
        entry.insert("device_minor", QString::number(minor(info.st_rdev)));
    }
    if (!supportedEntryType(type))
    {
        entry.insert("reason", "Unsupported " + type + " entry skipped");
        return entry;
    }
    sink_.entry(entry);
    const auto counter = type == "directory"          ? "directories"
                         : type == "symlink"          ? "symlinks"
                         : type == "fifo"             ? "fifos"
                         : type == "character_device" ? "character_devices"
                                                      : "block_devices";
    incrementSourceCounter(status, counter);
    snapshots_.insert(path, info);
    if (structural)
    {
        structural_.insert(path);
    }
    return entry;
}

void SourceEntries::verify() const
{
    for (auto item = snapshots_.begin(); item != snapshots_.end(); ++item)
    {
        checkOperationCancelled();
        const auto after = statBelow(root_, item.key());
        require(structural_.contains(item.key())
                    ? sameSourceDirectoryState(item.value(), after)
                    : sameFileState(item.value(), after),
                "Source entry changed during traversal: " + item.key());
    }
}

} // namespace backup::agent
