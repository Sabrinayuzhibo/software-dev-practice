#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <functional>

namespace backup::core
{
inline constexpr quint16 kArchiveFormat = 1;
inline constexpr qint64 kArchiveMetadataBytes = 64 * 1024;
enum class ArchiveRecord : quint32
{
    Summary = 1,
    Warning = 2,
    Entry = 3,
    Data = 4,
    FileEnd = 5,
    Final = 6
};

// 文件头、带 64 位长度的记录及最终摘要；借用已打开的输出，不负责发布文件。
class ArchiveWriter
{
  public:
    explicit ArchiveWriter(QFile& output);
    void write(ArchiveRecord type, const QByteArray& payload);
    void finish();

  private:
    QFile& output_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    qint64 records_ = 0;
    qint64 bytes_ = 0;
    bool finished_ = false;
};

// 回调同步执行，不持有传入内容；文件数据已逐块解码，完整文件在 fileEnd 前校验。
// 调用方只有在 verifyArchive 成功返回后才能提交版本或发布归档。
struct ArchiveCallbacks
{
    std::function<void()> checkpoint;
    std::function<void(const QJsonObject&)> summary;
    std::function<void(const QJsonObject&)> warning;
    std::function<void(const QJsonObject&)> entry;
    std::function<void(const QByteArray&)> content;
    std::function<void()> fileEnd;
};
struct ArchiveInfo
{
    QJsonObject summary;
    QByteArray digest;
    qint64 records;
    qint64 bytes;
};

// 从当前位置读取完整 BSAR1，拒绝未知格式、越界长度、损坏清单/内容及多余尾字节。
// 文件内容使用有界缓冲；清单结构校验空间与条目数量相关。失败抛出 Error。
ArchiveInfo verifyArchive(QFile& input, const ArchiveCallbacks& callbacks = {});
} // 命名空间 backup::core
