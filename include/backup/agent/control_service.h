#pragma once

#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>

namespace backup::agent
{
// 同一用户的本机控制通道；断开客户端不取消任务。实现限制请求及缓冲大小。
class ControlService final : public QObject
{
  public:
    using Command = std::function<void(const QByteArray&)>;
    ControlService(const QString& directory, bool daemon, Command command);
    ~ControlService() override;
    void reply(const QString& id, bool ok, const QJsonObject& value);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// 图形界面的轻量代理：连接或启动唯一常驻 Agent；stdin 结束仅退出代理。
int runControlProxy(const QString& directory);
} // 命名空间 backup::agent
