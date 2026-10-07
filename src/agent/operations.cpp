#include "backup/agent/operations.h"
#include "backup/agent/channel.h"
#include "backup/agent/restore_journal.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryFile>
#include <QThread>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
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

bool unchanged(const struct stat& before, const struct stat& after)
{
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_size == after.st_size &&
           before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

QString readFile(int root, const QString& path, Channel* channel,
                 QJsonObject& status, const Progress& progress)
{
    Descriptor descriptor(openBelow(root, path, O_RDONLY | O_NONBLOCK));
    struct stat before
    {
    };
    require(::fstat(descriptor.get(), &before) == 0 && S_ISREG(before.st_mode),
            "Source is no longer a regular file: " + path);
    QFile file;
    require(file.open(descriptor.get(), QIODevice::ReadOnly),
            "Cannot read: " + path);
    if (channel)
    {
        channel->call("entry", {{"path", path},
                                {"type", "file"},
                                {"size", QString::number(before.st_size)}});
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 offset = 0;
    while (true)
    {
        checkCancelled();
        const auto chunk = file.read(kChunkBytes);
        require(file.error() == QFileDevice::NoError, "Cannot read: " + path);
        if (chunk.isEmpty())
        {
            break;
        }
        require(chunk.size() <= before.st_size - offset,
                "Source changed during read: " + path);
        hash.addData(chunk);
        if (channel)
        {
            channel->call("chunk",
                          {{"offset", QString::number(offset)},
                           {"data", QString::fromLatin1(chunk.toBase64())}});
        }
        offset += chunk.size();
        require(offset <= before.st_size,
                "Source changed during read: " + path);
        const auto processed = number(status, "bytes");
        require(processed <= std::numeric_limits<qint64>::max() - chunk.size(),
                "Source too large");
        status.insert("bytes", processed + chunk.size());
        status.insert("path", path);
        progress(status);
    }
    struct stat after
    {
    };
    Descriptor current(openBelow(root, path, O_RDONLY | O_NONBLOCK));
    require(::fstat(current.get(), &after) == 0 && unchanged(before, after) &&
                offset == before.st_size,
            "Source changed during read: " + path);
    const QString digest = QString::fromLatin1(hash.result().toHex());
    if (channel)
    {
        channel->call("finish_file", {{"sha256", digest}});
    }
    return digest;
}

void addSample(QJsonArray& sample, const QJsonObject& item)
{
    const auto fits = [&]
    {
        return sample.size() < kPageSize &&
               QJsonDocument(sample).toJson(QJsonDocument::Compact).size() +
                       detailLine(item).size() <
                   kDetailPageBytes;
    };
    // Keep backed-up entries visible when skipped entries fill the preview.
    if (item.value("type") == "file" || item.value("type") == "directory")
    {
        for (qsizetype index = sample.size(); index > 0 && !fits(); --index)
        {
            const auto type = sample[index - 1].toObject().value("type");
            if (type == "symlink" || type == "special")
            {
                sample.removeAt(index - 1);
            }
        }
    }
    if (fits())
    {
        sample.append(item);
    }
}

QJsonObject scanTree(const QString& source, const Progress& progress)
{
    const auto rootPath = canonicalPath(source);
    Descriptor root(::open(QFile::encodeName(rootPath).constData(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    QJsonObject status{{"stage", "scan"}, {"root", rootPath},
                       {"files", 0},      {"directories", 0},
                       {"bytes", 0},      {"complete", true},
                       {"error_count", 0}};
    QJsonArray sample;
    QJsonArray warnings;
    const auto problem = [&](const QString& path, const QString& reason)
    {
        warnings.append(QJsonObject{{"path", path.isEmpty() ? "." : path},
                                    {"reason", reason},
                                    {"severity", "error"}});
        status.insert("complete", false);
        status.insert("error_count", number(status, "error_count") + 1);
    };
    std::function<void(const QString&)> visit = [&](const QString& relative)
    {
        checkCancelled();
        try
        {
            Descriptor directory(
                relative.isEmpty()
                    ? ::dup(root.get())
                    : openBelow(root.get(), relative, O_RDONLY | O_DIRECTORY));
            struct stat before
            {
            };
            require(::fstat(directory.get(), &before) == 0,
                    "Cannot inspect directory");
            const auto native = std::filesystem::path(
                QFile::encodeName(rootPath + '/' + relative).toStdString());
            std::error_code error;
            auto iterator = std::filesystem::directory_iterator(native, error);
            require(!error, QString::fromStdString(error.message()));
            while (iterator != std::filesystem::directory_iterator())
            {
                checkCancelled();
                const auto entry = *iterator;
                const auto bytes =
                    entry.path()
                        .lexically_relative(std::filesystem::path(
                            QFile::encodeName(rootPath).toStdString()))
                        .native();
                const auto path = QString::fromUtf8(bytes.data(), bytes.size());
                try
                {
                    require(path.toUtf8().toStdString() == bytes &&
                                validPath(path),
                            "Unsupported filename encoding or path");
                    const auto type = entry.symlink_status().type();
                    status.insert("path", path);
                    if (type == std::filesystem::file_type::directory)
                    {
                        status.insert("directories",
                                      number(status, "directories") + 1);
                        addSample(sample,
                                  {{"path", path}, {"type", "directory"}});
                        visit(path);
                    }
                    else if (type == std::filesystem::file_type::regular)
                    {
                        const auto previous = number(status, "bytes");
                        QString hash;
                        try
                        {
                            hash = readFile(root.get(), path, nullptr, status,
                                            progress);
                        }
                        catch (...)
                        {
                            status.insert("bytes", previous);
                            addSample(sample,
                                      {{"path", path}, {"type", "file"}});
                            throw;
                        }
                        status.insert("files", number(status, "files") + 1);
                        addSample(sample,
                                  {{"path", path},
                                   {"type", "file"},
                                   {"size", number(status, "bytes") - previous},
                                   {"sha256", hash}});
                    }
                    else
                    {
                        const auto label =
                            type == std::filesystem::file_type::symlink
                                ? "symlink"
                                : "special";
                        addSample(sample, {{"path", path}, {"type", label}});
                        warnings.append(QJsonObject{
                            {"path", path},
                            {"reason",
                             "Special file skipped in basic backup"}});
                    }
                    progress(status);
                }
                catch (const std::exception& failure)
                {
                    checkCancelled();
                    problem(path, QString::fromUtf8(failure.what()));
                }
                iterator.increment(error);
                require(!error, QString::fromStdString(error.message()));
            }
            struct stat after
            {
            };
            require(::fstat(directory.get(), &after) == 0 &&
                        unchanged(before, after),
                    "Source directory changed during traversal");
        }
        catch (const std::exception& failure)
        {
            checkCancelled();
            problem(relative, QString::fromUtf8(failure.what()));
        }
    };
    visit({});
    status.insert("sample", sample);
    status.insert("warnings", warnings);
    status.insert("hashed_files", status.value("files"));
    status.insert("unreadable_files", status.value("error_count"));
    return status;
}

QJsonObject traverse(const QString& source, Channel* channel,
                     const Progress& progress)
{
    const QString rootPath = canonicalPath(source);
    Descriptor root(::open(QFile::encodeName(rootPath).constData(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat rootBefore
    {
    };
    require(::fstat(root.get(), &rootBefore) == 0, "Cannot inspect source");
    QJsonObject status{{"stage", channel ? "upload" : "scan"},
                       {"files", 0},
                       {"directories", 0},
                       {"bytes", 0},
                       {"root", rootPath}};
    QJsonArray sample;
    QJsonArray warnings;
    std::vector<std::pair<QString, struct stat>> directoryStates;
    const auto nativeRoot =
        std::filesystem::path(QFile::encodeName(rootPath).toStdString());
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(nativeRoot))
    {
        checkCancelled();
        const auto native =
            entry.path().lexically_relative(nativeRoot).native();
        const QString path = QString::fromUtf8(
            native.data(), static_cast<qsizetype>(native.size()));
        require(path.toUtf8().toStdString() == native && validPath(path),
                "Unsupported filename encoding or path");
        const auto type = entry.symlink_status().type();
        try
        {
            if (type == std::filesystem::file_type::directory)
            {
                Descriptor directory(
                    openBelow(root.get(), path, O_RDONLY | O_DIRECTORY));
                struct stat info
                {
                };
                require(::fstat(directory.get(), &info) == 0,
                        "Cannot inspect directory");
                directoryStates.emplace_back(path, info);
                status.insert("directories", number(status, "directories") + 1);
                if (channel)
                {
                    channel->call("entry",
                                  {{"path", path}, {"type", "directory"}});
                }
            }
            else if (type == std::filesystem::file_type::regular)
            {
                const auto previous = number(status, "bytes");
                const QString digest =
                    readFile(root.get(), path, channel, status, progress);
                status.insert("files", number(status, "files") + 1);
                addSample(sample, QJsonObject{{"path", path},
                                              {"type", "file"},
                                              {"size", number(status, "bytes") -
                                                           previous},
                                              {"sha256", digest}});
            }
            else
            {
                warnings.append(QJsonObject{
                    {"path", path},
                    {"reason", "Special file skipped in basic backup"}});
            }
            status.insert("path", path);
            progress(status);
        }
        catch (const std::exception& error)
        {
            throw Error(path + ": " + QString::fromUtf8(error.what()));
        }
    }
    // Directory timestamps detect additions/removals while walking the tree.
    for (const auto& [path, before] : directoryStates)
    {
        Descriptor directory(
            openBelow(root.get(), path, O_RDONLY | O_DIRECTORY));
        struct stat after
        {
        };
        require(::fstat(directory.get(), &after) == 0 &&
                    unchanged(before, after),
                "Source directory changed: " + path);
    }
    struct stat rootAfter
    {
    };
    require(::fstat(root.get(), &rootAfter) == 0 &&
                unchanged(rootBefore, rootAfter),
            "Source directory changed during traversal");
    status.insert("sample", sample);
    status.insert("warnings", warnings);
    status.insert("hashed_files", status.value("files"));
    status.insert("unreadable_files", 0);
    status.insert("complete", true);
    return status;
}

void fetchManifest(Channel& channel, const QString& versionId,
                   const QJsonObject& summary, QTemporaryFile& output)
{
    QSet<QString> paths;
    QSet<QString> directories;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 offset = 0;
    qint64 count = 0;
    qint64 files = 0;
    qint64 bytes = 0;
    while (true)
    {
        checkCancelled();
        const auto page =
            channel.call("entries", {{"version_id", versionId},
                                     {"offset", QString::number(offset)}});
        for (const auto& value : page.value("entries").toArray())
        {
            const auto entry = value.toObject();
            const auto path = text(entry, "path");
            require(validPath(path) && !paths.contains(path),
                    "Unsafe or duplicate manifest path: " + path);
            const auto slash = path.lastIndexOf('/');
            require(slash < 0 || directories.contains(path.left(slash)),
                    "Invalid manifest parent");
            require(number(entry, "index") == count, "Invalid manifest index");
            paths.insert(path);
            const auto type = text(entry, "type");
            if (type == "directory")
            {
                directories.insert(path);
            }
            else
            {
                require(type == "file", "Unsupported manifest type");
                const auto digest = text(entry, "sha256").toLatin1();
                require(digest.size() == 64 &&
                            QByteArray::fromHex(digest).toHex() == digest,
                        "Invalid file checksum");
                const auto size = number(entry, "size");
                require(size <= std::numeric_limits<qint64>::max() - bytes,
                        "Manifest size overflow");
                bytes += size;
                ++files;
            }
            const auto line =
                QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
            writeAll(output, line);
            hash.addData(line);
            ++count;
        }
        const auto next = number(page, "offset");
        if (page.value("done").toBool())
        {
            break;
        }
        require(next > offset, "Manifest cursor did not advance");
        offset = next;
    }
    require(count == number(summary, "entries") &&
                files == number(summary, "files") &&
                directories.size() == number(summary, "directories") &&
                bytes == number(summary, "bytes") &&
                QString::fromLatin1(hash.result().toHex()) ==
                    text(summary, "manifest_sha256"),
            "Version manifest checksum or statistics mismatch");
    require(output.seek(0), "Cannot read verified manifest");
}

void restoreFile(Channel& channel, const QString& versionId, int parent,
                 const QString& leaf, const QJsonObject& entry,
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
        syncFile(file);
        // linkat publishes the verified file without overwriting a concurrent
        // insertion.
        require(::linkat(parent, temporary.constData(), parent,
                         leaf.toUtf8().constData(), 0) == 0,
                "Cannot publish restored file (destination may already exist)");
        require(::unlinkat(parent, temporary.constData(), 0) == 0 &&
                    ::fsync(parent) == 0,
                "Cannot persist restored file");
    }
    catch (...)
    {
        ::unlinkat(parent, temporary.constData(), 0);
        throw;
    }
}
} // namespace

QJsonObject scan(const QString& source, const Progress& progress)
{
    try
    {
        return scanTree(source, progress);
    }
    catch (const std::exception& failure)
    {
        checkCancelled();
        return {{"root", source},
                {"files", 0},
                {"directories", 0},
                {"bytes", 0},
                {"complete", false},
                {"error_count", 1},
                {"sample", QJsonArray{}},
                {"warnings", QJsonArray{QJsonObject{
                                 {"path", "."},
                                 {"reason", QString::fromUtf8(failure.what())},
                                 {"severity", "error"}}}}};
    }
}

QJsonObject backup(const QJsonObject& task, const QJsonObject& target,
                   const QString& operationId, const Progress& progress)
{
    Channel channel(target);
    channel.authenticate();
    const auto source = canonicalPath(text(task, "path"));
    require(
        !overlaps(source, canonicalPath(text(channel.health(), "data_root"))),
        "Source overlaps repository");
    channel.call("begin", {{"operation_id", operationId},
                           {"task_id", text(task, "id")},
                           {"source", source}});
    auto result = traverse(source, &channel, progress);
    const auto warnings = result.value("warnings").toArray();
    qint64 warningOffset = 0;
    while (warningOffset < warnings.size())
    {
        const auto page = detailPage(warnings, warningOffset);
        const auto items = page.value("items").toArray();
        channel.call("warnings",
                     {{"offset", warningOffset}, {"warnings", items}});
        warningOffset += items.size();
    }
    progress({{"stage", "commit"},
              {"files", result.value("files")},
              {"bytes", result.value("bytes")}});
    QJsonObject version;
    try
    {
        version = channel.call("commit", {{"operation_id", operationId}});
    }
    catch (const std::exception&)
    {
        // A lost commit acknowledgement is resolved by the same operation ID.
        progress({{"stage", "confirm"}});
        try
        {
            Channel confirmation(target);
            confirmation.authenticate();
            version =
                confirmation.call("version", {{"version_id", operationId}});
        }
        catch (const std::exception&)
        {
            throw Error("COMMIT_UNCERTAIN: result pending confirmation; query "
                        "this operation before starting another backup");
        }
    }
    result.insert("version", version);
    return result;
}

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
    require(!overlaps(destination,
                      canonicalPath(text(channel.health(), "data_root"))) &&
                !overlaps(destination, stateDirectory),
            "Restore destination overlaps application data");
    for (const auto& value : tasks)
    {
        require(!overlaps(destination,
                          canonicalPath(text(value.toObject(), "path"))),
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
    QTemporaryFile manifest;
    require(manifest.open(), "Cannot create temporary manifest");
    progress({{"stage", "verify"}, {"files", 0}, {"bytes", 0}});
    fetchManifest(channel, versionId, summary, manifest);
    RestoreJournal journal(stateDirectory, text(args, "operation_id"));
    progress({{"restore_journal", true}});

    // The existing parent must resolve without symlinks; mkdir only creates the
    // leaf.
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
    QJsonObject status{{"stage", "restore"},
                       {"files", 0},
                       {"directories", 0},
                       {"bytes", 0},
                       {"destination", destination}};
    while (!manifest.atEnd())
    {
        checkCancelled();
        const auto entry = parseObject(manifest.readLine(64 * 1024));
        const QString path = text(entry, "path");
        const auto slash = path.lastIndexOf('/');
        Descriptor directory(slash < 0 ? ::dup(root.get())
                                       : openBelow(root.get(), path.left(slash),
                                                   O_RDONLY | O_DIRECTORY));
        const QString filename = path.mid(slash + 1);
        status.insert("path", path);
        try
        {
            auto intent = entry;
            const auto temporary = (".backup-" + newId()).toUtf8();
            if (entry.value("type") == "file")
            {
                intent.insert("temporary_path",
                              path.left(slash + 1) +
                                  QString::fromUtf8(temporary));
            }
            journal.record(intent, "pending");
            if (entry.value("type") == "directory")
            {
                require(::mkdirat(directory.get(),
                                  filename.toUtf8().constData(), 0700) == 0,
                        "Cannot create restore directory");
                require(::fsync(directory.get()) == 0,
                        "Cannot persist directory");
                status.insert("directories", number(status, "directories") + 1);
            }
            else
            {
                restoreFile(channel, versionId, directory.get(), filename,
                            entry, temporary, status, progress);
                status.insert("files", number(status, "files") + 1);
            }
            journal.record(entry, "written");
            progress(status);
        }
        catch (const std::exception& error)
        {
            throw Error(path + ": " + QString::fromUtf8(error.what()) +
                        "; partial restore at " + destination);
        }
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
    status.insert("warnings", warnings);
    return status;
}
} // namespace backup::agent
