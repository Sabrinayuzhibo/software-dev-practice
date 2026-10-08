#include "backup/agent/source_tree.h"
#include "backup/agent/channel.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"
#include "backup/core/source_scope.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <map>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
void checkCancelled()
{
    require(!QThread::currentThread()->isInterruptionRequested(),
            "Operation interrupted");
}

void increment(QJsonObject& status, const char* field, qint64 count = 1)
{
    const auto previous = number(status, field);
    require(count >= 0 &&
                count <= std::numeric_limits<qint64>::max() - previous,
            "Source statistics overflow");
    status.insert(QLatin1String(field), previous + count);
}

QString entryType(mode_t mode)
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

bool sameDirectory(const struct stat& before, const struct stat& after)
{
    return S_ISDIR(after.st_mode) && before.st_dev == after.st_dev &&
           before.st_ino == after.st_ino;
}

class SourceEntries
{
  public:
    SourceEntries(int root, Channel* channel, const Progress& progress)
        : root_(root), channel_(channel), progress_(progress)
    {
    }
    QJsonObject process(const QString& path, const struct stat& info,
                        QJsonObject& status, bool structural = false);
    void verify() const;

  private:
    QJsonObject regular(const QString& path, const struct stat& info,
                        QJsonObject& status);
    int root_;
    Channel* channel_;
    const Progress& progress_;
    std::map<std::pair<dev_t, ino_t>, QJsonObject> groups_;
    QHash<QString, struct stat> snapshots_;
    QSet<QString> structural_;
};

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
    if (group != groups_.end())
    {
        const auto anchorPath = text(group->second, "path");
        require(sameFileState(snapshots_.value(anchorPath), before) &&
                    sameFileState(before, statBelow(root_, anchorPath)),
                "Hard link group changed: " + path);
        entry.insert("type", "hardlink");
        entry.insert("link_to", anchorPath);
        entry.insert("sha256", group->second.value("sha256"));
        if (channel_)
        {
            channel_->call("entry", entry);
        }
        increment(status, "bytes", before.st_size);
        increment(status, "hardlinks");
        snapshots_.insert(path, before);
    }
    else
    {
        if (before.st_nlink > 1)
        {
            entry.insert("link_group", path);
        }
        if (channel_)
        {
            channel_->call("entry", entry);
        }
        QFile file;
        require(file.open(descriptor.get(), QIODevice::ReadOnly),
                "Cannot read: " + path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 offset = 0;
        while (true)
        {
            checkCancelled();
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
            if (channel_)
            {
                channel_->call(
                    "chunk", {{"offset", QString::number(offset)},
                              {"data", QString::fromLatin1(chunk.toBase64())}});
            }
            offset += chunk.size();
            increment(status, "bytes", chunk.size());
            progress_(status);
        }
        require(offset == before.st_size &&
                    sameFileState(before, statBelow(root_, path)),
                "Source changed during read: " + path);
        const auto digest = QString::fromLatin1(hash.result().toHex());
        entry.insert("sha256", digest);
        if (channel_)
        {
            channel_->call("finish_file", {{"sha256", digest}});
        }
        increment(status, "stored_bytes", before.st_size);
        if (before.st_nlink > 1)
        {
            groups_.emplace(key, entry);
            snapshots_.insert(path, before);
        }
    }
    increment(status, "files");
    return entry;
}

QJsonObject SourceEntries::process(const QString& path, const struct stat& info,
                                   QJsonObject& status, bool structural)
{
    const auto type = entryType(info.st_mode);
    if (type == "file")
    {
        return regular(path, info, status);
    }
    QJsonObject entry{{"path", path}, {"type", type}};
    if (type == "symlink")
    {
        // O_PATH and empty-path readlinkat pin the link itself, even if its
        // target is a directory, dangling, absolute or not valid UTF-8.
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
    if (channel_)
    {
        channel_->call("entry", entry);
    }
    const auto counter = type == "directory"        ? "directories"
                         : type == "symlink"        ? "symlinks"
                         : type == "fifo"           ? "fifos"
                         : type == "character_device" ? "character_devices"
                         : type == "block_device"   ? "block_devices"
                                                     : "sockets";
    increment(status, counter);
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
        checkCancelled();
        const auto after = statBelow(root_, item.key());
        require(structural_.contains(item.key())
                    ? sameDirectory(item.value(), after)
                    : sameFileState(item.value(), after),
                "Source entry changed during traversal: " + item.key());
    }
}

