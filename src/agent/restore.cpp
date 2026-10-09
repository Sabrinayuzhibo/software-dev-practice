#include "backup/agent/channel.h"
#include "backup/agent/operations.h"
#include "backup/agent/restore_journal.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"
#include "backup/core/source_scope.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <QThread>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
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

void fetchManifest(Channel& channel, const QString& versionId,
                   const QJsonObject& summary, QTemporaryFile& output)
{
    const auto format = summary.value("format").toInt();
    require(supportedVersionFormat(format), "Unsupported version format");
    Manifest catalog;
    const SourceScope scope(summary, "source");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 offset = 0;
    while (true)
    {
        checkCancelled();
        const auto page =
            channel.call("entries", {{"version_id", versionId},
                                     {"offset", QString::number(offset)}});
        require(page.value("entries").isArray(), "Invalid manifest page");
        for (const auto& value : page.value("entries").toArray())
        {
            const auto entry = value.toObject();
            require(scope.allows(entry),
                    "Manifest entry is outside the saved selection: " +
                        text(entry, "path"));
            catalog.append(entry, format);
            const auto line =
                QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
            writeAll(output, line);
            hash.addData(line);
        }
        const auto next = number(page, "offset");
        if (page.value("done").toBool())
        {
            break;
        }
        require(next > offset, "Manifest cursor did not advance");
        offset = next;
    }
    for (const auto& value : scope.selection())
    {
        const auto path = text(value.toObject(), "path");
        require((value.toObject().value("type") == "directory" &&
                 !scope.includes("directory")) ||
                    catalog.contains(path),
                "Selected source missing from manifest: " + path);
    }
    if (scope.hasEmptyDirectoryRule() && !scope.preservesEmptyDirectories())
    {
        catalog.requirePopulatedDirectories();
    }
    const auto totals = catalog.totals();
    for (auto item = totals.begin(); item != totals.end(); ++item)
    {
        const auto field = item.key().toLatin1();
        if ((format == 1 && field != "entries" && field != "files" &&
             field != "directories" && field != "bytes") ||
            (format == 2 &&
             (field == "character_devices" || field == "block_devices" ||
              field == "sockets")))
        {
            continue;
        }
        require(number(summary, field.constData()) ==
                    number(totals, field.constData()),
                "Version manifest statistics mismatch: " + item.key());
    }
    require(QString::fromLatin1(hash.result().toHex()) ==
                text(summary, "manifest_sha256"),
            "Version manifest checksum mismatch");
    require(output.seek(0), "Cannot read verified manifest");
}

void publishFile(int descriptor, int parent, const QByteArray& leaf)
{
    // Follow only our own proc-fd reference, never a mutable source path.
    // Unlike AT_EMPTY_PATH this works without CAP_DAC_READ_SEARCH on Linux.
    const auto pinned = "/proc/self/fd/" + QByteArray::number(descriptor);
    const auto result = ::linkat(AT_FDCWD, pinned.constData(), parent,
                                 leaf.constData(), AT_SYMLINK_FOLLOW);
    require(result == 0, "Cannot create restored hard link or publish file: " +
                             QString::fromLocal8Bit(strerror(errno)));
}

void metadataWarning(QJsonObject& status, const QString& path,
                     const QString& field, int error)
{
    const auto count = number(status, "metadata_warning_count") + 1;
    status.insert("metadata_warning_count", count);
    auto warnings = status.value("warnings").toArray();
    if (warnings.size() < 100)
    {
        warnings.append(QJsonObject{
            {"path", path},
            {"type", "metadata"},
            {"reason", field + ": " + QString::fromLocal8Bit(strerror(error))}});
        status.insert("warnings", warnings);
    }
}

