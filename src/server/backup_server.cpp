#include "backup/server/backup_server.h"
#include "backup/core/io.h"
#include "backup/protocol/frame.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>
#include <memory>

namespace backup::server
{
namespace
{
struct Session
{
    protocol::FrameDecoder decoder;
    bool authenticated = false;
    bool uploading = false;
};
} // namespace

BackupServer::BackupServer(QObject* parent) : QObject(parent)
{
    connect(
        &listener_, &QTcpServer::newConnection, this,
        [this]
        {
            while (auto* socket = listener_.nextPendingConnection())
            {
                auto session = std::make_shared<Session>();
                socket->setReadBufferSize(protocol::kMaxFrameBytes + 4);
                auto* timeout = new QTimer(socket);
                timeout->setSingleShot(true);
                timeout->start(30000);
                connect(timeout, &QTimer::timeout, socket, &QTcpSocket::abort);
                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket, session, timeout]
                        {
                            timeout->start();
                            QList<protocol::Message> messages;
                            QString error;
                            if (!session->decoder.feed(socket->readAll(),
                                                       &messages, &error))
                            {
                                socket->abort();
                                return;
                            }
                            for (const auto& message : messages)
                            {
                                protocol::Message response{
                                    "result", message.requestId, {}};
                                try
                                {
                                    const auto& args = message.payload;
                                    const auto& action = message.type;
                                    if (action == "ping")
                                    {
                                        response.type = "pong";
                                        response.payload = {
                                            {"server", "Backup Server"},
                                            {"data_root", repository_.root()},
                                            {"protocol", 1}};
                                    }
                                    else if (action == "authenticate")
                                    {
                                        core::require(
                                            core::text(args, "token") ==
                                                repository_.token(),
                                            "Local authentication failed");
                                        session->authenticated = true;
                                    }
                                    else
                                    {
                                        core::require(
                                            session->authenticated,
                                            "Authentication required");
                                        response.payload = handleRequest(
                                            action, args, &session->uploading);
                                    }
                                }
                                catch (const std::exception& failure)
                                {
                                    if (session->uploading)
                                    {
                                        repository_.abort();
                                        session->uploading = false;
                                    }
                                    response.type = "error";
                                    response.payload = {
                                        {"reason",
                                         QString::fromUtf8(failure.what())}};
                                }
                                const auto frame = protocol::encode(response);
                                if (frame.isEmpty() ||
                                    socket->bytesToWrite() >
                                        protocol::kMaxFrameBytes ||
                                    socket->write(frame) != frame.size())
                                {
                                    socket->abort();
                                    return;
                                }
                            }
                        });
                connect(socket, &QTcpSocket::disconnected, socket,
                        [this, session, socket]
                        {
                            if (session->uploading)
                            {
                                repository_.abort();
                            }
                            socket->deleteLater();
                        });
            }
        });
}

QJsonObject BackupServer::handleRequest(const QString& action,
                                        const QJsonObject& args,
                                        bool* uploading)
{
    if (action == "health")
    {
        return repository_.health();
    }
    if (action == "version_warnings")
    {
        return repository_.warnings(args);
    }
    if (action == "versions")
    {
        return repository_.list(args);
    }
    if (action == "version")
    {
        return repository_.version(core::text(args, "version_id"));
    }
    if (action == "entries")
    {
        return repository_.entries(args);
    }
    if (action == "download")
    {
        return repository_.download(args);
    }
    if (action == "begin")
    {
        const auto result = repository_.begin(args);
        *uploading = true;
        return result;
    }
    if (action == "commit")
    {
        const QString id = core::text(args, "operation_id");
        if (!*uploading)
        {
            return repository_.version(id);
        }
        const auto result =
            repository_.commit(id, args.value("warnings").toArray());
        *uploading = false;
        return result;
    }
    core::require(*uploading, "No upload owned by this connection");
    if (action == "entry")
    {
        repository_.addEntry(args);
    }
    else if (action == "chunk")
    {
        repository_.append(args);
    }
    else if (action == "finish_file")
    {
        repository_.finishFile(core::text(args, "sha256"));
    }
    else if (action == "warnings")
    {
        repository_.appendWarnings(args);
    }
    else if (action == "abort")
    {
        repository_.abort();
        *uploading = false;
    }
    else
    {
        throw core::Error("Unsupported request");
    }
    return {};
}

bool BackupServer::prepareDataRoot(const QString& path, QString* error)
{
    try
    {
        repository_.open(path);
        return true;
    }
    catch (const std::exception& failure)
    {
        *error = QString::fromUtf8(failure.what());
        return false;
    }
}

bool BackupServer::listen(quint16 port, QString* error)
{
    if (!listener_.listen(QHostAddress::LocalHost, port))
    {
        *error = listener_.errorString();
        return false;
    }
    return true;
}

quint16 BackupServer::serverPort() const
{
    return listener_.serverPort();
}
} // namespace backup::server
