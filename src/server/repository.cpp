#include "backup/server/repository.h"
#include "backup/core/io.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <limits>

namespace backup::server
{
using namespace core;

void Repository::open(const QString& root)
{
    require(QDir(root).isAbsolute(), "--data-dir must be an absolute path");
    makeDirectory(root);
    root_ = canonicalPath(root);
    for (const auto& child :
         {"database", "storage", "storage/versions", "storage/staging"})
    {
        const QString path = root_ + '/' + child;
        require(!QFileInfo(path).isSymLink(),
                "Repository contains a link: " + path);
        makeDirectory(path);
    }
    lock_ = std::make_unique<QLockFile>(root_ + "/database/server.lock");
    require(lock_->tryLock(), "Repository is already in use");
    const QString tokenPath = root_ + "/database/access.json";
    if (!QFileInfo::exists(tokenPath))
    {
        writeObject(tokenPath, {{"token", newId() + newId()}});
        require(QFile::setPermissions(tokenPath,
                                      QFile::ReadOwner | QFile::WriteOwner),
                "Cannot protect local access token");
    }
    token_ = text(readObject(tokenPath), "token");
    // This probe also rejects unwritable storage before listening.
    const QString probe = root_ + "/storage/.probe-" + newId();
    writeObject(probe, {{"format", 1}});
    require(QFile::remove(probe), "Cannot remove storage probe");
}

QString Repository::root() const
{
    return root_;
}
QString Repository::token() const
{
    return token_;
}

QString Repository::versionPath(const QString& id) const
{
    require(validId(id), "Invalid version identifier");
    return root_ + "/storage/versions/" + id;
}

QJsonObject Repository::version(const QString& id) const
{
    const auto result = readObject(versionPath(id) + "/summary.json");
    require(result.value("format").toInt() == 1 && result.value("id") == id,
            "Unsupported or corrupt version");
    return result;
}

QJsonObject Repository::list(const QJsonObject& request) const
{
    QList<QJsonObject> versions;
    const auto names = QDir(root_ + "/storage/versions")
                           .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& name : names)
    {
        auto value = version(name);
        if (request.value("task_id").toString().isEmpty() ||
            value.value("task_id") == request.value("task_id"))
        {
            versions.append(value);
        }
    }
    std::sort(versions.begin(), versions.end(),
              [](const auto& left, const auto& right)
              {
                  return left.value("completed_at").toString() >
                         right.value("completed_at").toString();
              });
    const qint64 offset =
        request.contains("offset") ? number(request, "offset") : 0;
    QJsonArray page;
    for (qint64 index = offset;
         index < versions.size() && page.size() < kPageSize; ++index)
    {
        page.append(versions[index]);
    }
    return {{"versions", page}, {"total", versions.size()}};
}

QJsonObject Repository::entries(const QJsonObject& request) const
{
    const auto id = text(request, "version_id");
    version(id);
    QFile file(versionPath(id) + "/entries.jsonl");
    require(file.open(QIODevice::ReadOnly), "Missing version manifest");
    const qint64 offset = number(request, "offset");
    require(offset <= file.size() && file.seek(offset),
            "Invalid manifest offset");
    QJsonArray page;
    while (!file.atEnd() && page.size() < kPageSize)
    {
        const auto line = file.readLine(64 * 1024);
        require(line.endsWith('\n'), "Corrupt manifest entry");
        page.append(parseObject(line));
    }
    return {{"entries", page},
            {"offset", QString::number(file.pos())},
            {"done", file.atEnd()}};
}

QJsonObject Repository::download(const QJsonObject& request) const
{
    const QString id = text(request, "version_id");
    const auto summary = version(id);
    const qint64 index = number(request, "index");
    require(index < number(summary, "entries"), "Invalid content index");
    QFile file(versionPath(id) + '/' + QString::number(index) + ".data");
    require(file.open(QIODevice::ReadOnly), "Missing backup content");
    const qint64 offset = number(request, "offset");
    require(offset <= file.size() && file.seek(offset),
            "Invalid content offset");
    const auto data = file.read(kChunkBytes);
    require(file.error() == QFileDevice::NoError, "Cannot read backup content");
    return {{"data", QString::fromLatin1(data.toBase64())},
            {"offset", QString::number(offset)},
            {"done", file.atEnd()}};
}

