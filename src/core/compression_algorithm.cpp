#include "backup/core/compression_algorithm.h"
#include "backup/core/io.h"
#include "compression_algorithms.h"

namespace backup::core
{
const ICompressionAlgorithm& selectCompressionAlgorithm(const QString& name)
{
    static const detail::HuffmanAlgorithm huffman;
    static const detail::Lz77Algorithm lz77;
    if (name == "huffman")
    {
        return huffman;
    }
    if (name == "lz77")
    {
        return lz77;
    }
    throw Error("Unsupported compression algorithm: " + name);
}
} // 命名空间 backup::core
