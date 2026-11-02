#pragma once

#include "backup/core/compression_algorithm.h"
#include <QJsonObject>

namespace backup::core
{
inline constexpr qsizetype kCompressionHeaderBytes = 45;

// 未指定策略时默认不压缩；未知算法或非字符串配置会被拒绝。
QString compressionAlgorithm(const QJsonObject& value);

// 独立编码不超过 kChunkBytes 的非空块，记录算法、长度和 SHA-256。
// 压缩负载未缩小时保留原文。
QByteArray compressBlock(const QByteArray& input, const QString& algorithm);

// 输入须包含完整固定头；缓冲负载前先校验声明的长度。
qsizetype compressionBlockSize(const QByteArray& header,
                               const QString& algorithm);

// 解码一个完整块，限制输出大小并校验补位、引用、原文长度和摘要。
// 输入格式异常或数据损坏时抛出 Error。
QByteArray decompressBlock(const QByteArray& frame, const QString& algorithm);
} // 命名空间 backup::core
