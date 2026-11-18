#include "backup/agent/channel.h"
#include "backup/core/compression.h"
#include "backup/core/detail_page.h"
#include "sealed_session.h"

#include <limits>

namespace backup::agent
{
using namespace core;

QJsonObject SealedSection::json() const
{
    return {{"offset", QString::number(offset)},
            {"size", QString::number(size)}};
}

SealedSection SealedSection::parse(const QJsonValue& value)
{
    const auto section = value.toObject();
    return {number(section, "offset"), number(section, "size")};
}

QJsonObject SealedDownload::privateSummary(const VaultAccess& access,
                                           const QJsonObject& header)
{
    if (!access.key)
    {
        throw RepositoryLocked();
    }
    const auto id = text(header, "id");
    require(validId(id) && header.value("sealed_format") == 1 &&
                header.value("repository_id") ==
                    access.header.value("repository_id"),
            "Invalid encrypted version identity");
    const auto sealed = decodeCiphertext(text(header, "private_summary"),
                                         64 * 1024 + kSealOverhead);
    const auto key =
        deriveSecret(*access.key, vaultContext(access, id, "version-key"));
    const auto result = parseObject(encryptionAlgorithm().open(
        *key, sealed, vaultContext(access, id, "summary")));
    const auto summary = result.value("summary").toObject();
    require(summary.value("id") == id &&
                summary.value("task_id") == header.value("task_id"),
            "Encrypted version identity mismatch");
    return result;
}

SealedDownload::SealedDownload(Channel& channel, VaultAccess access,
                               QJsonObject header)
    : channel_(channel), access_(std::move(access)), id_(text(header, "id")),
      summary_(privateSummary(access_, header))
{
    versionKey_ =
        deriveSecret(*access_.key, vaultContext(access_, id_, "version-key"));
}

QJsonObject SealedDownload::dispatch(const QString& action,
                                     const QJsonObject& request)
{
    if (action == "entries")
    {
        return entries(number(request, "offset"));
    }
    if (action == "download")
    {
        return data(request);
    }
    if (action == "version_warnings")
    {
        return warnings(request.contains("offset") ? number(request, "offset")
                                                   : 0);
    }
    return summary();
}

QJsonObject SealedDownload::summary() const
{
    return summary_.value("summary").toObject();
}

QByteArray SealedDownload::block(SealedSection section, qint64 offset)
{
    require(offset >= 0 && offset <= section.size && offset % kChunkBytes == 0,
            "Invalid decrypted block offset");
    if (offset == section.size)
    {
        return {};
    }
    const auto seals = offset / kChunkBytes * kSealOverhead;
    require(seals <= std::numeric_limits<qint64>::max() - offset &&
                section.offset <=
                    std::numeric_limits<qint64>::max() - offset - seals,
            "Encrypted offset overflow");
    const auto position = section.offset + offset + seals;
    if (position == cachedAt_)
    {
        return cached_;
    }
    const auto count = qMin(kChunkBytes, section.size - offset) + kSealOverhead;
    const auto value =
        channel_.rawCall("sealed_read", {{"version_id", id_},
                                         {"offset", QString::number(position)},
                                         {"length", QString::number(count)}});
    const auto data = decodeCiphertext(text(value, "data"), count);
    require(data.size() == count, "Truncated encrypted block");
    auto plain = encryptionAlgorithm().open(
        *versionKey_, data,
        vaultContext(access_, id_, QByteArray::number(position)));
    cachedAt_ = position;
    cached_ = std::move(plain);
    return cached_;
}

QByteArray SealedDownload::line(SealedSection section, qint64& offset)
{
    QByteArray result;
    while (offset < section.size && result.size() < 64 * 1024)
    {
        const auto inner = offset % kChunkBytes;
        const auto data = block(section, offset - inner);
        const auto end = data.indexOf('\n', inner);
        const auto length = (end < 0 ? data.size() : end + 1) - inner;
        require(length > 0, "Invalid encrypted line offset");
        result.append(data.constData() + inner, length);
        offset += length;
        if (end >= 0)
        {
            require(result.size() < 64 * 1024, "Encrypted line is too long");
            return result;
        }
    }
    throw Error("Incomplete encrypted metadata line");
}

QJsonObject SealedDownload::entries(qint64 offset)
{
    const auto section = SealedSection::parse(summary_.value("manifest"));
    require(offset >= 0 && offset <= section.size, "Invalid manifest offset");
    QJsonArray page;
    qint64 bytes = 0;
    while (offset < section.size && page.size() < kPageSize)
    {
        auto next = offset;
        const auto encoded = line(section, next);
        if (!page.isEmpty() && bytes + encoded.size() > kDetailPageBytes)
        {
            break;
        }
        const auto entry = parseObject(encoded);
        if (entry.value("type") == "file")
        {
            files_.insert(number(entry, "index"),
                          {number(entry, "sealed_offset"),
                           compressionAlgorithm(entry) == "none"
                               ? number(entry, "size")
                               : number(entry, "stored_size")});
        }
        page.append(entry);
        bytes += encoded.size();
        offset = next;
    }
    return {{"entries", page},
            {"offset", QString::number(offset)},
            {"done", offset == section.size}};
}

QJsonObject SealedDownload::warnings(qint64 offset)
{
    const auto section = SealedSection::parse(summary_.value("warnings"));
    const auto count = number(summary(), "warning_count");
    require(offset >= 0 && offset <= count, "Invalid warning offset");
    if (offset < warningIndex_)
    {
        warningIndex_ = 0;
        warningOffset_ = 0;
    }
    while (warningIndex_ < offset)
    {
        line(section, warningOffset_);
        ++warningIndex_;
    }
    QJsonArray page;
    qint64 bytes = 0;
    while (warningIndex_ < count && page.size() < kPageSize)
    {
        auto next = warningOffset_;
        const auto encoded = line(section, next);
        if (!page.isEmpty() && bytes + encoded.size() > kDetailPageBytes)
        {
            break;
        }
        page.append(parseObject(encoded));
        bytes += encoded.size();
        warningOffset_ = next;
        ++warningIndex_;
    }
    require(warningIndex_ < count || warningOffset_ == section.size,
            "Encrypted warning count mismatch");
    const QJsonValue next =
        warningIndex_ < count ? QJsonValue(warningIndex_) : QJsonValue();
    return {
        {"warnings", page}, {"warning_count", count}, {"next_offset", next}};
}

QJsonObject SealedDownload::data(const QJsonObject& request)
{
    const auto index = number(request, "index");
    require(files_.contains(index),
            "Encrypted content requires a manifest entry");
    const auto section = files_.value(index);
    const auto offset = number(request, "offset");
    const auto bytes = block(section, offset);
    return {{"data", encodeCiphertext(bytes)},
            {"offset", QString::number(offset)},
            {"done", bytes.size() == section.size - offset}};
}
} // 命名空间 backup::agent
