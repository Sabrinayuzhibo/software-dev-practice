#pragma once

#include <QObject>
#include <QTcpSocket>

#include "backup/protocol/frame.h"

namespace backup::agent {

class AgentClient : public QObject {
    Q_OBJECT

public:
    explicit AgentClient(QObject* parent = nullptr);
    void connectAndPing(const QString& host, quint16 port);

signals:
    void stateChanged(const QString& state);
    void pingSucceeded(const QString& serverName);
    void failed(const QString& reason);

private:
    QTcpSocket socket_;
    protocol::FrameDecoder decoder_;
    QString requestId_;
};

}  // namespace backup::agent
