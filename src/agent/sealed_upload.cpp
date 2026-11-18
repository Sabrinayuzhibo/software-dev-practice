#include "backup/agent/channel.h"
#include "backup/agent/worker_support.h"
#include "backup/core/compression.h"
#include "backup/core/detail_page.h"
#include "backup/core/source_scope.h"
#include "backup/core/version_verifier.h"
#include "sealed_session.h"

#include <QJsonDocument>
#include <limits>

namespace backup::agent
{
using namespace core;
namespace
{
QString digestOf(const QCryptographicHash& hash)
{
    return QString::fromLatin1(hash.result().toHex());
}
} // 匿名命名空间

SealedUpload::SealedUpload(Channel& channel, VaultAccess access,
                           const QJsonObject& request)
    : channel_(channel), access_(std::move(access)),
      id_(text(request, "operation_id")), codec_(compressionAlgorithm(request))
{
    if (!access_.key)
    {
        throw RepositoryLocked();
    }
    versionKey_ =
        deriveSecret(*access_.key, vaultContext(access_, id_, "version-key"));
    const SourceScope scope(request, "source");
    summary_ = scope.json("source");
    summary_.insert("format",
                    codec_ == "none" ? kPlainVersionFormat : kVersionFormat);
    if (codec_ != "none")
    {
        summary_.insert("compression", codec_);
    }
    for (const auto* field :
         {"root_metadata", "source_name", "task_id", "imported_from"})
    {
        summary_.insert(QLatin1String(field),
                        request.value(QLatin1String(field)));
    }
    summary_.insert("id", id_);
    summary_.insert("started_at", now());
    QJsonObject rules;
    for (const auto* field : {"filters", "file_types", "preserve_empty_dirs"})
    {
        rules.insert(QLatin1String(field),
                     summary_.value(QLatin1String(field)));
    }
    summary_.insert("rules", rules);
    channel_.rawCall("sealed_begin", {{"operation_id", id_},
                                      {"task_id", summary_.value("task_id")},
                                      {"repository_id",
                                       access_.header.value("repository_id")}});
}

QJsonObject SealedUpload::receipt() const
{
    auto result = summary_;
    result.insert("version_id", id_);
    result.insert("compression", codec_);
    return result;
}

QJsonObject SealedUpload::dispatch(const QString& action,
                                   const QJsonObject& value)
{
    using Write = void (SealedUpload::*)(const QJsonObject&);
    static const QHash<QString, Write> writes{
        {"entry", &SealedUpload::entry},
        {"chunk", &SealedUpload::chunk},
        {"warnings", &SealedUpload::warnings}};
    if (const auto write = writes.value(action))
    {
        (this->*write)(value);
        return {};
    }
    if (action == "finish_file")
    {
        finishFile(text(value, "sha256"));
        return {};
    }
    require(action == "commit", "Unsupported encrypted upload request");
    return commit(value);
}

void SealedUpload::record()
{
    manifest_.append(file_, summary_.value("format").toInt());
    const auto line =
        QJsonDocument(file_).toJson(QJsonDocument::Compact) + '\n';
    require(line.size() < 64 * 1024, "Manifest entry exceeds supported size");
    entries_.append(line);
    file_ = {};
}

void SealedUpload::entry(const QJsonObject& value)
{
    require(file_.isEmpty(), "Incomplete encrypted file");
    file_ = value;
    for (const auto* field :
         {"compression", "stored_size", "stored_sha256", "sealed_offset"})
    {
        file_.remove(QLatin1String(field));
    }
    file_.insert("index", QString::number(manifest_.count()));
    if (file_.value("type") != "file")
    {
        record();
        return;
    }
    fileSection_ = {position_, 0};
    originalBytes_ = 0;
    originalHash_.reset();
    encodedHash_.reset();
}

void SealedUpload::append(const QByteArray& data)
{
    if (data.isEmpty())
    {
        return;
    }
    checkOperationCancelled();
    const auto bytes = encryptionAlgorithm().seal(
        *versionKey_, data,
        vaultContext(access_, id_, QByteArray::number(position_)));
    require(bytes.size() <= std::numeric_limits<qint64>::max() - position_,
            "Encrypted version exceeds supported size");
    channel_.rawCall("sealed_chunk", {{"offset", QString::number(position_)},
                                      {"data", encodeCiphertext(bytes)}});
    position_ += bytes.size();
    storageHash_.addData(bytes);
}

void SealedUpload::flushFileBlocks()
{
    while (pending_.size() >= kChunkBytes)
    {
        append(pending_.first(kChunkBytes));
        pending_.remove(0, kChunkBytes);
    }
}

void SealedUpload::chunk(const QJsonObject& value)
{
    const auto input = decodeCiphertext(text(value, "data"), kChunkBytes);
    require(file_.value("type") == "file" && !input.isEmpty() &&
                number(value, "offset") == originalBytes_ &&
                input.size() <= number(file_, "size") - originalBytes_,
            "Invalid encrypted file chunk");
    originalBytes_ += input.size();
    originalHash_.addData(input);
    const auto encoded =
        codec_ == "none" ? input : compressBlock(input, codec_);
    encodedHash_.addData(encoded);
    require(encoded.size() <=
                std::numeric_limits<qint64>::max() - fileSection_.size,
            "Encoded file is too large");
    fileSection_.size += encoded.size();
    pending_ += encoded;
    flushFileBlocks();
}

void SealedUpload::finishFile(const QString& digest)
{
    require(file_.value("type") == "file" &&
                originalBytes_ == number(file_, "size") &&
                digestOf(originalHash_) == digest,
            "Incomplete or corrupt encrypted file");
    append(pending_);
    pending_.clear();
    file_.insert("sha256", digest);
    file_.insert("sealed_offset", QString::number(fileSection_.offset));
    if (codec_ != "none")
    {
        file_.insert("compression", codec_);
        file_.insert("stored_size", QString::number(fileSection_.size));
        file_.insert("stored_sha256", digestOf(encodedHash_));
    }
    record();
}

void SealedUpload::warnings(const QJsonObject& value)
{
    require(number(value, "offset") == warnings_.size(),
            "Unexpected warning offset");
    for (const auto& warning : value.value("warnings").toArray())
    {
        warnings_.append(detailLine(warning.toObject()));
    }
}

SealedSection SealedUpload::writeLines(const QList<QByteArray>& lines)
{
    const auto offset = position_;
    qint64 size = 0;
    for (const auto& value : lines)
    {
        require(value.size() <= std::numeric_limits<qint64>::max() - size,
                "Encrypted metadata is too large");
        size += value.size();
        pending_ += value;
        flushFileBlocks();
    }
    append(pending_);
    pending_.clear();
    return {offset, size};
}

void SealedUpload::prepareCommit()
{
    require(commitRequest_.isEmpty() && file_.isEmpty(),
            "Incomplete encrypted version");
    const auto totals = manifest_.totals();
    for (auto item = totals.begin(); item != totals.end(); ++item)
    {
        summary_.insert(item.key(), item.value());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& line : entries_)
    {
        hash.addData(line);
    }
    summary_.insert("manifest_sha256", digestOf(hash));
    VersionVerifier verifier(summary_);
    for (const auto& line : entries_)
    {
        channel_.keepAlive();
        verifier.append(parseObject(line));
    }
    verifier.finish();
    hash.reset();
    for (const auto& line : warnings_)
    {
        hash.addData(line);
    }
    summary_.insert("warnings_sha256", digestOf(hash));
    summary_.insert("warning_count", warnings_.size());
    summary_.insert("completed_at", now());
    const auto privateInfo =
        QJsonObject{{"summary", summary_},
                    {"manifest", writeLines(entries_).json()},
                    {"warnings", writeLines(warnings_).json()}};
    const auto json = QJsonDocument(privateInfo).toJson(QJsonDocument::Compact);
    require(json.size() <= 64 * 1024,
            "Encrypted summary exceeds supported size");
    const auto sealed = encryptionAlgorithm().seal(
        *versionKey_, json, vaultContext(access_, id_, "summary"));
    commitRequest_ = {{"operation_id", id_},
                      {"stored_size", QString::number(position_)},
                      {"stored_sha256", digestOf(storageHash_)},
                      {"private_summary", encodeCiphertext(sealed)}};
}

QJsonObject SealedUpload::commit(const QJsonObject& value)
{
    require(value.value("operation_id") == id_ && !commitRequest_.isEmpty(),
            "Encrypted upload is not ready to commit");
    const auto reply = channel_.rawCall("sealed_commit", commitRequest_);
    return SealedDownload::privateSummary(access_, reply)
        .value("summary")
        .toObject();
}
} // 命名空间 backup::agent
