#include "backup/agent/agent_client.h"

#include <QJsonValue>
#include <QUuid>

namespace backup::agent {

AgentClient::AgentClient(QObject* parent) : QObject(parent) {
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        emit stateChanged(QStringLiteral("已连接，正在检查服务端…"));
        const QByteArray frame = protocol::encode(
            {QStringLiteral("ping"), requestId_, {}});
        socket_.write(frame);
    });
    connect(&socket_, &QTcpSocket::readyRead, this, [this] {
        QList<protocol::Message> messages;
        QString error;
        if (!decoder_.feed(socket_.readAll(), &messages, &error)) {
            emit failed(error);
            socket_.disconnectFromHost();
            return;
        }
        for (const auto& message : messages) {
            if (message.requestId != requestId_) {
                continue;
            }
            if (message.type == QStringLiteral("pong")) {
                emit pingSucceeded(
                    message.payload.value("server").toString());
            } else if (message.type == QStringLiteral("error")) {
                emit failed(message.payload.value("reason").toString());
            }
        }
    });
    connect(&socket_, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                emit failed(socket_.errorString());
            });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        emit stateChanged(QStringLiteral("连接已断开"));
    });
}

void AgentClient::connectAndPing(const QString& host, quint16 port) {
    socket_.abort();
    decoder_ = protocol::FrameDecoder();
    requestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit stateChanged(QStringLiteral("正在连接 %1:%2…").arg(host).arg(port));
    socket_.connectToHost(host, port);
}

}  // namespace backup::agent
