#include "archive_io.h"
#include "backup/agent/worker_support.h"
#include "backup/core/io.h"

#include <QDir>
#include <QFileInfo>
#include <fcntl.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
QString archivePath(const QString& supplied)
{
    require(QDir::isAbsolutePath(supplied) && supplied.size() <= 4096 &&
                !supplied.contains(QChar(0)),
            "Invalid archive path");
    require(!QFileInfo(supplied).isSymLink(),
            "Archive file cannot be a symbolic link");
    return canonicalPath(supplied);
}

void snapshotArchive(const QString& path, QTemporaryFile& snapshot,
                     const Progress& progress)
{
    Descriptor filesystem(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Descriptor descriptor(
        openBelow(filesystem.get(), path.mid(1), O_RDONLY | O_NONBLOCK));
    struct stat before
    {
    };
    require(::fstat(descriptor.get(), &before) == 0 && S_ISREG(before.st_mode),
            "Archive must be a regular file");
    QFile source;
    require(source.open(descriptor.get(), QIODevice::ReadOnly) &&
                snapshot.open(),
            "Cannot open archive or create private snapshot");
    // 快照只由 fd 持有，即使进程被强制结束也不会遗留大文件。
    require(::unlink(QFile::encodeName(snapshot.fileName()).constData()) == 0,
            "Cannot unlink private archive snapshot");
    snapshot.setAutoRemove(false);
    qint64 offset = 0;
    while (offset < before.st_size)
    {
        checkOperationCancelled();
        const auto bytes =
            source.read(qMin(kChunkBytes, before.st_size - offset));
        require(source.error() == QFileDevice::NoError && !bytes.isEmpty(),
                "Cannot read archive snapshot");
        writeAll(snapshot, bytes);
        offset += bytes.size();
        progress(
            {{"stage", "archive_verify"}, {"bytes", offset}, {"files", 0}});
    }
    struct stat after
    {
    };
    require(source.read(1).isEmpty() &&
                source.error() == QFileDevice::NoError &&
                ::fstat(descriptor.get(), &after) == 0 &&
                sameFileState(before, after) &&
                sameFileState(before, statBelow(filesystem.get(), path.mid(1))),
            "Archive changed while taking snapshot");
    require(snapshot.flush() && snapshot.seek(0),
            "Cannot read private archive snapshot");
}
} // 命名空间 backup::agent
