#include "backup/agent/channel.h"
#include "backup/agent/remote_session.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "sealed_session.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSslConfiguration>
#include <sys/stat.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
constexpr int kTimeoutMs = 10000;
}

Channel::Channel(const QJsonObject& target) : target_(target)
{
    const QString host = target.value("host").toString("127.0.0.1");
    const auto port = target.contains("port") ? number(target, "port") : 9000;
    auto endpoint = target;
    endpoint.insert("host", host);
    endpoint.insert("port", port);
    validateEndpoint(endpoint);
    socket_.setReadBufferSize(protocol::kMaxFrameBytes + 4);
    if (remoteTarget(target_))
    {
        connectRemote(host, static_cast<quint16>(port));
        health_ = rawCall("ping");
        require(health_.value("remote_auth") == true,
                "Server does not support remote authentication");
        return;
    }
    socket_.connectToHost(host, static_cast<quint16>(port));
    if (!socket_.waitForConnected(kTimeoutMs))
    {
        throw ChannelError(ConnectionFailure::Network,
                           "Cannot connect to local Server: " +
                               socket_.errorString());
    }
    health_ = rawCall("ping");
    validateRepository();
}

void Channel::connectRemote(const QString& host, quint16 port)
{
    auto config = QSslConfiguration::defaultConfiguration();
    config.setProtocol(QSsl::TlsV1_2OrLater);
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    const auto ca = target_.value("ca_certificate").toString();
    if (!ca.isEmpty())
    {
        const auto certs = QSslCertificate::fromPath(
            ca, QSsl::Pem, QSslCertificate::PatternSyntax::FixedString);
        if (certs.isEmpty())
        {
            throw ChannelError(
                ConnectionFailure::Certificate,
                "CERTIFICATE_FAILED: Cannot read the selected CA file");
        }
        config.setCaCertificates(certs);
    }
    if (!QSslSocket::supportsSsl())
    {
        throw ChannelError(ConnectionFailure::Certificate,
                           "CERTIFICATE_FAILED: TLS backend is unavailable");
    }
    socket_.setSslConfiguration(config);
    socket_.connectToHost(host, port);
    if (!socket_.waitForConnected(kTimeoutMs))
    {
        throw ChannelError(ConnectionFailure::Network,
                           "Cannot connect to remote Server: " +
                               socket_.errorString());
    }
    socket_.setPeerVerifyName(host);
    socket_.startClientEncryption();
    if (!socket_.waitForEncrypted(kTimeoutMs))
    {
        if (!socket_.sslHandshakeErrors().isEmpty() ||
            socket_.error() == QAbstractSocket::SslHandshakeFailedError)
        {
            throw ChannelError(ConnectionFailure::Certificate,
                               "CERTIFICATE_FAILED: " + socket_.errorString());
        }
        throw ChannelError(ConnectionFailure::Network,
                           "Remote TLS connection interrupted: " +
                               socket_.errorString());
    }
}

void Channel::validateRepository() const
{
    const auto expectedVault =
        target_.value("repository_encryption_id").toString();
    require(expectedVault.isEmpty() ||
                health_.value("encryption").toObject().value("repository_id") ==
                    expectedVault,
            "Repository encryption identity changed");
    const auto expectedRoot = target_.value("repository_path").toString();
    const auto root = text(health_, "data_root");
    require(expectedRoot.isEmpty() ||
                (remoteTarget(target_)
                     ? expectedRoot == root
                     : canonicalPath(expectedRoot) == canonicalPath(root)),
            "Repository path does not match the connected Server");
}

Channel::~Channel() = default;

QJsonObject Channel::health() const
{
    return health_;
}

void Channel::authenticate()
{
    if (remoteTarget(target_))
    {
        RemoteSessions::resume(*this, target_);
        health_ = rawCall("ping");
        validateRepository();
        return;
    }
    const QString path = text(health_, "data_root") + "/database/access.json";
    struct stat info
    {
    };
    require(::lstat(QFile::encodeName(path).constData(), &info) == 0 &&
                S_ISREG(info.st_mode) && info.st_uid == ::getuid() &&
                (info.st_mode & 0077) == 0,
            "Local token must be owned by this user and private");
    call("authenticate", {{"token", text(readObject(path), "token")}});
}

