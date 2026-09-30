#include "backup/server/backup_server.h"

#include <QDir>
#include <QHostAddress>
#include <QJsonObject>
#include <QTcpSocket>

#include <memory>

#include "backup/protocol/frame.h"

namespace backup::server {

BackupServer::BackupServer(QObject* parent) : QObject(parent) {
    connect(&listener_, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* socket = listener_.nextPendingConnection()) {
            auto decoder = std::make_shared<protocol::FrameDecoder>();
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, decoder] {
                        QList<protocol::Message> messages;
                        QString error;
                        if (!decoder->feed(socket->readAll(), &messages, &error)) {
                            socket->disconnectFromHost();
                            return;
                        }
                        for (const auto& message : messages) {
                            protocol::Message reply;
                            reply.requestId = message.requestId;
                            if (message.type == QStringLiteral("ping")) {
                                reply.type = QStringLiteral("pong");
                                reply.payload = {{"server", "Backup Server"}};
                            } else {
                                reply.type = QStringLiteral("error");
                                reply.payload = {{"reason", "Unsupported request"}};
                            }
                            socket->write(protocol::encode(reply));
                        }
                    });
            connect(socket, &QTcpSocket::disconnected,
                    socket, &QObject::deleteLater);
        }
    });
}

bool BackupServer::prepareDataRoot(const QString& path, QString* error) {
    if (path.isEmpty()) {
        *error = QStringLiteral("--data-dir is required");
        return false;
    }
    const QDir root(path);
    if (!root.isAbsolute()) {
        *error = QStringLiteral("--data-dir must be an absolute path");
        return false;
    }
    for (const QString& child : {QStringLiteral("database"),
                                 QStringLiteral("storage")}) {
        if (!QDir().mkpath(root.filePath(child))) {
            *error = QStringLiteral("Cannot create %1").arg(root.filePath(child));
            return false;
        }
    }
    return true;
}

bool BackupServer::listen(quint16 port, QString* error) {
    if (!listener_.listen(QHostAddress::LocalHost, port)) {
        *error = listener_.errorString();
        return false;
    }
    return true;
}

quint16 BackupServer::serverPort() const { return listener_.serverPort(); }

}  // namespace backup::server
