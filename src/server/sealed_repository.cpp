#include "backup/core/encryption.h"
#include "backup/core/io.h"
#include "backup/server/repository.h"

#include <QDir>
#include <QFileInfo>

namespace backup::server
{
using namespace core;

QJsonObject Repository::encryption() const
{
    return encryption_;
}

void Repository::initializeEncryption(const QJsonObject& header)
{
    require(encryption_.isEmpty() && activeId_.isEmpty(),
            "Repository encryption policy is already set or busy");
    for (const auto* name : {"versions", "staging", "deleting"})
    {
        const QDir folder(root_ + "/storage/" + name);
        require(folder.isReadable() &&
                    folder
                        .entryList(QDir::AllEntries | QDir::Hidden |
                                   QDir::System | QDir::NoDotAndDotDot)
                        .isEmpty(),
                "Encryption requires a new, empty repository");
    }
    const QDir receipts(root_ + "/database/deletions");
    require(receipts.isReadable() &&
                receipts.entryList(QDir::Files | QDir::Hidden | QDir::System)
                    .isEmpty(),
            "Encryption requires a repository with no previous backups");
    require(
        header.value("format") == 1 && validId(text(header, "repository_id")) &&
            decodeCiphertext(text(header, "salt"), kSaltBytes).size() ==
                kSaltBytes &&
            decodeCiphertext(text(header, "wrapped_key"), 32 + kSealOverhead)
                    .size() == 32 + kSealOverhead,
        "Invalid repository encryption header");
    encryption_ = {{"format", 1},
                   {"repository_id", header.value("repository_id")},
                   {"salt", header.value("salt")},
                   {"wrapped_key", header.value("wrapped_key")}};
    // 写入结果不确定时保持加密策略，避免接受明文写入。
    writeObject(root_ + "/database/encryption.json", encryption_);
}

QJsonObject Repository::beginSealed(const QJsonObject& request)
{
    require(!encryption_.isEmpty() && request.value("repository_id") ==
                                          encryption_.value("repository_id"),
            "Repository encryption policy changed");
    const auto id = text(request, "operation_id");
    require(activeId_.isEmpty() && !QFileInfo::exists(versionPath(id)),
            "Repository is busy or operation is already committed");
    require(!QFileInfo::exists(root_ + "/database/deletions/" + id + ".json"),
            "Operation belongs to a deleted version");
    const auto task = text(request, "task_id");
    require(validId(task), "Invalid task identifier");
    stagingPath_ = root_ + "/storage/staging/" + id;
    require(!QFileInfo::exists(stagingPath_), "Operation already exists");
    makeDirectory(stagingPath_);
    content_.setFileName(stagingPath_ + "/sealed.data");
    require(content_.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            "Cannot create encrypted version storage");
    summary_ = {{"sealed_format", 1},
                {"id", id},
                {"task_id", task},
                {"repository_id", encryption_.value("repository_id")},
                {"started_at", now()}};
    storedHash_.reset();
    activeId_ = id;
    sealedUpload_ = true;
    return {{"version_id", id}};
}

void Repository::appendSealed(const QJsonObject& request)
{
    require(sealedUpload_ && content_.isOpen() &&
                number(request, "offset") == content_.pos(),
            "Invalid encrypted upload offset");
    const auto bytes =
        decodeCiphertext(text(request, "data"), kChunkBytes + kSealOverhead);
    require(bytes.size() > kSealOverhead, "Invalid encrypted upload block");
    writeAll(content_, bytes);
    storedHash_.addData(bytes);
}

QJsonObject Repository::commitSealed(const QJsonObject& request)
{
    const auto id = text(request, "operation_id");
    require(sealedUpload_ && id == activeId_ && content_.isOpen() &&
                number(request, "stored_size") == content_.pos() &&
                QString::fromLatin1(storedHash_.result().toHex()) ==
                    text(request, "stored_sha256"),
            "Incomplete or corrupt encrypted upload");
    const auto summary = text(request, "private_summary");
    require(decodeCiphertext(summary, 64 * 1024 + kSealOverhead).size() >
                kSealOverhead,
            "Missing encrypted version summary");
    summary_.insert("private_summary", summary);
    summary_.insert("completed_at", now());
    summary_.insert("stored_size", QString::number(content_.pos()));
    syncFile(content_);
    content_.close();
    writeObject(stagingPath_ + "/summary.json", summary_);
    require(QDir().rename(stagingPath_, versionPath(id)),
            "Cannot publish encrypted version");
    activeId_.clear();
    sealedUpload_ = false;
    syncDirectory(root_ + "/storage/versions");
    syncDirectory(root_ + "/storage/staging");
    return summary_;
}

QJsonObject Repository::readSealed(const QJsonObject& request) const
{
    const auto id = text(request, "version_id");
    const auto header = version(id);
    require(header.value("sealed_format") == 1, "Version is not encrypted");
    const auto offset = number(request, "offset");
    const auto length = number(request, "length");
    const auto size = number(header, "stored_size");
    require(offset >= 0 && offset <= size && length > kSealOverhead &&
                length <= kChunkBytes + kSealOverhead &&
                length <= size - offset,
            "Invalid encrypted read range");
    QFile file(versionPath(id) + "/sealed.data");
    require(!QFileInfo(file).isSymLink() && file.open(QIODevice::ReadOnly) &&
                file.size() == size && file.seek(offset),
            "Missing encrypted storage");
    const auto block = file.read(length);
    require(block.size() == length, "Truncated encrypted storage");
    return {{"data", encodeCiphertext(block)}};
}
} // 命名空间 backup::server