void restoreMetadata(int descriptor, const QJsonObject& entry,
                     QJsonObject& status)
{
    if (!entry.contains("mode"))
    {
        return;
    }
    const auto path = text(entry, "path");
    if (::fchown(descriptor, static_cast<uid_t>(number(entry, "uid")),
                 static_cast<gid_t>(number(entry, "gid"))) != 0)
    {
        metadataWarning(status, path, "owner/group", errno);
    }
    if (::fchmod(descriptor, static_cast<mode_t>(number(entry, "mode"))) != 0)
    {
        metadataWarning(status, path, "permissions", errno);
    }
    const timespec times[2] = {
        {0, UTIME_OMIT},
        {static_cast<time_t>(signedNumber(entry, "mtime_sec")),
         static_cast<long>(number(entry, "mtime_nsec"))}};
    if (::futimens(descriptor, times) != 0)
    {
        metadataWarning(status, path, "modification time", errno);
    }
}

void restoreMetadataAt(int parent, const QByteArray& leaf,
                       const QJsonObject& entry, QJsonObject& status)
{
    if (!entry.contains("mode"))
    {
        return;
    }
    const auto path = text(entry, "path");
    if (::fchownat(parent, leaf.constData(),
                   static_cast<uid_t>(number(entry, "uid")),
                   static_cast<gid_t>(number(entry, "gid")),
                   AT_SYMLINK_NOFOLLOW) != 0)
    {
        metadataWarning(status, path, "owner/group", errno);
    }
    if (text(entry, "type") != "symlink" &&
        ::fchmodat(parent, leaf.constData(),
                   static_cast<mode_t>(number(entry, "mode")), 0) != 0)
    {
        metadataWarning(status, path, "permissions", errno);
    }
    const timespec times[2] = {
        {0, UTIME_OMIT},
        {static_cast<time_t>(signedNumber(entry, "mtime_sec")),
         static_cast<long>(number(entry, "mtime_nsec"))}};
    if (::utimensat(parent, leaf.constData(), times, AT_SYMLINK_NOFOLLOW) != 0)
    {
        metadataWarning(status, path, "modification time", errno);
    }
}