QJsonObject Repository::begin(const QJsonObject& request)
{
    const QString id = text(request, "operation_id");
    const QString finalPath = versionPath(id);
    require(activeId_.isEmpty(), "Repository is busy");
    require(!QFileInfo::exists(finalPath), "Operation already committed");
    const QString source = canonicalPath(text(request, "source"));
    require(!overlaps(source, root_), "Source overlaps repository");
    require(validId(text(request, "task_id")), "Invalid task identifier");
    stagingPath_ = root_ + "/storage/staging/" + id;
    require(!QFileInfo::exists(stagingPath_),
            "Operation already exists; use a new operation");
    makeDirectory(stagingPath_);
    summary_ = {{"format", 1},
                {"id", id},
                {"task_id", request.value("task_id")},
                {"source", source},
                {"started_at", now()},
                {"rules", QJsonObject{}}};
    files_ = 0;
    directoriesCount_ = 0;
    bytes_ = 0;
    entryCount_ = 0;
    paths_.clear();
    directories_.clear();
    fileHash_.reset();
    manifestHash_.reset();
    manifest_.setFileName(stagingPath_ + "/entries.jsonl");
    require(manifest_.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            "Cannot create version manifest");
    activeId_ = id;
    return {{"version_id", id}};
}

void Repository::recordEntry(const QJsonObject& entry)
{
    const auto line =
        QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
    writeAll(manifest_, line);
    manifestHash_.addData(line);
    ++entryCount_;
}

void Repository::addEntry(const QJsonObject& entry)
{
    require(!activeId_.isEmpty() && !content_.isOpen(), "Invalid upload state");
    const QString path = text(entry, "path");
    require(validPath(path) && !paths_.contains(path),
            "Unsafe or duplicate path: " + path);
    const auto slash = path.lastIndexOf('/');
    require(slash < 0 || directories_.contains(path.left(slash)),
            "Missing parent directory");
    const QString type = text(entry, "type");
    require(type == "file" || type == "directory", "Unsupported entry type");
    paths_.insert(path);
    currentEntry_ = {{"path", path},
                     {"type", type},
                     {"index", QString::number(entryCount_)}};
    if (type == "directory")
    {
        directories_.insert(path);
        ++directoriesCount_;
        recordEntry(currentEntry_);
        return;
    }
    const qint64 size = number(entry, "size");
    require(size <= std::numeric_limits<qint64>::max() - bytes_,
            "Version too large");
    currentEntry_.insert("size", QString::number(size));
    content_.setFileName(stagingPath_ + '/' + QString::number(entryCount_) +
                         ".data");
    require(content_.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            "Cannot create content");
    fileHash_.reset();
}

void Repository::append(const QJsonObject& request)
{
    require(content_.isOpen(), "No active file");
    const QString encoded = text(request, "data");
    require(encoded.size() <= (kChunkBytes * 4 / 3 + 4), "Chunk too large");
    const auto data = QByteArray::fromBase64Encoding(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    require(bool(data) && !data.decoded.isEmpty(), "Invalid content encoding");
    require(number(request, "offset") == content_.pos(),
            "Unexpected content offset");
    require(data.decoded.size() <=
                number(currentEntry_, "size") - content_.pos(),
            "Content exceeds declared size");
    writeAll(content_, data.decoded);
    fileHash_.addData(data.decoded);
}

void Repository::finishFile(const QString& digest)
{
    require(content_.isOpen(), "No active file");
    require(content_.pos() == number(currentEntry_, "size"),
            "Incomplete content");
    require(QString::fromLatin1(fileHash_.result().toHex()) == digest,
            "Content checksum mismatch");
    syncFile(content_);
    content_.close();
    currentEntry_.insert("sha256", digest);
    recordEntry(currentEntry_);
    ++files_;
    bytes_ += number(currentEntry_, "size");
}

QJsonObject Repository::commit(const QString& id, const QJsonArray& warnings)
{
    require(activeId_ == id && !content_.isOpen(), "Incomplete upload");
    summary_.insert("completed_at", now());
    summary_.insert("files", QString::number(files_));
    summary_.insert("directories", QString::number(directoriesCount_));
    summary_.insert("bytes", QString::number(bytes_));
    summary_.insert("entries", QString::number(entryCount_));
    summary_.insert("manifest_sha256",
                    QString::fromLatin1(manifestHash_.result().toHex()));
    summary_.insert("warnings", warnings);
    syncFile(manifest_);
    manifest_.close();
    writeObject(stagingPath_ + "/summary.json", summary_);
    // Same-filesystem rename is the sole visibility point. Old versions are
    // immutable.
    require(QDir().rename(stagingPath_, versionPath(id)),
            "Cannot publish version");
    activeId_.clear();
    syncDirectory(root_ + "/storage/versions");
    syncDirectory(root_ + "/storage/staging");
    return summary_;
}

void Repository::abort()
{
    content_.close();
    manifest_.close();
    // Keep interrupted staging for diagnosis; it is never returned by
    // list/read.
    activeId_.clear();
}
} // namespace backup::server
