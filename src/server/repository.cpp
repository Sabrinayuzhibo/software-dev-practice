#include "backup/server/repository.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <algorithm>
#include <filesystem>
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
    const auto checked = health();
    require(checked.value("storage_state") == "available",
            checked.value("storage_error").toString());
}

QString Repository::root() const
{
    return root_;
}
QString Repository::token() const
{
    return token_;
}

QJsonObject Repository::health() const
{
    QJsonObject result{{"server", "Backup Server"},
                       {"data_root", root_},
                       {"storage_state", "available"}};
    try
    {
        for (const auto* folder :
             {"database", "storage/staging", "storage/versions"})
        {
            const auto path = root_ + '/' + folder;
            require(!QFileInfo(path).isSymLink(),
                    "Repository contains a link: " + path);
            std::filesystem::directory_iterator readable(
                QFile::encodeName(path).toStdString());
            QTemporaryFile probe(path + "/.probe-XXXXXX");
            const auto opened = probe.open();
            require(opened, "Storage unavailable: " + path + ": " +
                                probe.errorString());
            writeAll(probe, "storage check\n");
            syncFile(probe);
            require(probe.seek(0) && probe.readAll() == "storage check\n",
                    "Cannot read storage probe: " + path);
            require(probe.remove(), "Cannot remove storage probe");
            syncDirectory(path);
        }
    }
    catch (const std::exception& error)
    {
        result.insert("storage_state", "unavailable");
        result.insert("storage_error", QString::fromUtf8(error.what()));
    }
    return result;
}

QString Repository::versionPath(const QString& id) const
{
    require(validId(id), "Invalid version identifier");
    return root_ + "/storage/versions/" + id;
}

QJsonObject Repository::version(const QString& id) const
{
    auto result = readObject(versionPath(id) + "/summary.json");
    require(result.value("format").toInt() == 1 && result.value("id") == id,
            "Unsupported or corrupt version");
    if (result.contains("warnings"))
    {
        require(result.value("warnings").isArray(), "Invalid version warnings");
        result.insert("warning_count",
                      result.value("warnings").toArray().size());
        result.remove("warnings");
    }
    return result;
}

QJsonObject Repository::warnings(const QJsonObject& request) const
{
    const auto id = text(request, "version_id");
    const auto summary = readObject(versionPath(id) + "/summary.json");
    require(summary.value("format").toInt() == 1 && summary.value("id") == id &&
                (!summary.contains("warnings") ||
                 summary.value("warnings").isArray()),
            "Unsupported or corrupt version");
    const auto offset =
        request.contains("offset") ? number(request, "offset") : 0;
    auto page = summary.contains("warnings")
                    ? detailPage(summary.value("warnings").toArray(), offset)
                    : readDetailPage(versionPath(id) + "/warnings.jsonl",
                                     offset, number(summary, "warning_count"),
                                     text(summary, "warnings_sha256"));
    page.insert("warnings", page.take("items"));
    return page;
}

QJsonObject Repository::list(const QJsonObject& request) const
{
    QList<QJsonObject> versions;
    const auto path = root_ + "/storage/versions";
    // QDir::entryList silently returns an empty list for an unreadable folder.
    for (const auto& entry : std::filesystem::directory_iterator(
             QFile::encodeName(path).toStdString()))
    {
        if (!entry.is_directory())
        {
            continue;
        }
        const auto name =
            QString::fromStdString(entry.path().filename().string());
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
    QJsonArray items;
    for (const auto& item : versions)
    {
        items.append(item);
    }
    auto page = detailPage(items, offset);
    page.insert("versions", page.take("items"));
    return page;
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
    qint64 bytes = 0;
    while (!file.atEnd() && page.size() < kPageSize)
    {
        const auto position = file.pos();
        const auto line = file.readLine(64 * 1024);
        require(line.endsWith('\n'), "Corrupt manifest entry");
        if (bytes + line.size() > kDetailPageBytes && !page.isEmpty())
        {
            require(file.seek(position), "Cannot seek manifest");
            break;
        }
        page.append(parseObject(line));
        bytes += line.size();
    }
    require(file.error() == QFileDevice::NoError,
            "Cannot read version manifest");
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
    warningHash_.reset();
    warningCount_ = 0;
    try
    {
        manifest_.setFileName(stagingPath_ + "/entries.jsonl");
        require(manifest_.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                "Cannot create version manifest");
        warnings_.setFileName(stagingPath_ + "/warnings.jsonl");
        require(warnings_.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                "Cannot create version warnings");
    }
    catch (...)
    {
        // begin has not transferred ownership to the connection yet.
        abort();
        throw;
    }
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

void Repository::appendWarnings(const QJsonObject& request)
{
    require(!activeId_.isEmpty() && warnings_.isOpen(), "No active upload");
    require(number(request, "offset") == warningCount_,
            "Invalid warning offset");
    require(request.value("warnings").isArray(), "Invalid warning batch");
    const auto items = request.value("warnings").toArray();
    require(items.size() <= kPageSize, "Too many warnings in one batch");
    for (const auto& value : items)
    {
        const auto warning = value.toObject();
        const auto line = detailLine({{"path", text(warning, "path")},
                                      {"reason", text(warning, "reason")}});
        writeAll(warnings_, line);
        warningHash_.addData(line);
        ++warningCount_;
    }
}

QJsonObject Repository::commit(const QString& id, const QJsonArray& warnings)
{
    require(activeId_ == id && !content_.isOpen(), "Incomplete upload");
    // Accept the original commit interface for existing clients.
    require(warnings.isEmpty() || warningCount_ == 0,
            "Warnings already uploaded");
    qint64 offset = 0;
    while (offset < warnings.size())
    {
        const auto page = detailPage(warnings, offset);
        const auto items = page.value("items").toArray();
        appendWarnings({{"offset", offset}, {"warnings", items}});
        offset += items.size();
    }
    summary_.insert("completed_at", now());
    summary_.insert("files", QString::number(files_));
    summary_.insert("directories", QString::number(directoriesCount_));
    summary_.insert("bytes", QString::number(bytes_));
    summary_.insert("entries", QString::number(entryCount_));
    summary_.insert("manifest_sha256",
                    QString::fromLatin1(manifestHash_.result().toHex()));
    summary_.insert("warning_count", warningCount_);
    summary_.insert("warnings_sha256",
                    QString::fromLatin1(warningHash_.result().toHex()));
    syncFile(warnings_);
    warnings_.close();
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
    warnings_.close();
    // Keep interrupted staging for diagnosis; it is never returned by
    // list/read.
    activeId_.clear();
}
} // namespace backup::server
