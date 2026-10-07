#pragma once

// Blocking, bounded RPC channel; construct and use only on an Agent worker.
#include "backup/protocol/frame.h"
#include <QTcpSocket>

namespace backup::agent
{
class Channel
{
  public:
    explicit Channel(const QJsonObject& target);
    QJsonObject call(const QString& action, const QJsonObject& payload = {});
    QJsonObject health() const;
    void authenticate();

  private:
    QTcpSocket socket_;
    protocol::FrameDecoder decoder_;
    QJsonObject health_;
};
} // namespace backup::agent
