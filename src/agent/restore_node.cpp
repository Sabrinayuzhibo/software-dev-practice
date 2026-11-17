#include "backup/agent/restore_node.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"

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
void restoreSocket(int parent, const QByteArray& leaf)
{
    Descriptor socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    const auto temporary = (".backup-" + newId()).toUtf8();
    const auto pinned =
        "/proc/self/fd/" + QByteArray::number(parent) + '/' + temporary;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(pinned.size() < static_cast<qsizetype>(sizeof(address.sun_path)),
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
        const auto protectedNode = ::fchmodat(parent, temporary.constData(),
                                              0600, AT_SYMLINK_NOFOLLOW);
        const auto protectError = errno;
        require(protectedNode == 0,
                "Cannot protect restored socket: " +
                    QString::fromLocal8Bit(strerror(protectError)));
        const auto published =
            ::renameat2(parent, temporary.constData(), parent, leaf.constData(),
                        RENAME_NOREPLACE);
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

void requireCreated(int result, const QString& type)
{
    const auto error = errno;
    require(result == 0, "Cannot restore " + type + ": " +
                             QString::fromLocal8Bit(strerror(error)));
}

RestoredNode restoreDevice(int parent, const QByteArray& leaf,
                           const QJsonObject& entry)
{
    const auto type = text(entry, "type");
    const bool character = type == "character_device";
    const auto identifier =
        makedev(static_cast<unsigned int>(number(entry, "device_major")),
                static_cast<unsigned int>(number(entry, "device_minor")));
    const auto mode = character ? S_IFCHR : S_IFBLK;
    requireCreated(::mknodat(parent, leaf.constData(), mode | 0600, identifier),
                   type);
    struct stat restored
    {
    };
    require(
        ::fstatat(parent, leaf.constData(), &restored, AT_SYMLINK_NOFOLLOW) ==
                0 &&
            (character ? S_ISCHR(restored.st_mode) : S_ISBLK(restored.st_mode)),
        "Restored node type changed: " + text(entry, "path"));
    require(major(restored.st_rdev) == number(entry, "device_major") &&
                minor(restored.st_rdev) == number(entry, "device_minor"),
            "Restored device number changed: " + text(entry, "path"));
    return {character ? "character_devices" : "block_devices",
            MetadataTiming::None};
}
} // 匿名命名空间

RestoredNode restoreNode(int parent, const QByteArray& leaf,
                         const QJsonObject& entry)
{
    const auto type = text(entry, "type");
    if (type == "directory")
    {
        requireCreated(::mkdirat(parent, leaf.constData(), 0700), type);
        return {"directories", MetadataTiming::Deferred};
    }
    if (type == "symlink")
    {
        const auto target = symlinkTarget(entry);
        requireCreated(
            ::symlinkat(target.constData(), parent, leaf.constData()), type);
        return {"symlinks", MetadataTiming::Immediate};
    }
    if (type == "fifo")
    {
        requireCreated(::mkfifoat(parent, leaf.constData(), 0600), type);
        return {"fifos", MetadataTiming::Immediate};
    }
    if (type == "character_device" || type == "block_device")
    {
        return restoreDevice(parent, leaf, entry);
    }
    if (type == "socket")
    {
        restoreSocket(parent, leaf);
        struct stat restored
        {
        };
        require(::fstatat(parent, leaf.constData(), &restored,
                          AT_SYMLINK_NOFOLLOW) == 0 &&
                    S_ISSOCK(restored.st_mode),
                "Restored node type changed: " + text(entry, "path"));
        return {"sockets", MetadataTiming::None};
    }
    throw Error("Unsupported restore node type: " + type);
}
} // 命名空间 backup::agent
