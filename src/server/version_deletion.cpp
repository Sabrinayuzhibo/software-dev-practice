#include "backup/core/io.h"
#include "backup/server/repository.h"

#include <QFileInfo>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace backup::server
{
using namespace core;
namespace
{
constexpr int kDeleteBatchSize = 128;

QString descriptorPath(int descriptor)
{
    return "/proc/self/fd/" + QString::number(descriptor);
}

bool directoryExists(int parent, const QByteArray& name)
{
    struct stat info
    {
    };
    if (::fstatat(parent, name.constData(), &info, AT_SYMLINK_NOFOLLOW) == 0)
    {
        require(S_ISDIR(info.st_mode),
                "Invalid version directory: " + QString::fromUtf8(name));
        return true;
    }
    require(errno == ENOENT, "Cannot inspect version directory: " +
                                 QString::fromLocal8Bit(strerror(errno)));
    return false;
}

bool reclaimBatch(int directory)
{
    Descriptor copy(::dup(directory));
    const auto closeDirectory = [](DIR* directory) { ::closedir(directory); };
    std::unique_ptr<DIR, decltype(closeDirectory)> entries(
        ::fdopendir(copy.get()), closeDirectory);
    require(bool(entries), "Cannot enumerate deleted version");
    copy.release();
    QList<QByteArray> names;
    bool complete = false;
    while (names.size() < kDeleteBatchSize)
    {
        errno = 0;
        const auto* entry = ::readdir(entries.get());
        if (!entry)
        {
            require(errno == 0, "Cannot enumerate deleted version");
            complete = true;
            break;
        }
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..")
        {
            continue;
        }
        struct stat info
        {
        };
        require(
            ::fstatat(directory, name.constData(), &info,
                      AT_SYMLINK_NOFOLLOW) == 0 &&
                !S_ISDIR(info.st_mode),
            "Unexpected directory or unreadable entry in deleted version: " +
                QString::fromUtf8(name));
        names.append(name);
    }
    // Repository versions are flat. Unlink entries through the pinned
    // directory, never recurse into a directory or follow a symbolic link.
    for (const auto& name : names)
    {
        const auto result = ::unlinkat(directory, name.constData(), 0);
        require(result == 0, "Cannot reclaim version entry " +
                                 QString::fromUtf8(name) + ": " +
                                 QString::fromLocal8Bit(strerror(errno)));
    }
    require(::fsync(directory) == 0, "Cannot persist reclaimed version data");
    return complete;
}
} // namespace

void Repository::acquireVersion(const QString& id)
{
    version(id);
    ++readers_[id];
}

void Repository::releaseVersion(const QString& id)
{
    auto item = readers_.find(id);
    if (item != readers_.end() && --item.value() == 0)
    {
        readers_.erase(item);
    }
}

QJsonObject Repository::deleteVersion(const QString& id)
{
    require(validId(id), "Invalid version identifier");
    require(readers_.value(id) == 0,
            "Version is in use; retry after reading ends");
    require(activeId_ != id, "Version is still being uploaded");
    Descriptor root(::open(QFile::encodeName(root_).constData(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    Descriptor versions(
        openBelow(root.get(), "storage/versions", O_RDONLY | O_DIRECTORY));
    Descriptor deleting(
        openBelow(root.get(), "storage/deleting", O_RDONLY | O_DIRECTORY));
    Descriptor receipts(
        openBelow(root.get(), "database/deletions", O_RDONLY | O_DIRECTORY));
    const auto name = id.toUtf8();
    const auto receiptPath =
        descriptorPath(receipts.get()) + '/' + id + ".json";
    require(!QFileInfo(receiptPath).isSymLink(), "Invalid deletion receipt");
    const bool visible = directoryExists(versions.get(), name);
    bool hidden = directoryExists(deleting.get(), name);
    require(!(visible && hidden), "Conflicting version deletion directories");
    QJsonObject receipt;
    if (QFileInfo::exists(receiptPath))
    {
        receipt = readObject(receiptPath);
        require(receipt.value("format").toInt() == 1 &&
                    receipt.value("version_id") == id &&
                    receipt.value("version").toObject().value("id") == id &&
                    receipt.value("done").isBool(),
                "Invalid deletion receipt");
        require(!receipt.value("done").toBool() || (!visible && !hidden),
                "Deleted version has reappeared");
    }
    else
    {
        require(visible && !hidden, "Version not found: " + id);
        receipt = {{"format", 1},
                   {"version_id", id},
                   {"version", version(id)},
                   {"started_at", now()},
                   {"done", false}};
        // Persist intent before hiding anything. A retry can finish either
        // side of the rename, including a lost reply or a process restart.
        writeObject(receiptPath, receipt);
    }
    if (visible)
    {
        const auto renamed =
            ::renameat2(versions.get(), name.constData(), deleting.get(),
                        name.constData(), RENAME_NOREPLACE);
        require(renamed == 0, "Cannot hide version for deletion: " +
                                  QString::fromLocal8Bit(strerror(errno)));
        hidden = true;
    }
    require(::fsync(versions.get()) == 0 && ::fsync(deleting.get()) == 0,
            "Cannot persist version deletion");
    if (hidden)
    {
        Descriptor directory(
            openBelow(deleting.get(), id, O_RDONLY | O_DIRECTORY));
        if (!reclaimBatch(directory.get()))
        {
            return receipt;
        }
        require(::unlinkat(deleting.get(), name.constData(), AT_REMOVEDIR) ==
                        0 &&
                    ::fsync(deleting.get()) == 0,
                "Cannot finish reclaiming version directory");
    }
    if (!receipt.value("done").toBool())
    {
        receipt.insert("done", true);
        receipt.insert("finished_at", now());
        writeObject(receiptPath, receipt);
    }
    return receipt;
}
} // namespace backup::server
