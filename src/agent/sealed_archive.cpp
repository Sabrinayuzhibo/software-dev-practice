#include "sealed_archive.h"
#include "backup/core/io.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QtEndian>
#include <cstring>

namespace backup::agent
{
using namespace core;
namespace
{
const QByteArray kMagic("BSAE\0\1", 6);
constexpr qsizetype kHeaderLimit = 4096;

QByteArray lengthBytes(quint32 bytes)
{
    QByteArray result(sizeof(bytes), Qt::Uninitialized);
    qToBigEndian(bytes, result.data());
    return result;
}

QByteArray readExactly(QFile& file, qint64 size)
{
    const auto data = file.read(size);
    require(data.size() == size, "Truncated encrypted archive");
    return data;
}

quint32 readLength(QFile& file)
{
    return qFromBigEndian<quint32>(readExactly(file, 4).constData());
}

QByteArray contextFor(QByteArrayView header)
{
    return "BackupSystem/v1/archive/" +
           QCryptographicHash::hash(header, QCryptographicHash::Sha256);
}

QByteArray frameContext(const QByteArray& header, quint64 index, quint32 length)
{
    return header + '/' + QByteArray::number(index) + '/' + lengthBytes(length);
}
} // 匿名命名空间

SealedArchiveOutput::SealedArchiveOutput(QFile& file, VaultAccess access)
    : file_(file), access_(std::move(access))
{
    require(bool(access_.key), "REPOSITORY_LOCKED: unlock the repository");
    const auto header =
        QJsonDocument(
            QJsonObject{{"key", access_.header},
                        {"archive_id", encodeCiphertext(secureRandom(16))}})
            .toJson(QJsonDocument::Compact);
    context_ = contextFor(header);
    access_.key = deriveSecret(*access_.key, context_);
    writeAll(file_, kMagic + lengthBytes(header.size()) + header);
    open(QIODevice::WriteOnly | QIODevice::Unbuffered);
}

bool SealedArchiveOutput::isSequential() const
{
    return true;
}
qint64 SealedArchiveOutput::readData(char*, qint64)
{
    return -1;
}

void SealedArchiveOutput::frame(QByteArrayView bytes)
{
    const auto length = static_cast<quint32>(bytes.size());
    const auto sealed = encryptionAlgorithm().seal(
        *access_.key, bytes, frameContext(context_, sequence_++, length));
    writeAll(file_, lengthBytes(length));
    writeAll(file_, sealed);
}

qint64 SealedArchiveOutput::writeData(const char* data, qint64 size)
{
    for (qint64 offset = 0; offset < size;)
    {
        const auto count = qMin(size - offset, kChunkBytes - pending_.size());
        pending_.append(data + offset, count);
        offset += count;
        if (pending_.size() == kChunkBytes)
        {
            frame(pending_);
            pending_.clear();
        }
    }
    return size;
}

void SealedArchiveOutput::finish()
{
    if (!pending_.isEmpty())
    {
        frame(pending_);
        pending_.clear();
    }
    frame({});
    close();
}

bool SealedArchiveInput::detects(QFile& file)
{
    return file.peek(4) == "BSAE";
}

SealedArchiveInput::SealedArchiveInput(QFile& file, const QString& password,
                                       const VaultAccess& existing)
    : file_(file)
{
    require(readExactly(file_, kMagic.size()) == kMagic,
            "Unsupported encrypted archive format");
    const auto size = readLength(file_);
    require(size > 0 && size <= kHeaderLimit,
            "Invalid encrypted archive header size");
    const auto header = readExactly(file_, size);
    const auto json = parseObject(header);
    require(decodeCiphertext(text(json, "archive_id"), 16).size() == 16,
            "Invalid encrypted archive identifier");
    const auto key = json.value("key").toObject();
    access_ = existing.key && existing.header == key
                  ? existing
                  : unlockVault(key, password);
    context_ = contextFor(header);
    access_.key = deriveSecret(*access_.key, context_);
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

bool SealedArchiveInput::isSequential() const
{
    return true;
}
qint64 SealedArchiveInput::writeData(const char*, qint64)
{
    return -1;
}

void SealedArchiveInput::frame()
{
    const auto size = readLength(file_);
    require(size <= kChunkBytes, "Encrypted archive block is too large");
    pending_ = encryptionAlgorithm().open(
        *access_.key, readExactly(file_, size + kSealOverhead),
        frameContext(context_, sequence_++, size));
    position_ = 0;
    if (size == 0)
    {
        char tail;
        require(file_.read(&tail, 1) == 0, "Trailing encrypted archive bytes");
        finished_ = true;
    }
}

qint64 SealedArchiveInput::readData(char* output, qint64 size)
{
    qint64 total = 0;
    while (total < size && !finished_)
    {
        if (position_ == pending_.size())
        {
            frame();
            if (finished_)
            {
                break;
            }
        }
        const auto count = qMin(size - total, pending_.size() - position_);
        std::memcpy(output + total, pending_.constData() + position_, count);
        total += count;
        position_ += count;
    }
    return total;
}
} // 命名空间 backup::agent