QJsonObject Channel::rawCall(const QString& action, const QJsonObject& payload)
{
    const QString id = newId();
    const auto frame = protocol::encode({action, id, payload});
    require(!frame.isEmpty(), "Request exceeds protocol limit");
    if (socket_.write(frame) != frame.size())
    {
        throw ChannelError(ConnectionFailure::Network, socket_.errorString());
    }
    QElapsedTimer deadline;
    deadline.start();
    while (socket_.bytesToWrite() > 0)
    {
        if (deadline.elapsed() >= kTimeoutMs ||
            !socket_.waitForBytesWritten(kTimeoutMs))
        {
            throw ChannelError(ConnectionFailure::Network,
                               "Server write interrupted or timed out");
        }
    }
    while (deadline.elapsed() < kTimeoutMs)
    {
        if (socket_.bytesAvailable() == 0 &&
            !socket_.waitForReadyRead(kTimeoutMs -
                                      static_cast<int>(deadline.elapsed())))
        {
            throw ChannelError(ConnectionFailure::Network,
                               "Server disconnected or timed out");
        }
        QList<protocol::Message> messages;
        QString error;
        require(decoder_.feed(socket_.readAll(), &messages, &error), error);
        if (messages.isEmpty())
        {
            continue;
        }
        require(messages.size() == 1 && messages.first().requestId == id,
                "Mismatched server response");
        const auto& message = messages.first();
        if (message.type == "error")
        {
            const auto reason = message.payload.value("reason").toString(
                "Server rejected request");
            if (reason.startsWith("AUTH_REQUIRED:"))
            {
                throw ChannelError(ConnectionFailure::Authentication, reason);
            }
            throw ServerError(reason);
        }
        require(message.type == (action == "ping" ? "pong" : "result"),
                "Invalid response type");
        lastResponse_.start();
        return message.payload;
    }
    throw ChannelError(ConnectionFailure::Network, "Server response timed out");
}
void Channel::keepAlive()
{
    // 空文件、目录和链接可以在 Agent 内处理很久，同时须保留 Server 读取保护。
    constexpr auto kKeepAliveMs = 5000;
    if (!health_.value("encryption").toObject().isEmpty() &&
        lastResponse_.hasExpired(kKeepAliveMs))
    {
        rawCall("ping");
    }
}

void Channel::prepareCommit()
{
    if (upload_)
    {
        upload_->prepareCommit();
    }
}

QJsonObject Channel::call(const QString& action, const QJsonObject& payload)
{
    const auto header = health_.value("encryption").toObject();
    if (header.isEmpty() || action == "authenticate")
    {
        return rawCall(action, payload);
    }
    const auto access = VaultKeys::access(header);
    if (action == "health")
    {
        auto result = rawCall(action, payload);
        result.insert("repository_encryption_id",
                      header.value("repository_id"));
        result.insert("repository_locked", !access.key);
        return result;
    }
    if (action == "begin" || action == "begin_import")
    {
        upload_ = std::make_unique<SealedUpload>(*this, access, payload);
        return upload_->receipt();
    }
    keepAlive();
    if (upload_)
    {
        return upload_->dispatch(action, payload);
    }
    if (!access.key)
    {
        throw RepositoryLocked();
    }
    if (action == "versions")
    {
        auto result = rawCall(action, payload);
        QJsonArray versions;
        const auto offset =
            payload.contains("offset") ? number(payload, "offset") : 0;
        qint64 bytes = 0;
        for (const auto& value : result.value("versions").toArray())
        {
            const auto summary =
                call("version", {{"version_id", value.toObject().value("id")}});
            bytes +=
                QJsonDocument(summary).toJson(QJsonDocument::Compact).size();
            if (!versions.isEmpty() && bytes > kDetailPageBytes)
            {
                result.insert("next_offset", offset + versions.size());
                break;
            }
            versions.append(summary);
        }
        result.insert("versions", versions);
        return result;
    }
    if (action == "delete_version")
    {
        auto result = rawCall(action, payload);
        result.insert("version", SealedDownload::privateSummary(
                                     access, result.value("version").toObject())
                                     .value("summary"));
        return result;
    }
    if (action == "version" || action == "entries" || action == "download" ||
        action == "version_warnings")
    {
        const auto id = text(payload, "version_id");
        if (!download_ || version_ != id)
        {
            const auto version = rawCall("version", {{"version_id", id}});
            download_ =
                std::make_unique<SealedDownload>(*this, access, version);
            version_ = id;
        }
        return download_->dispatch(action, payload);
    }
    throw Error("Unsupported encrypted repository request");
}
} // 命名空间 backup::agent
