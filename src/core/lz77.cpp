#include "backup/core/io.h"
#include "compression_algorithms.h"

#include <QtEndian>
#include <algorithm>
#include <array>
#include <vector>

namespace backup::core::detail
{
namespace
{
constexpr int kDictionaryBytes = 65535;
constexpr int kMaximumMatch = 255;
constexpr int kMinimumMatch = 3;
constexpr int kHashBuckets = 65536;
constexpr int kSearchLimit = 64;
constexpr int kRecordBytes = 4;

int hashAt(const QByteArray& input, int position)
{
    const auto* bytes =
        reinterpret_cast<const quint8*>(input.constData()) + position;
    return ((bytes[0] * 251 + bytes[1]) * 251 + bytes[2]) % kHashBuckets;
}
} // 匿名命名空间

CompressionCode Lz77Algorithm::code() const noexcept
{
    return CompressionCode::Lz77;
}

QByteArray Lz77Algorithm::compress(const QByteArray& input) const
{
    require(!input.isEmpty() && input.size() <= kChunkBytes,
            "Invalid LZ77 input size");
    std::array<int, kHashBuckets> heads;
    heads.fill(-1);
    std::vector<int> previous(input.size(), -1);
    QByteArray output;
    output.reserve(input.size());
    int position = 0;
    while (position < input.size())
    {
        int length = 0;
        int distance = 0;
        const int maximum = std::min(
            kMaximumMatch, static_cast<int>(input.size()) - position - 1);
        if (maximum >= kMinimumMatch)
        {
            int candidate = heads[hashAt(input, position)];
            for (int searched = 0;
                 candidate >= 0 && position - candidate <= kDictionaryBytes &&
                 searched < kSearchLimit;
                 ++searched)
            {
                int matched = 0;
                while (matched < maximum &&
                       input[candidate + matched] == input[position + matched])
                {
                    ++matched;
                }
                if (matched >= kMinimumMatch && matched > length)
                {
                    length = matched;
                    distance = position - candidate;
                }
                if (length == maximum)
                {
                    break;
                }
                candidate = previous[candidate];
            }
        }
        const auto encoded = qToBigEndian(static_cast<quint16>(distance));
        output.append(reinterpret_cast<const char*>(&encoded), sizeof(encoded));
        output.append(static_cast<char>(length));
        output.append(input[position + length]);
        const int next = position + length + 1;
        // 已处理的字节也加入字典，使后续匹配可以引用这些字节。
        for (; position < next; ++position)
        {
            if (position + kMinimumMatch <= input.size())
            {
                const auto hash = hashAt(input, position);
                previous[position] = heads[hash];
                heads[hash] = position;
            }
        }
    }
    return output;
}

QByteArray Lz77Algorithm::decompress(const QByteArray& input,
                                     qsizetype originalSize) const
{
    require(originalSize > 0 && originalSize <= kChunkBytes &&
                !input.isEmpty() && input.size() <= originalSize * kRecordBytes,
            "Invalid LZ77 payload or original size");
    const auto size = originalSize;
    require(input.size() % kRecordBytes == 0, "Truncated LZ77 record");
    QByteArray output;
    output.reserve(size);
    for (qsizetype position = 0; position < input.size();
         position += kRecordBytes)
    {
        const auto distance =
            qFromBigEndian<quint16>(input.constData() + position);
        const auto length = static_cast<quint8>(input[position + 2]);
        require((length == 0 && distance == 0) ||
                    (length > 0 && distance > 0 && distance <= output.size()),
                "Invalid LZ77 dictionary reference");
        require(length + 1 <= size - output.size(),
                "LZ77 output exceeds original length");
        for (int index = 0; index < length; ++index)
        {
            // 从持续增长的输出中读取，正确处理字典区间的重叠复制。
            const auto byte = output[output.size() - distance];
            output.append(byte);
        }
        output.append(input[position + 3]);
    }
    require(output.size() == size, "Incomplete LZ77 output");
    return output;
}
} // 命名空间 backup::core::detail
