#pragma once

#include "backup/core/io.h"
#include <QDeadlineTimer>
#include <QHash>

namespace backup::agent
{
class Channel;

enum class ConnectionFailure
{
    Network,
    Authentication,
    Certificate
};

class ChannelError : public core::Error
{
  public:
    ChannelError(ConnectionFailure kind, const QString& message);
    ConnectionFailure kind() const;
    QString code() const;

  private:
    ConnectionFailure kind_;
};

bool remoteTarget(const QJsonObject& target);
void validateEndpoint(const QJsonObject& target);
QString endpointKey(const QJsonObject& target);

// 会话令牌仅由 Agent 的串行数据工作线程访问，不属于目标配置。
class RemoteSessions
{
  public:
    class Scope
    {
      public:
        explicit Scope(RemoteSessions& sessions);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
    static void resume(Channel& channel, const QJsonObject& target);
    QJsonObject login(Channel& channel, const QJsonObject& target,
                      const QJsonObject& args);

  private:
    struct Session
    {
        QString token;
        QDeadlineTimer expiry;
    };
    QHash<QString, Session> sessions_;
};
} // 命名空间 backup::agent
