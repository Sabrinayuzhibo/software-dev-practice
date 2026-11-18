#include "backup/server/backup_server.h"
#include "backup/core/io.h"
#include "backup/protocol/frame.h"

#include <QHostAddress>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTimer>
#include <memory>

namespace backup::server
{
struct BackupServer::Session
{
    protocol::FrameDecoder decoder;
    bool authenticated = false;
    QByteArray remoteSession;
    bool uploading = false;
    QSet<QString> reading;
};

BackupServer::BackupServer(QObject* parent)
    : QObject(parent), listener_(std::make_unique<QTcpServer>())
{
}

void BackupServer::acceptFrom(QTcpServer& listener)
{
    connect(
        &listener, &QTcpServer::pendingConnectionAvailable, this,
        [this]
        {
            while (auto* socket = listener_->nextPendingConnection())
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
                                    response.payload =
                                        dispatch(*session, message.type,
                                                 message.payload);
                                    if (message.type == "ping")
                                    {
                                        response.type = "pong";
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
                            release(*session);
                            socket->deleteLater();
                        });
            }
        });
}

void BackupServer::release(Session& session)
{
    if (session.uploading)
    {
        repository_.abort();
        session.uploading = false;
    }
    for (const auto& id : session.reading)
    {
        repository_.releaseVersion(id);
    }
    session.reading.clear();
    session.remoteSession.clear();
}

QJsonObject BackupServer::dispatch(Session& session, const QString& action,
                                   const QJsonObject& args)
{
    using core::require;
    if (remote_ && !session.remoteSession.isEmpty() &&
        !remote_->valid(session.remoteSession))
    {
        release(session);
    }
    const bool authorized =
        remote_ ? remote_->valid(session.remoteSession) : session.authenticated;
    if (action == "ping")
    {
        QJsonObject result{{"server", "Backup Server"},
                           {"protocol", 1},
                           {"remote_auth", bool(remote_)},
                           {"archive_format", 1},
                           {"sealed_protocol", 1}};
        if (!remote_ || authorized)
        {
            result.insert("data_root", repository_.root());
            result.insert("encryption", repository_.encryption());
        }
        return result;
    }
    if (remote_ && action == "remote_login")
    {
        require(!session.uploading, "Login during upload is not allowed");
        return remote_->login(args);
    }
    if (remote_ && action == "remote_resume")
    {
        require(!session.uploading,
                "Authentication during upload is not allowed");
        const auto token =
            RemoteAuth::digest(core::text(args, "session_token"));
        require(remote_->valid(token),
                "AUTH_REQUIRED: Remote session expired or invalid");
        session.remoteSession = token;
        return {};
    }
    if (!remote_ && action == "authenticate")
    {
        require(core::text(args, "token") == repository_.token(),
                "Local authentication failed");
        session.authenticated = true;
        return {};
    }
    require(authorized, "AUTH_REQUIRED: Authentication required");
    return handleRequest(action, args, &session.uploading, &session.reading);
}

QJsonObject BackupServer::handleRequest(const QString& action,
                                        const QJsonObject& args,
                                        bool* uploading, QSet<QString>* reading)
{
    if (action == "version" || action == "version_warnings" ||
        action == "entries" || action == "download" || action == "sealed_read")
    {
        const auto id = core::text(args, "version_id");
        if (!reading->contains(id))
        {
            repository_.acquireVersion(id);
            reading->insert(id);
        }
    }
    if (action == "initialize_encryption")
    {
        repository_.initializeEncryption(args);
        return repository_.encryption();
    }
    if (action == "sealed_read")
    {
        return repository_.readSealed(args);
    }
    if (action == "sealed_begin")
    {
        const auto result = repository_.beginSealed(args);
        *uploading = true;
        return result;
    }
    if (action == "sealed_commit")
    {
        core::require(*uploading, "No upload owned by this connection");
        const auto result = repository_.commitSealed(args);
        *uploading = false;
        return result;
    }
    if (action == "sealed_chunk")
    {
        core::require(*uploading, "No upload owned by this connection");
        repository_.appendSealed(args);
        return {};
    }
    if (action == "delete_version")
    {
        return repository_.deleteVersion(core::text(args, "version_id"));
    }
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
    if (action == "begin" || action == "begin_import")
    {
        const auto result = repository_.begin(args, action == "begin_import");
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

void BackupServer::enableRemote(const QString& cert, const QString& key,
                                const QString& account, int lifetimeSeconds)
{
    using core::require;
    require(QSslSocket::supportsSsl(), "TLS backend is unavailable");
    const auto chain = QSslCertificate::fromPath(
        cert, QSsl::Pem, QSslCertificate::PatternSyntax::FixedString);
    QFile file(key);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 64 * 1024,
            "Cannot read TLS private key");
    const auto pem = file.readAll();
    QSslKey privateKey(pem, QSsl::Rsa);
    if (privateKey.isNull())
    {
        privateKey = QSslKey(pem, QSsl::Ec);
    }
    require(!chain.isEmpty() && !privateKey.isNull(),
            "Invalid TLS certificate or private key");
    auto config = QSslConfiguration::defaultConfiguration();
    config.setLocalCertificateChain(chain);
    config.setPrivateKey(privateKey);
    config.setProtocol(QSsl::TlsV1_2OrLater);
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    auto listener = std::make_unique<QSslServer>();
    listener->setSslConfiguration(config);
    listener->setHandshakeTimeout(10000);
    remote_ = std::make_unique<RemoteAuth>(account, lifetimeSeconds);
    listener_ = std::move(listener);
}

bool BackupServer::listen(const QHostAddress& address, quint16 port,
                          QString* error)
{
    if (address != QHostAddress::LocalHost && !remote_)
    {
        *error = "Network listening requires TLS and a remote account";
        return false;
    }
    acceptFrom(*listener_);
    if (!listener_->listen(address, port))
    {
        *error = listener_->errorString();
        return false;
    }
    return true;
}

quint16 BackupServer::serverPort() const
{
    return listener_->serverPort();
}
} // 命名空间 backup::server