struct stat restoreFile(Channel& channel, const QString& versionId, int parent,
                        const QByteArray& leaf, const QJsonObject& entry,
                        const QByteArray& temporary, QJsonObject& status,
                        const Progress& progress)
{
    Descriptor descriptor(
        ::openat(parent, temporary.constData(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    QFile file;
    require(file.open(descriptor.get(), QIODevice::WriteOnly),
            "Cannot open restore file");
    try
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 offset = 0;
        const auto size = number(entry, "size");
        while (true)
        {
            checkCancelled();
            const auto reply =
                channel.call("download", {{"version_id", versionId},
                                          {"index", entry.value("index")},
                                          {"offset", QString::number(offset)}});
            const auto decoded = QByteArray::fromBase64Encoding(
                reply.value("data").toString().toLatin1(),
                QByteArray::AbortOnBase64DecodingErrors);
            require(bool(decoded) && decoded.decoded.size() <= kChunkBytes &&
                        number(reply, "offset") == offset,
                    "Invalid download block");
            const auto& data = decoded.decoded;
            require(data.size() <= size - offset,
                    "Backup content exceeds declared size");
            writeAll(file, data);
            hash.addData(data);
            offset += data.size();
            status.insert("bytes", number(status, "bytes") + data.size());
            progress(status);
            if (reply.value("done").toBool())
            {
                break;
            }
            require(!data.isEmpty(), "Empty non-final download block");
        }
        require(offset == size && QString::fromLatin1(hash.result().toHex()) ==
                                      text(entry, "sha256"),
                "Restored file checksum mismatch");
        restoreMetadata(descriptor.get(), entry, status);
        syncFile(file);
        publishFile(descriptor.get(), parent, leaf);
        require(::unlinkat(parent, temporary.constData(), 0) == 0 &&
                    ::fsync(parent) == 0,
                "Cannot persist restored file");
        struct stat restored
        {
        };
        require(::fstat(descriptor.get(), &restored) == 0,
                "Cannot inspect restored file");
        return restored;
    }
    catch (...)
    {
        ::unlinkat(parent, temporary.constData(), 0);
        throw;
    }
}

void restoreHardlink(int root, int parent, const QByteArray& leaf,
                     const QJsonObject& entry,
                     QHash<QString, struct stat>& groups)
{
    const auto anchor = text(entry, "link_to");
    require(groups.contains(anchor), "Hard link source has not been restored");
    Descriptor file(openBelow(root, anchor, O_PATH));
    struct stat before
    {
    };
    require(::fstat(file.get(), &before) == 0 && S_ISREG(before.st_mode) &&
                sameFileState(before, groups.value(anchor)),
            "Restored hard link source changed: " + anchor);
    publishFile(file.get(), parent, leaf);
    require(::fsync(parent) == 0, "Cannot persist restored hard link");
    struct stat after
    {
    };
    require(::fstat(file.get(), &after) == 0,
            "Cannot inspect restored hard link");
    // Creating a hard link changes ctime and link count, but not the content.
    before.st_ctim = after.st_ctim;
    require(sameFileState(before, after) &&
                sameFileState(after, statBelow(root, anchor)),
            "Restored hard link source changed: " + anchor);
    groups.insert(anchor, after);
}

void restoreSocket(int parent, const QByteArray& leaf)
{
    Descriptor socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    const auto temporary = (".backup-" + newId()).toUtf8();
    const auto pinned = "/proc/self/fd/" + QByteArray::number(parent) + '/' +
                        temporary;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(pinned.size() <
                static_cast<qsizetype>(sizeof(address.sun_path)),
            "Cannot create restored socket path");
    std::memcpy(address.sun_path, pinned.constData(), pinned.size() + 1);
    const auto bound =
        ::bind(socket.get(), reinterpret_cast<sockaddr*>(&address),
               offsetof(sockaddr_un, sun_path) + pinned.size() + 1);
    const auto bindError = errno;
    require(bound == 0, "Cannot create restored socket: " +
                            QString::fromLocal8Bit(strerror(bindError)));
    try
    {
        const auto protectedNode =
            ::fchmodat(parent, temporary.constData(), 0600,
                       AT_SYMLINK_NOFOLLOW);
        const auto protectError = errno;
        require(protectedNode == 0,
                "Cannot protect restored socket: " +
                    QString::fromLocal8Bit(strerror(protectError)));
        const auto published =
            ::renameat2(parent, temporary.constData(), parent,
                        leaf.constData(), RENAME_NOREPLACE);
        const auto publishError = errno;
        require(published == 0,
                "Cannot publish restored socket: " +
                    QString::fromLocal8Bit(strerror(publishError)));
    }
    catch (...)
    {
        ::unlinkat(parent, temporary.constData(), 0);
        throw;
    }
}

QJsonObject restoreTree(Channel& channel, const QString& versionId,
                        QTemporaryFile& manifest, int root,
                        RestoreJournal& journal, const QString& destination,
                        const Progress& progress)
{
    QJsonObject status{{"stage", "restore"}, {"files", 0},
                       {"directories", 0},   {"bytes", 0},
                       {"symlinks", 0},      {"hardlinks", 0},
                       {"fifos", 0},         {"character_devices", 0},
                       {"block_devices", 0}, {"sockets", 0},
                       {"metadata_warning_count", 0},
                       {"warnings", QJsonArray{}},
                       {"destination", destination}};
    QHash<QString, struct stat> groups;
    QHash<QString, QString> members;
    QVector<QJsonObject> directories;
    while (!manifest.atEnd())
    {
        checkCancelled();
        const auto entry = parseObject(manifest.readLine(64 * 1024));
        const QString path = text(entry, "path");
        const auto type = text(entry, "type");
        status.insert("path", path);
        try
        {
            const auto slash = path.lastIndexOf('/');
            Descriptor directory(slash < 0 ? ::dup(root)
                                           : openBelow(root, path.left(slash),
                                                       O_RDONLY | O_DIRECTORY));
            const auto leaf = path.mid(slash + 1).toUtf8();
            const auto temporary = (".backup-" + newId()).toUtf8();
            auto intent = entry;
            if (type == "file")
            {
                intent.insert("temporary_path",
                              path.left(slash + 1) +
                                  QString::fromUtf8(temporary));
            }
            journal.record(intent, "pending");
            if (type == "file")
            {
                const auto info =
                    restoreFile(channel, versionId, directory.get(), leaf,
                                entry, temporary, status, progress);
                if (entry.contains("link_group"))
                {
                    groups.insert(path, info);
                    members.insert(path, path);
                }
            }
            else if (type == "hardlink")
            {
                restoreHardlink(root, directory.get(), leaf, entry, groups);
                members.insert(path, text(entry, "link_to"));
                status.insert("bytes",
                              number(status, "bytes") + number(entry, "size"));
                status.insert("hardlinks", number(status, "hardlinks") + 1);
            }
            else
            {
                int result = -1;
                if (type == "directory")
                {
                    result = ::mkdirat(directory.get(), leaf.constData(), 0700);
                }
                else if (type == "symlink")
                {
                    const auto target = symlinkTarget(entry);
                    result = ::symlinkat(target.constData(), directory.get(),
                                         leaf.constData());
                }
                else if (type == "fifo")
                {
                    result =
                        ::mkfifoat(directory.get(), leaf.constData(), 0600);
                }
                else if (type == "character_device" ||
                         type == "block_device")
                {
                    const auto identifier = makedev(
                        static_cast<unsigned int>(number(entry, "device_major")),
                        static_cast<unsigned int>(number(entry, "device_minor")));
                    const auto mode =
                        type == "character_device" ? S_IFCHR : S_IFBLK;
                    result = ::mknodat(directory.get(), leaf.constData(),
                                      mode | 0600, identifier);
                }
                else if (type == "socket")
                {
                    restoreSocket(directory.get(), leaf);
                    result = 0;
                }
                require(result == 0,
                        "Cannot restore " + type + ": " +
                            QString::fromLocal8Bit(strerror(errno)));
                if (type == "character_device" || type == "block_device" ||
                    type == "socket")
                {
                    struct stat restored{};
                    require(::fstatat(directory.get(), leaf.constData(),
                                      &restored, AT_SYMLINK_NOFOLLOW) == 0 &&
                                ((type == "socket" &&
                                  S_ISSOCK(restored.st_mode)) ||
                                 (type == "character_device" &&
                                  S_ISCHR(restored.st_mode)) ||
                                 (type == "block_device" &&
                                  S_ISBLK(restored.st_mode))),
                            "Restored node type changed: " + path);
                    if (type != "socket")
                    {
                        require(major(restored.st_rdev) ==
                                        number(entry, "device_major") &&
                                    minor(restored.st_rdev) ==
                                        number(entry, "device_minor"),
                                "Restored device number changed: " + path);
                    }
                }
                require(::fsync(directory.get()) == 0,
                        "Cannot persist restored " + type);
                if (type == "directory")
                {
                    directories.append(entry);
                }
                else if (type == "symlink" || type == "fifo")
                {
                    restoreMetadataAt(directory.get(), leaf, entry, status);
                }
            }
            const auto counter = type == "directory" ? "directories"
                                 : type == "symlink" ? "symlinks"
                                 : type == "fifo"    ? "fifos"
                                 : type == "character_device"
                                     ? "character_devices"
                                 : type == "block_device" ? "block_devices"
                                 : type == "socket"       ? "sockets"
                                                     : "files";
            status.insert(QLatin1String(counter), number(status, counter) + 1);
            journal.record(entry, "written");
            progress(status);
        }
        catch (const std::exception& error)
        {
            throw Error(path + ": " + QString::fromUtf8(error.what()) +
                        "; partial restore at " + destination);
        }
    }
    for (auto item = directories.crbegin(); item != directories.crend(); ++item)
    {
        const auto path = text(*item, "path");
        const auto slash = path.lastIndexOf('/');
        Descriptor parent(slash < 0 ? ::dup(root)
                                    : openBelow(root, path.left(slash),
                                                O_RDONLY | O_DIRECTORY));
        restoreMetadataAt(parent.get(), path.mid(slash + 1).toUtf8(), *item,
                          status);
        require(::fsync(parent.get()) == 0,
                "Cannot persist restored directory metadata: " + path);
    }
    if (number(status, "metadata_warning_count") > 0)
    {
        status.insert("warning_count", number(status, "metadata_warning_count"));
    }
    for (auto member = members.begin(); member != members.end(); ++member)
    {
        checkCancelled();
        require(sameFileState(groups.value(member.value()),
                              statBelow(root, member.key())),
                "Restored hard link member changed: " + member.key() +
                    "; partial restore at " + destination);
    }
    return status;
}

void checkDestination(const QString& destination, const QString& dataRoot,
                      const QString& stateDirectory, const QJsonArray& tasks,
                      const QJsonArray& targets)
{
    require(!overlaps(destination, canonicalPath(dataRoot)) &&
                !overlaps(destination, stateDirectory),
            "Restore destination overlaps application data");
    for (const auto& value : tasks)
    {
        require(!SourceScope(value.toObject()).overlaps(destination),
                "Restore destination overlaps a configured source");
    }
    for (const auto& value : targets)
    {
        const auto repository = value.toObject();
        for (const auto* field : {"data_root", "repository_path"})
        {
            if (repository.contains(QLatin1String(field)))
            {
                require(!overlaps(destination,
                                  canonicalPath(text(repository, field))),
                        "Restore destination overlaps a configured repository");
            }
        }
    }
}
} // namespace

QJsonObject restore(const QJsonObject& args, const QJsonObject& target,
                    const QJsonArray& tasks, const QJsonArray& targets,
                    const QString& stateDirectory, const Progress& progress)
{
    Channel channel(target);
    channel.authenticate();
    const QString versionId = text(args, "version_id");
    const auto summary = channel.call("version", {{"version_id", versionId}});
    const QString supplied = text(args, "destination");
    require(!QFileInfo(supplied).isSymLink(),
            "Restore root cannot be a symbolic link");
    const QString destination = canonicalPath(supplied);
    checkDestination(destination, text(channel.health(), "data_root"),
                     stateDirectory, tasks, targets);
    QTemporaryFile manifest;
    require(manifest.open(), "Cannot create temporary manifest");
    progress({{"stage", "verify"}, {"files", 0}, {"bytes", 0}});
    fetchManifest(channel, versionId, summary, manifest);
    RestoreJournal journal(stateDirectory, text(args, "operation_id"));
    progress({{"restore_journal", true}});

    // Resolve parents without following links; mkdir only creates the leaf.
    Descriptor filesystem(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const auto parentPath = QFileInfo(destination).absolutePath();
    Descriptor parent(parentPath == "/"
                          ? ::dup(filesystem.get())
                          : openBelow(filesystem.get(), parentPath.mid(1),
                                      O_RDONLY | O_DIRECTORY));
    const auto leaf = QFileInfo(destination).fileName().toUtf8();
    require(!leaf.isEmpty(), "Cannot restore to filesystem root");
    if (::mkdirat(parent.get(), leaf.constData(), 0700) != 0)
    {
        require(errno == EEXIST, "Cannot create restore directory");
    }
    Descriptor root(::openat(parent.get(), leaf.constData(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    require(isEmptyDirectory(root.get()), "Restore destination must be empty");
    require(::fsync(parent.get()) == 0, "Cannot persist restore directory");
    auto status = restoreTree(channel, versionId, manifest, root.get(), journal,
                              destination, progress);
    if (summary.value("root_metadata").isObject())
    {
        auto metadata = summary.value("root_metadata").toObject();
        metadata.insert("path", ".");
        restoreMetadata(root.get(), metadata, status);
    }
    require(::fsync(root.get()) == 0 && ::fsync(parent.get()) == 0,
            "Cannot persist restored tree");
    QJsonArray warnings;
    qint64 offset = 0;
    do
    {
        const auto page =
            channel.call("version_warnings",
                         {{"version_id", versionId}, {"offset", offset}});
        for (const auto& warning : page.value("warnings").toArray())
        {
            warnings.append(warning);
        }
        offset = page.value("next_offset").isNull()
                     ? -1
                     : number(page, "next_offset");
    } while (offset >= 0);
    auto restoreWarnings = status.value("warnings").toArray();
    for (const auto& warning : warnings)
    {
        if (restoreWarnings.size() < 100)
        {
            restoreWarnings.append(warning);
        }
    }
    status.insert("warnings", restoreWarnings);
    return status;
}
} // namespace backup::agent
