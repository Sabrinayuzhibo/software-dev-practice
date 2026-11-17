#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>

namespace backup::agent
{
class Channel;

// 遍历器只描述条目和数据块；接收者决定是否上传及如何处理失败。
// 方法在当前工作线程同步执行，失败可抛出 core::Error。
// 一个普通文件依次 entry、零到多个 chunk、finishFile；其他类型只有 entry。
// problem 的 status 已初始化 error_count；扫描累计失败，上传抛出后终止。
class ISourceSink
{
  public:
    virtual ~ISourceSink() = default;
    virtual QString stage() const = 0;
    virtual void entry(const QJsonObject& value) = 0;
    virtual void chunk(qint64 offset, const QByteArray& bytes) = 0;
    virtual void finishFile(const QString& sha256) = 0;
    virtual void problem(const QString& path, const QString& reason,
                         QJsonObject& status, QJsonArray& warnings) const = 0;
};

class ScanSourceSink final : public ISourceSink
{
  public:
    QString stage() const override;
    void entry(const QJsonObject&) override;
    void chunk(qint64, const QByteArray&) override;
    void finishFile(const QString&) override;
    void problem(const QString& path, const QString& reason,
                 QJsonObject& status, QJsonArray& warnings) const override;
};

// 借用 Channel；调用者保证它与接收者同线程且生命周期更长。
class UploadSourceSink final : public ISourceSink
{
  public:
    explicit UploadSourceSink(Channel& channel);
    QString stage() const override;
    void entry(const QJsonObject& value) override;
    void chunk(qint64 offset, const QByteArray& bytes) override;
    void finishFile(const QString& sha256) override;
    void problem(const QString& path, const QString& reason,
                 QJsonObject& status, QJsonArray& warnings) const override;

  private:
    Channel& channel_;
};
} // namespace backup::agent
