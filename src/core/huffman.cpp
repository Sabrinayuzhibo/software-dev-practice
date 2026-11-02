#include "backup/core/io.h"
#include "compression_algorithms.h"

#include <QtEndian>
#include <array>
#include <functional>
#include <queue>
#include <vector>

namespace backup::core::detail
{
namespace
{
constexpr int kAlphabetSize = 256;
constexpr qsizetype kTableBytes = kAlphabetSize * sizeof(quint32);
constexpr qsizetype kPayloadOffset = kTableBytes + sizeof(quint32);
using Frequencies = std::array<quint32, kAlphabetSize>;

struct Node
{
    quint32 frequency;
    int symbol;
    int left = -1;
    int right = -1;
};

std::vector<Node> buildTree(const Frequencies& frequencies)
{
    std::vector<Node> nodes;
    using Item = std::pair<quint32, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    for (int symbol = 0; symbol < kAlphabetSize; ++symbol)
    {
        if (frequencies[symbol] != 0)
        {
            queue.emplace(frequencies[symbol], static_cast<int>(nodes.size()));
            nodes.push_back({frequencies[symbol], symbol});
        }
    }
    // 频率相同时按节点插入顺序排序，保证各平台建树结果一致。
    while (queue.size() > 1)
    {
        const auto left = queue.top();
        queue.pop();
        const auto right = queue.top();
        queue.pop();
        const auto frequency = left.first + right.first;
        queue.emplace(frequency, static_cast<int>(nodes.size()));
        nodes.push_back({frequency, -1, left.second, right.second});
    }
    return nodes;
}

void makeCodes(const std::vector<Node>& tree, int index, QByteArray prefix,
               std::array<QByteArray, kAlphabetSize>& codes)
{
    const auto& node = tree[index];
    if (node.symbol >= 0)
    {
        // 单一符号仍使用一位编码；空文件不产生数据块。
        codes[node.symbol] = prefix.isEmpty() ? QByteArray(1, '\0') : prefix;
        return;
    }
    makeCodes(tree, node.left, prefix + char(0), codes);
    makeCodes(tree, node.right, prefix + char(1), codes);
}

Frequencies readFrequencies(const QByteArray& input, qsizetype size)
{
    require(input.size() >= kPayloadOffset,
            "Truncated Huffman frequency table");
    Frequencies frequencies{};
    quint64 total = 0;
    for (int symbol = 0; symbol < kAlphabetSize; ++symbol)
    {
        frequencies[symbol] = qFromBigEndian<quint32>(input.constData() +
                                                      symbol * sizeof(quint32));
        total += frequencies[symbol];
    }
    require(total == static_cast<quint64>(size),
            "Huffman frequencies do not match original length");
    return frequencies;
}
} // 匿名命名空间

CompressionCode HuffmanAlgorithm::code() const noexcept
{
    return CompressionCode::Huffman;
}

QByteArray HuffmanAlgorithm::compress(const QByteArray& input) const
{
    require(!input.isEmpty() && input.size() <= kChunkBytes,
            "Invalid Huffman input size");
    Frequencies frequencies{};
    for (const auto byte : input)
    {
        ++frequencies[static_cast<quint8>(byte)];
    }
    const auto tree = buildTree(frequencies);
    std::array<QByteArray, kAlphabetSize> codes;
    makeCodes(tree, static_cast<int>(tree.size()) - 1, {}, codes);
    quint32 bitCount = 0;
    for (int symbol = 0; symbol < kAlphabetSize; ++symbol)
    {
        bitCount += frequencies[symbol] * codes[symbol].size();
    }
    QByteArray output(kPayloadOffset + (bitCount + 7) / 8, '\0');
    for (int symbol = 0; symbol < kAlphabetSize; ++symbol)
    {
        qToBigEndian(frequencies[symbol],
                     output.data() + symbol * sizeof(quint32));
    }
    qToBigEndian(bitCount, output.data() + kTableBytes);
    quint32 position = 0;
    for (const auto byte : input)
    {
        for (const auto bit : codes[static_cast<quint8>(byte)])
        {
            if (bit)
            {
                const auto offset = kPayloadOffset + position / 8;
                output[offset] =
                    static_cast<char>(static_cast<quint8>(output[offset]) |
                                      (1u << (7 - position % 8)));
            }
            ++position;
        }
    }
    return output;
}

QByteArray HuffmanAlgorithm::decompress(const QByteArray& input,
                                        qsizetype originalSize) const
{
    require(originalSize > 0 && originalSize <= kChunkBytes &&
                input.size() <= kPayloadOffset + kChunkBytes,
            "Invalid Huffman payload or original size");
    const auto size = originalSize;
    const auto frequencies = readFrequencies(input, size);
    const auto bitCount =
        qFromBigEndian<quint32>(input.constData() + kTableBytes);
    require(bitCount > 0 && bitCount <= size * 8 &&
                input.size() == kPayloadOffset + (bitCount + 7) / 8,
            "Invalid Huffman bit count or payload length");
    const auto padding = (8 - bitCount % 8) % 8;
    require((static_cast<quint8>(input.back()) & ((1u << padding) - 1)) == 0,
            "Nonzero Huffman padding");
    const auto tree = buildTree(frequencies);
    const int root = static_cast<int>(tree.size()) - 1;
    int current = root;
    QByteArray output;
    output.reserve(size);
    Frequencies decoded{};
    for (quint32 position = 0; position < bitCount; ++position)
    {
        const auto byte =
            static_cast<quint8>(input[kPayloadOffset + position / 8]);
        const bool bit = (byte & (1u << (7 - position % 8))) != 0;
        if (tree[root].symbol < 0)
        {
            current = bit ? tree[current].right : tree[current].left;
        }
        else
        {
            require(!bit, "Invalid single-symbol Huffman code");
        }
        const auto symbol = tree[current].symbol;
        if (symbol >= 0)
        {
            require(output.size() < size,
                    "Huffman output exceeds original length");
            output.append(static_cast<char>(symbol));
            ++decoded[symbol];
            current = root;
        }
    }
    require(current == root && output.size() == size && decoded == frequencies,
            "Incomplete Huffman code or inconsistent frequencies");
    return output;
}
} // 命名空间 backup::core::detail