void addSample(QJsonArray& sample, QJsonObject entry)
{
    if (entry.value("type") == "symlink" && entry.contains("target_base64"))
    {
        entry.insert("link_target", QString::fromUtf8(symlinkTarget(entry)));
        entry.remove("target_base64");
    }
    if (entry.contains("size"))
    {
        entry.insert("size", number(entry, "size"));
    }
    const auto fits = [&]
    {
        return sample.size() < kPageSize &&
               QJsonDocument(sample).toJson(QJsonDocument::Compact).size() +
                       detailLine(entry).size() <
                   kDetailPageBytes;
    };
    if (!entry.contains("reason"))
    {
        for (qsizetype index = sample.size(); index > 0 && !fits(); --index)
        {
            if (sample[index - 1].toObject().contains("reason"))
            {
                sample.removeAt(index - 1);
            }
        }
    }
    if (fits())
    {
        sample.append(entry);
    }
}
} // namespace

QJsonObject sourceTree(const QJsonObject& source, Channel* channel,
                       const Progress& progress)
{
    const SourceScope scope(source);
    const auto rootPath = canonicalPath(scope.root());
    require(rootPath == scope.root(),
            "Source root has changed: " + scope.root());
    Descriptor filesystem(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
    const int flags =
        (scope.selection().isEmpty() ? O_RDONLY : O_PATH) | O_DIRECTORY;
    // Resolve every parent without following links, including the logical
    // root. Selected files do not require listing that root directory.
    Descriptor root(rootPath == "/"
                        ? ::open("/", flags | O_CLOEXEC)
                        : openBelow(filesystem.get(), rootPath.mid(1), flags));
    struct stat rootBefore
    {
    };
    require(::fstat(root.get(), &rootBefore) == 0, "Cannot inspect source");
    QJsonObject status{{"stage", channel ? "upload" : "scan"},
                       {"root", rootPath},
                       {"files", 0},
                       {"directories", 0},
                       {"bytes", 0},
                       {"stored_bytes", 0},
                       {"symlinks", 0},
                       {"hardlinks", 0},
                       {"fifos", 0},
                       {"character_devices", 0},
                       {"block_devices", 0},
                       {"sockets", 0},
                       {"complete", true},
                       {"error_count", 0}};
    SourceEntries entries(root.get(), channel, progress);
    QJsonArray sample;
    QJsonArray warnings;
    const auto problem = [&](const QString& path, const QString& reason)
    {
        if (channel)
        {
            throw Error(path + ": " + reason);
        }
        warnings.append(QJsonObject{{"path", path.isEmpty() ? "." : path},
                                    {"reason", reason},
                                    {"severity", "error"}});
        status.insert("complete", false);
        increment(status, "error_count");
    };
    QSet<QString> processed;
    QHash<QString, struct stat> deferredDirectories;
    std::function<void(const QString&)> visit;
    const auto includeParents = [&](const QString& path)
    {
        QString parent;
        const auto components = path.split('/');
        for (qsizetype index = 0; index + 1 < components.size(); ++index)
        {
            parent = parent.isEmpty() ? components[index]
                                      : parent + '/' + components[index];
            if (deferredDirectories.contains(parent))
            {
                const auto info = deferredDirectories.take(parent);
                addSample(sample, entries.process(parent, info, status));
            }
        }
    };
    const auto process =
        [&](const QString& path, const QString& expected, bool structural)
    {
        checkCancelled();
        if (processed.contains(path))
        {
            return;
        }
        const auto previous = number(status, "bytes");
        QString type = expected;
        try
        {
            const auto info = statBelow(root.get(), path);
            type = entryType(info.st_mode);
            require(expected.isEmpty() || type == expected,
                    "Selected source type changed: " + path);
            status.insert("path", path);
            if (type == "directory" && !structural &&
                !scope.includes("directory"))
            {
                deferredDirectories.insert(path, info);
                processed.insert(path);
                visit(path);
                progress(status);
                return;
            }
            if ((type == "character_device" || type == "block_device" ||
                 type == "socket") &&
                !scope.includes(type) &&
                (scope.fileTypes().isEmpty() ||
                 scope.hasEmptyDirectoryRule()))
            {
                QJsonObject skipped{{"path", path},
                                    {"type", type},
                                    {"reason", type +
                                                   " node skipped; select this "
                                                   "type to back it up"}};
                addSample(sample, skipped);
                warnings.append(skipped);
                processed.insert(path);
                progress(status);
                return;
            }
            if (type != "directory" && !scope.includes(type) &&
                (supportedEntryType(type) || !scope.hasEmptyDirectoryRule()))
            {
                processed.insert(path);
                return;
            }
            if (supportedEntryType(type))
            {
                includeParents(path);
            }
            const auto entry = entries.process(path, info, status, structural);
            processed.insert(path);
            addSample(sample, entry);
            if (entry.contains("reason"))
            {
                warnings.append(entry);
            }
            if (S_ISDIR(info.st_mode) && !structural)
            {
                visit(path);
            }
            progress(status);
        }
        catch (const std::exception& failure)
        {
            checkCancelled();
            if (!channel)
            {
                status.insert("bytes", previous);
                if (!type.isEmpty() && !processed.contains(path))
                {
                    addSample(sample, {{"path", path}, {"type", type}});
                }
            }
            problem(path, QString::fromUtf8(failure.what()));
        }
    };
    visit = [&](const QString& relative)
    {
        checkCancelled();
        try
        {
            Descriptor directory(
                relative.isEmpty()
                    ? ::dup(root.get())
                    : openBelow(root.get(), relative, O_RDONLY | O_DIRECTORY));
            // Enumerate the pinned directory, never a path swapped to a link.
            const auto pinned =
                "/proc/self/fd/" + std::to_string(directory.get());
            std::error_code error;
            auto iterator = std::filesystem::directory_iterator(pinned, error);
            require(!error, QString::fromStdString(error.message()));
            while (iterator != std::filesystem::directory_iterator())
            {
                checkCancelled();
                const auto native = iterator->path().filename().native();
                const auto name =
                    QString::fromUtf8(native.data(), native.size());
                const auto path =
                    relative.isEmpty() ? name : relative + '/' + name;
                try
                {
                    require(name.toUtf8().toStdString() == native &&
                                validPath(path),
                            "Unsupported filename encoding or path");
                    process(path, {}, false);
                }
                catch (const std::exception& failure)
                {
                    checkCancelled();
                    problem(path, QString::fromUtf8(failure.what()));
                }
                iterator.increment(error);
                require(!error, QString::fromStdString(error.message()));
            }
        }
        catch (const std::exception& failure)
        {
            checkCancelled();
            problem(relative, QString::fromUtf8(failure.what()));
        }
    };
    if (scope.selection().isEmpty())
    {
        visit({});
    }
    else
    {
        for (const auto& value : scope.selection())
        {
            const auto item = value.toObject();
            const auto path = text(item, "path");
            // Structural parents are created without scanning their siblings.
            QString parent;
            const auto components = path.split('/');
            for (qsizetype index = 0; index + 1 < components.size(); ++index)
            {
                parent = parent.isEmpty() ? components[index]
                                          : parent + '/' + components[index];
                process(parent, "directory", true);
            }
            process(path, text(item, "type"), false);
        }
    }
    try
    {
        entries.verify();
        for (auto item = deferredDirectories.begin();
             item != deferredDirectories.end(); ++item)
        {
            checkCancelled();
            require(sameFileState(item.value(), statBelow(root.get(), item.key())),
                    "Source directory changed during traversal: " + item.key());
        }
        struct stat after
        {
        };
        require(::fstat(root.get(), &after) == 0 &&
                    (scope.selection().isEmpty()
                         ? sameFileState(rootBefore, after)
                         : sameDirectory(rootBefore, after)),
                "Source directory changed during traversal");
    }
    catch (const std::exception& failure)
    {
        checkCancelled();
        problem(".", QString::fromUtf8(failure.what()));
    }
    status.insert("sample", sample);
    status.insert("warnings", warnings);
    status.insert("entries",
                  number(status, "files") + number(status, "directories") +
                      number(status, "symlinks") + number(status, "fifos") +
                      number(status, "character_devices") +
                      number(status, "block_devices") +
                      number(status, "sockets"));
    status.insert("hashed_files",
                  number(status, "files") - number(status, "hardlinks"));
    status.insert("unreadable_files", status.value("error_count"));
    return status;
}
} // namespace backup::agent
