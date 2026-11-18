#include "backup/agent/channel.h"
#include "backup/agent/metadata_restore.h"
#include "backup/agent/operations.h"
#include "backup/agent/restore_journal.h"
#include "backup/agent/restore_node.h"
#include "backup/agent/worker_support.h"
#include "backup/core/compression.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"
#include "backup/core/source_scope.h"
#include "backup/core/version_verifier.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
void fetchManifest(Channel& channel, const QString& versionId,
                   const QJsonObject& summary, QTemporaryFile& output)
{
    VersionVerifier verifier(summary);
    qint64 offset = 0;
    while (true)
    {
        checkOperationCancelled();
        const auto page =
            channel.call("entries", {{"version_id", versionId},
                                     {"offset", QString::number(offset)}});
        require(page.value("entries").isArray(), "Invalid manifest page");
        for (const auto& value : page.value("entries").toArray())
        {
            const auto entry = value.toObject();
            writeAll(output, verifier.append(entry));
        }
        const auto next = number(page, "offset");
        if (page.value("done").toBool())
        {
            break;
        }
        require(next > offset, "Manifest cursor did not advance");
        offset = next;
    }
    verifier.finish();
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
        QCryptographicHash storedHash(QCryptographicHash::Sha256);
        qint64 offset = 0;
        qint64 originalOffset = 0;
        const auto size = number(entry, "size");
        const auto compression = compressionAlgorithm(entry);
        const auto storedSize =
            compression == "none" ? size : number(entry, "stored_size");
        QByteArray pending;
        const auto writeDecoded = [&](const QByteArray& data)
        {
            require(data.size() <= size - originalOffset,
                    "Decompressed content exceeds declared size");
            writeAll(file, data);
            hash.addData(data);
            originalOffset += data.size();
            status.insert("bytes", number(status, "bytes") + data.size());
            progress(status);
        };
        while (true)
        {
            checkOperationCancelled();
            const auto reply =
                channel.call("download", {{"version_id", versionId},
                                          {"index", entry.value("index")},
                                          {"offset", QString::number(offset)}});
            const auto decoded = QByteArray::fromBase64Encoding(
                reply.value("data").toString().toLatin1(),
                QByteArray::AbortOnBase64DecodingErrors);
            require(bool(decoded) && decoded.decoded.size() <= kChunkBytes &&
                        number(reply, "offset") == offset &&
                        reply.value("done").isBool(),
                    "Invalid download block");
            const auto& data = decoded.decoded;
            require(data.size() <= storedSize - offset,
                    "Backup content exceeds declared size");
            storedHash.addData(data);
            offset += data.size();
            if (compression == "none")
            {
                writeDecoded(data);
            }
            else
            {
                pending.append(data);
                require(pending.size() <=
                            kChunkBytes * 2 + kCompressionHeaderBytes,
                        "Compressed block buffer exceeds supported size");
                while (pending.size() >= kCompressionHeaderBytes)
                {
                    checkOperationCancelled();
                    const auto frameSize =
                        compressionBlockSize(pending, compression);
                    if (pending.size() < frameSize)
                    {
                        break;
                    }
                    const auto block =
                        decompressBlock(pending.first(frameSize), compression);
                    writeDecoded(block);
                    pending.remove(0, frameSize);
                }
            }
            if (reply.value("done").toBool())
            {
                break;
            }
            require(!data.isEmpty(), "Empty non-final download block");
        }
        require(offset == storedSize && pending.isEmpty() &&
                    originalOffset == size &&
                    QString::fromLatin1(hash.result().toHex()) ==
                        text(entry, "sha256"),
                "Restored file checksum mismatch");
        require(compression == "none" ||
                    QString::fromLatin1(storedHash.result().toHex()) ==
                        text(entry, "stored_sha256"),
                "Compressed file checksum mismatch");
        // 先刷新 Qt 写缓冲，避免后续写入覆盖已恢复的修改时间。
        require(file.flush(),
                "Cannot flush restored content: " + text(entry, "path"));
        restoreDescriptorMetadata(descriptor.get(), entry, status);
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

QJsonObject restoreTree(Channel& channel, const QString& versionId,
                        QTemporaryFile& manifest, int root,
                        RestoreJournal& journal, const QString& destination,
                        const Progress& progress)
{
    QJsonObject status{{"stage", "restore"},
                       {"files", 0},
                       {"directories", 0},
                       {"bytes", 0},
                       {"symlinks", 0},
                       {"hardlinks", 0},
                       {"fifos", 0},
                       {"character_devices", 0},
                       {"block_devices", 0},
                       {"sockets", 0},
                       {"metadata_warning_count", 0},
                       {"warnings", QJsonArray{}},
                       {"destination", destination}};
    QHash<QString, struct stat> groups;
    QHash<QString, QString> members;
    QVector<QJsonObject> directories;
    while (!manifest.atEnd())
    {
        checkOperationCancelled();
        channel.keepAlive();
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
            const char* counter = "files";
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
                const auto result = restoreNode(directory.get(), leaf, entry);
                require(::fsync(directory.get()) == 0,
                        "Cannot persist restored " + type);
                counter = result.counter;
                if (result.metadata == MetadataTiming::Deferred)
                {
                    directories.append(entry);
                }
                else if (result.metadata == MetadataTiming::Immediate)
                {
                    restorePathMetadata(directory.get(), leaf, entry, status);
                }
            }
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
    // 在收紧目录的属主/遍历权限前验证硬链接，之后不再按路径访问后代。
    for (auto member = members.begin(); member != members.end(); ++member)
    {
        checkOperationCancelled();
        require(sameFileState(groups.value(member.value()),
                              statBelow(root, member.key())),
                "Restored hard link member changed: " + member.key() +
                    "; partial restore at " + destination);
    }
    for (auto item = directories.crbegin(); item != directories.crend(); ++item)
    {
        const auto path = text(*item, "path");
        const auto slash = path.lastIndexOf('/');
        Descriptor parent(slash < 0 ? ::dup(root)
                                    : openBelow(root, path.left(slash),
                                                O_RDONLY | O_DIRECTORY));
        restorePathMetadata(parent.get(), path.mid(slash + 1).toUtf8(), *item,
                            status);
        require(::fsync(parent.get()) == 0,
                "Cannot persist restored directory metadata: " + path);
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
    require(::unlink(QFile::encodeName(manifest.fileName()).constData()) == 0,
            "Cannot unlink temporary manifest");
    manifest.setAutoRemove(false);
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
        restoreDescriptorMetadata(root.get(), metadata, status);
    }
    require(::fsync(root.get()) == 0 && ::fsync(parent.get()) == 0,
            "Cannot persist restored tree");
    auto warnings = status.value("warnings").toArray();
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
    // 包含根目录失败和版本原有警告；完整明细交给现有历史存储分页。
    status.insert("warnings", warnings);
    status.insert("warning_count", warnings.size());
    return status;
}
} // namespace backup::agent
