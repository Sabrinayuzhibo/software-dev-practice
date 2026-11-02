#pragma once

#include "backup/core/compression_algorithm.h"

namespace backup::core::detail
{
// 课件算法的具体实现；树、字典和缓冲均为调用内的局部资源，
// 因此工厂可以安全共享只读实例。
class HuffmanAlgorithm final : public ICompressionAlgorithm
{
  public:
    CompressionCode code() const noexcept override;
    QByteArray compress(const QByteArray& input) const override;
    QByteArray decompress(const QByteArray& input,
                          qsizetype originalSize) const override;
};

class Lz77Algorithm final : public ICompressionAlgorithm
{
  public:
    CompressionCode code() const noexcept override;
    QByteArray compress(const QByteArray& input) const override;
    QByteArray decompress(const QByteArray& input,
                          qsizetype originalSize) const override;
};
} // 命名空间 backup::core::detail
