#pragma once

// Blocking, bounded RPC channel; construct and use only on an Agent worker.
#include "backup/core/io.h"
#include "backup/protocol/frame.h"
#include <QTcpSocket>

namespace backup::agent
{
// A received rejection is distinct from an unknown outcome after disconnect.
class ServerError : public core::Error
{
  public:
    using core::Error::Error;
};

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
