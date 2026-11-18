#pragma once

// Blocking, bounded RPC channel; construct and use only on an Agent worker.
#include "backup/core/io.h"
#include "backup/protocol/frame.h"
#include <QElapsedTimer>
#include <QTcpSocket>

namespace backup::agent
{
class SealedUpload;
class SealedDownload;
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
    ~Channel();
    QJsonObject rawCall(const QString& action, const QJsonObject& payload = {});
    QJsonObject call(const QString& action, const QJsonObject& payload = {});
    QJsonObject health() const;
    void authenticate();
    void prepareCommit();
    void keepAlive();

  private:
    QTcpSocket socket_;
    QElapsedTimer lastResponse_;
    protocol::FrameDecoder decoder_;
    QJsonObject health_;
    std::unique_ptr<SealedUpload> upload_;
    std::unique_ptr<SealedDownload> download_;
    QString version_;
};
} // namespace backup::agent
