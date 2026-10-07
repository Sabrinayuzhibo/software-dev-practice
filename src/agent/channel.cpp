#include "backup/agent/channel.h"
#include "backup/core/io.h"

#include <QElapsedTimer>
#include <QFileInfo>
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
    health_ = call("ping");
    const auto expectedRoot = target.value("repository_path").toString();
    require(expectedRoot.isEmpty() ||
                canonicalPath(expectedRoot) ==
                    canonicalPath(text(health_, "data_root")),
            "Repository path does not match the connected Server");
}

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

QJsonObject Channel::call(const QString& action, const QJsonObject& payload)
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
        require(message.type != "error",
                message.payload.value("reason").toString(
                    "Server rejected request"));
        require(message.type == (action == "ping" ? "pong" : "result"),
                "Invalid response type");
        return message.payload;
    }
    throw Error("Server response timed out");
}
} // namespace backup::agent
