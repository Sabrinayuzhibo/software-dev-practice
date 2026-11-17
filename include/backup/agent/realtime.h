#pragma once

#include <QJsonArray>
#include <QObject>
#include <functional>
#include <memory>

namespace backup::agent
{
// Linux 监听与调度独立于备份实现；Start 在事件线程执行，复用手动备份流程。
class Realtime final : public QObject
{
  public:
    using Start = std::function<void(const QString&)>;
    Realtime(std::function<bool()> busy, Start start);
    ~Realtime() override;
    void configure(const QJsonObject& config);
    // 执行结果须先持久化；不确定提交会暂停该任务，直到原操作确认完成。
    void completed(const QJsonObject& operation);
    QJsonObject status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // 命名空间 backup::agent
