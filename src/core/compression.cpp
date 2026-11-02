#include "backup/core/compression.h"
#include "backup/core/io.h"

#include <QCryptographicHash>
#include <QtEndian>

namespace backup::core
{
namespace
{
constexpr auto kMagic = "BC01";
constexpr qsizetype kDigestOffset = 13;

void appendNumber(QByteArray& output, quint32 value)
{
    const auto encoded = qToBigEndian(value);
    output.append(reinterpret_cast<const char*>(&encoded), sizeof(encoded));
}
} // 匿名命名空间

QString compressionAlgorithm(const QJsonObject& value)
{
    if (!value.contains("compression"))
    {
        return "none";
    }
    const auto algorithm = value.value("compression");
    require(algorithm.isString(), "Unsupported compression algorithm");
    const auto name = algorithm.toString();
    if (name != "none")
    {
        selectCompressionAlgorithm(name);
    }
    return name;
}

QByteArray compressBlock(const QByteArray& input, const QString& algorithm)
{
    const ICompressionAlgorithm& codec = selectCompressionAlgorithm(algorithm);
    require(!input.isEmpty() && input.size() <= kChunkBytes,
            "Invalid compression block size");
    auto payload = codec.compress(input);
    const bool compressed = payload.size() < input.size();
    if (!compressed)
    {
        payload = input;
    }
    QByteArray frame(kMagic, 4);
    frame.append(
        static_cast<char>(compressed ? codec.code() : CompressionCode::Raw));
    appendNumber(frame, static_cast<quint32>(input.size()));
    appendNumber(frame, static_cast<quint32>(payload.size()));
    frame.append(QCryptographicHash::hash(input, QCryptographicHash::Sha256));
    frame.append(payload);
    return frame;
}

qsizetype compressionBlockSize(const QByteArray& header,
                               const QString& algorithm)
{
    const auto expected =
        static_cast<quint8>(selectCompressionAlgorithm(algorithm).code());
    const auto raw = static_cast<quint8>(CompressionCode::Raw);
    require(header.size() >= kCompressionHeaderBytes &&
                header.first(4) == QByteArray(kMagic, 4),
            "Invalid compressed block header");
    const auto code = static_cast<quint8>(header[4]);
    const auto original = qFromBigEndian<quint32>(header.constData() + 5);
    const auto stored = qFromBigEndian<quint32>(header.constData() + 9);
    require((code == raw || code == expected) && original > 0 &&
                original <= kChunkBytes && stored > 0 && stored <= original &&
                (code != raw || stored == original),
            "Invalid compressed block algorithm or lengths");
    return kCompressionHeaderBytes + stored;
}

QByteArray decompressBlock(const QByteArray& frame, const QString& algorithm)
{
    require(frame.size() == compressionBlockSize(frame, algorithm),
            "Truncated or trailing compressed block data");
    const auto size = qFromBigEndian<quint32>(frame.constData() + 5);
    const auto payload = frame.sliced(kCompressionHeaderBytes);
    const auto code = static_cast<quint8>(frame[4]);
    const ICompressionAlgorithm& codec = selectCompressionAlgorithm(algorithm);
    const auto decoded = code == static_cast<quint8>(CompressionCode::Raw)
                             ? payload
                             : codec.decompress(payload, size);
    require(decoded.size() == size &&
                QCryptographicHash::hash(decoded, QCryptographicHash::Sha256) ==
                    frame.sliced(kDigestOffset, 32),
            "Decompressed block checksum or length mismatch");
    return decoded;
}
} // 命名空间 backup::core
