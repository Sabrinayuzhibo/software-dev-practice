#include "backup/core/compression.h"
#include "backup/core/io.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QtEndian>
#include <functional>
#include <iostream>
#include <random>
#include <type_traits>

using namespace backup::core;

namespace
{
static_assert(std::is_abstract_v<ICompressionAlgorithm>);
static_assert(std::has_virtual_destructor_v<ICompressionAlgorithm>);

void rejects(const std::function<void()>& operation)
{
    try
    {
        operation();
    }
    catch (const Error&)
    {
        return;
    }
    throw Error("Malformed compressed input was accepted");
}

void roundTrips(const QString& algorithm)
{
    std::mt19937 random(20261025);
    for (const auto size : {1, 7, 8, 9, 1024, 4097, int(kChunkBytes)})
    {
        QByteArray binary(size, '\0');
        for (auto& byte : binary)
        {
            byte = static_cast<char>(random());
        }
        for (const auto& input :
             {QByteArray(size, '\0'), QByteArray(size, char(255)), binary})
        {
            const auto frame = compressBlock(input, algorithm);
            require(frame.size() <= input.size() + kCompressionHeaderBytes,
                    "Incompressible data was expanded beyond its header");
            require(frame == compressBlock(input, algorithm),
                    "Compression is not deterministic");
            require(decompressBlock(frame, algorithm) == input,
                    "Binary roundtrip failed");
        }
    }
    const auto repeated = QByteArray("abcabcabc\0", 10).repeated(10000);
    const auto frame = compressBlock(repeated, algorithm);
    require(frame.size() < repeated.size() && frame[4] != 0,
            "Repeated data did not use the selected codec");
    require(decompressBlock(frame, algorithm) == repeated,
            "Repeated or overlapping dictionary data failed");
    rejects([&] { decompressBlock(frame.chopped(1), algorithm); });
    rejects([&] { decompressBlock(frame + char(0), algorithm); });
    auto corrupt = frame;
    corrupt[13] = static_cast<char>(corrupt[13] ^ 1);
    rejects([&] { decompressBlock(corrupt, algorithm); });
    corrupt = frame;
    qToBigEndian(quint32(kChunkBytes + 1), corrupt.data() + 5);
    rejects([&] { compressionBlockSize(corrupt, algorithm); });
    corrupt = frame;
    qToBigEndian(quint32(0xffffffff), corrupt.data() + 9);
    rejects([&] { compressionBlockSize(corrupt, algorithm); });
    rejects(
        [&]
        { decompressBlock(frame, algorithm == "lz77" ? "huffman" : "lz77"); });
}

void malformedCodecs()
{
    auto huffman = compressBlock(QByteArray(4097, 'a'), "huffman");
    huffman.back() = char(1);
    rejects([&] { decompressBlock(huffman, "huffman"); });
    huffman = compressBlock(QByteArray(4097, 'a'), "huffman");
    qToBigEndian(quint32(0xffffffff), huffman.data() + kCompressionHeaderBytes);
    rejects([&] { decompressBlock(huffman, "huffman"); });
    huffman = compressBlock(QByteArray(4097, 'a'), "huffman");
    qToBigEndian(quint32(0xffffffff), huffman.data() + kCompressionHeaderBytes +
                                          256 * sizeof(quint32));
    rejects([&] { decompressBlock(huffman, "huffman"); });
    auto lz77 = compressBlock(QByteArray(10000, 'a'), "lz77");
    lz77[kCompressionHeaderBytes] = char(255);
    lz77[kCompressionHeaderBytes + 1] = char(255);
    lz77[kCompressionHeaderBytes + 2] = char(255);
    rejects([&] { decompressBlock(lz77, "lz77"); });
}

void algorithmInterface()
{
    std::mt19937 random(20261026);
    QByteArray binary(kChunkBytes, '\0');
    for (auto& byte : binary)
    {
        byte = static_cast<char>(random());
    }
    for (const auto* name : {"huffman", "lz77"})
    {
        const auto algorithm = QString::fromLatin1(name);
        const ICompressionAlgorithm& codec =
            selectCompressionAlgorithm(algorithm);
        const auto expected = algorithm == "huffman" ? CompressionCode::Huffman
                                                     : CompressionCode::Lz77;
        require(codec.code() == expected, "Factory returned the wrong codec");
        for (const auto& input :
             {QByteArray(1, 'a'), QByteArray(4097, 'a'), binary})
        {
            const auto payload = codec.compress(input);
            require(codec.decompress(payload, input.size()) == input,
                    "Virtual codec roundtrip failed");
        }
        rejects([&] { codec.compress({}); });
        rejects([&] { codec.compress(QByteArray(kChunkBytes + 1, 'a')); });
        rejects([&] { codec.decompress({}, 0); });
        rejects([&] { codec.decompress({}, -1); });
        rejects([&] { codec.decompress({}, kChunkBytes + 1); });
        const auto payload = codec.compress(QByteArray(4097, 'a'));
        rejects([&] { codec.decompress(payload, 4096); });
    }
    rejects([] { selectCompressionAlgorithm("none"); });
    rejects([] { selectCompressionAlgorithm("unknown"); });
}

void wireCompatibility()
{
    // 以下摘要采集自接口重构前的自由函数实现，固定完整 BC01 块的内容。
    // 覆盖两种算法和原文回退，避免只验证新编解码器之间能够相互配合。
    struct Sample
    {
        const char* algorithm;
        QByteArray input;
        QByteArray frameSha256;
    };
    const QByteArray repeated = QByteArray("abcabcabc\0", 10).repeated(10000);
    const QByteArray single(4097, 'a');
    const QByteArray raw = QByteArray::fromHex("00010203040506070809ff");
    const Sample samples[]{
        {"huffman", repeated,
         "6f6a5fac46888b5ad811c1dfa80c889fd051301f85aa91e6cce6c01f3c5864c3"},
        {"huffman", single,
         "e1b6349d71cd5e9166c954d04b39d60473fc8cdbf3d9047d68250cff65b6258b"},
        {"huffman", raw,
         "beb4759141598be20b3ea759f6e913b3e37b65cb33089c3c216395a38d094bc2"},
        {"lz77", repeated,
         "6dc7713dcebe630ea597366fc31105589fddb3dc0f8a1ed46ddf1e19a52aae95"},
        {"lz77", single,
         "7316945b215f8cb41763f26d2a57110a57b0337ef745baab07108689956df025"},
        {"lz77", raw,
         "beb4759141598be20b3ea759f6e913b3e37b65cb33089c3c216395a38d094bc2"}};
    for (const auto& sample : samples)
    {
        const auto frame = compressBlock(sample.input, sample.algorithm);
        require(QCryptographicHash::hash(frame, QCryptographicHash::Sha256)
                        .toHex() == sample.frameSha256,
                "Interface refactor changed the stored block format");
        require(decompressBlock(frame, sample.algorithm) == sample.input,
                "Compatible block failed to restore");
    }
}
} // 匿名命名空间

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        require(compressionAlgorithm({}) == "none", "Legacy policy changed");
        rejects([] { compressionAlgorithm({{"compression", "zstd"}}); });
        rejects([] { compressionAlgorithm({{"compression", true}}); });
        rejects([] { compressBlock({}, "huffman"); });
        rejects([]
                { compressBlock(QByteArray(kChunkBytes + 1, 'a'), "lz77"); });
        rejects([] { compressionBlockSize("BC01", "huffman"); });
        roundTrips("huffman");
        roundTrips("lz77");
        malformedCodecs();
        algorithmInterface();
        wireCompatibility();
        std::cout
            << "PASS: Huffman/LZ77 binary roundtrips, deterministic ties, "
               "single symbols, overlap, padding, raw fallback and corrupt "
               "bounds; virtual interface and pre-refactor frame "
               "compatibility\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
