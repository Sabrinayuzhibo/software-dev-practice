#include "backup/agent/channel.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "sealed_session.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <sys/stat.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
constexpr int kTimeoutMs = 10000;
}

Channel::Channel(const QJsonObject& target)
{
    const QString host = target.value("host").toString("127.0.0.1");
    const auto port = target.contains("port") ? number(target, "port") : 9000;
    require(host == "127.0.0.1" && port > 0 && port <= 65535,
            "Invalid local target");
    socket_.setReadBufferSize(protocol::kMaxFrameBytes + 4);
    socket_.connectToHost(host, static_cast<quint16>(port));
    const bool connected = socket_.waitForConnected(kTimeoutMs);
    require(connected,
            "Cannot connect to local Server: " + socket_.errorString());
    health_ = rawCall("ping");
    const auto expectedVault =
        target.value("repository_encryption_id").toString();
    require(expectedVault.isEmpty() ||
                health_.value("encryption").toObject().value("repository_id") ==
                    expectedVault,
            "Repository encryption identity changed");
    const auto expectedRoot = target.value("repository_path").toString();
    require(expectedRoot.isEmpty() ||
                canonicalPath(expectedRoot) ==
                    canonicalPath(text(health_, "data_root")),
            "Repository path does not match the connected Server");
}

Channel::~Channel() = default;

QJsonObject Channel::health() const
{
    return health_;
}

void Channel::authenticate()
{
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
    require(socket_.write(frame) == frame.size(), socket_.errorString());
    QElapsedTimer deadline;
    deadline.start();
    while (socket_.bytesToWrite() > 0)
    {
        require(deadline.elapsed() < kTimeoutMs &&
                    socket_.waitForBytesWritten(kTimeoutMs),
                "Server write interrupted or timed out");
    }
    while (deadline.elapsed() < kTimeoutMs)
    {
        if (socket_.bytesAvailable() == 0)
        {
            require(socket_.waitForReadyRead(
                        kTimeoutMs - static_cast<int>(deadline.elapsed())),
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
            throw ServerError(message.payload.value("reason").toString(
                "Server rejected request"));
        }
        require(message.type == (action == "ping" ? "pong" : "result"),
                "Invalid response type");
        lastResponse_.start();
        return message.payload;
    }
    throw Error("Server response timed out");
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
