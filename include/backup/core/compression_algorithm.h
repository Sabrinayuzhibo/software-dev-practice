#pragma once

#include <QByteArray>
#include <QString>

namespace backup::core
{
// 稳定的磁盘格式编号；Raw 表示块封装层回退保存的原文。
enum class CompressionCode : quint8
{
    Raw = 0,
    Huffman = 1,
    Lz77 = 2
};

// 将课件 ICompressionAlgorithm 接口适配为有界字节块接口。
// 实现类不保存调用状态，各次调用独立管理缓冲，可并发执行。
class ICompressionAlgorithm
{
  public:
    virtual ~ICompressionAlgorithm() = default;

    virtual CompressionCode code() const noexcept = 0;

    // 编码 1 至 kChunkBytes 字节；负载可能变大，由封装层决定是否保留。
    // 输入不合法时抛出 Error。
    virtual QByteArray compress(const QByteArray& input) const = 0;

    // 将完整负载解码为恰好 originalSize 字节，范围为 1 至 kChunkBytes。
    // 数据损坏或长度不合法时抛出 Error。
    virtual QByteArray decompress(const QByteArray& input,
                                  qsizetype originalSize) const = 0;
};

// 返回生命周期覆盖整个程序的共享只读算法对象，调用者不拥有或释放它。
// 名称为 none 或未知算法时抛出 Error。
const ICompressionAlgorithm& selectCompressionAlgorithm(const QString& name);
} // 命名空间 backup::core
