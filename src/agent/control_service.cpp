#include "backup/agent/control_service.h"
#include "backup/agent/history.h"
#include "backup/core/io.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
QString endpoint(const QString& state)
{
    const auto digest = QCryptographicHash::hash(canonicalPath(state).toUtf8(),
                                                 QCryptographicHash::Sha256)
                            .toHex()
                            .left(24);
    const auto directory = QString("/tmp/backup-agent-%1-%2")
                               .arg(::getuid())
                               .arg(QString::fromLatin1(digest));
    const auto encoded = QFile::encodeName(directory);
    require(::mkdir(encoded.constData(), 0700) == 0 || errno == EEXIST,
            "Cannot create private Agent socket directory");
    struct stat info
    {
    };
    require(::lstat(encoded.constData(), &info) == 0 && S_ISDIR(info.st_mode) &&
                info.st_uid == ::getuid() && (info.st_mode & 0077) == 0,
            "Agent socket directory must be private and owned by this user");
    return directory + "/control.sock";
}

QByteArray response(const QString& id, bool ok, const QJsonObject& value)
{
    auto result = QJsonDocument(QJsonObject{{"id", id},
                                            {"ok", ok},
                                            {ok ? "result" : "error", value}})
                      .toJson(QJsonDocument::Compact) +
                  '\n';
    if (result.size() > kAgentReplyBytes)
    {
        return response(id, false,
                        {{"message", "Response too large; use a paged query"}});
    }
    return result;
}
} // 命名空间

struct ControlService::Impl
{
    struct Request
    {
        QPointer<QLocalSocket> socket;
        QString original;
    };
    QLocalServer server;
    std::unique_ptr<QSocketNotifier> input;
    QByteArray buffer;
    QHash<QString, Request> requests;
    Command command;
    bool daemon;
    int clients = 0;

    void receive(QByteArray& pending, const QByteArray& chunk,
                 QLocalSocket* socket)
    {
        pending += chunk;
        while (pending.contains('\n'))
        {
            const auto end = pending.indexOf('\n');
            auto line = pending.left(end);
            pending.remove(0, end + 1);
            if (line.size() > kMaxJsonBytes)
            {
                if (socket)
                {
                    socket->abort();
                }
                else
                {
                    QCoreApplication::quit();
                }
                return;
            }
            if (socket)
            {
                try
                {
                    auto object = parseObject(line);
                    const auto original = text(object, "id");
                    require(requests.size() < 128, "Too many pending commands");
                    const auto id = newId();
                    requests.insert(id, {socket, original});
                    object.insert("id", id);
                    line = QJsonDocument(object).toJson(QJsonDocument::Compact);
                }
                catch (const std::exception&)
                {
                    socket->abort();
                    return;
                }
            }
            command(line);
        }
        if (pending.size() > kMaxJsonBytes)
        {
            if (socket)
            {
                socket->abort();
            }
            else
            {
                QCoreApplication::quit();
            }
        }
    }
};

ControlService::ControlService(const QString& directory, bool daemon,
                               Command command)
    : impl_(std::make_unique<Impl>())
{
    impl_->command = std::move(command);
    impl_->daemon = daemon;
    if (!daemon)
    {
        impl_->input = std::make_unique<QSocketNotifier>(STDIN_FILENO,
                                                         QSocketNotifier::Read);
        connect(impl_->input.get(), &QSocketNotifier::activated, this,
                [this]
                {
                    char chunk[8192];
                    const auto count =
                        ::read(STDIN_FILENO, chunk, sizeof(chunk));
                    if (count < 0 && errno == EINTR)
                    {
                        return;
                    }
                    if (count <= 0)
                    {
                        impl_->input->setEnabled(false);
                        QCoreApplication::quit();
                        return;
                    }
                    impl_->receive(impl_->buffer, QByteArray(chunk, count),
                                   nullptr);
                });
        return;
    }
    const auto path = endpoint(directory);
    // State 的排他锁已取得，只有当前实例可以清理上次崩溃留下的 socket。
    QLocalServer::removeServer(path);
    impl_->server.setSocketOptions(QLocalServer::UserAccessOption);
    require(impl_->server.listen(path),
            "Cannot listen on Agent control socket");
    connect(&impl_->server, &QLocalServer::newConnection, this,
            [this]
            {
                while (auto* socket = impl_->server.nextPendingConnection())
                {
                    if (impl_->clients >= 16)
                    {
                        socket->abort();
                        socket->deleteLater();
                        continue;
                    }
                    ++impl_->clients;
                    socket->setReadBufferSize(kMaxJsonBytes + 8192);
                    auto pending = std::make_shared<QByteArray>();
                    connect(socket, &QLocalSocket::readyRead, this,
                            [this, socket, pending] {
                                impl_->receive(*pending, socket->readAll(),
                                               socket);
                            });
                    connect(socket, &QLocalSocket::disconnected, this,
                            [this, socket]
                            {
                                --impl_->clients;
                                for (auto it = impl_->requests.begin();
                                     it != impl_->requests.end();)
                                {
                                    if (it->socket == socket)
                                    {
                                        it = impl_->requests.erase(it);
                                    }
                                    else
                                    {
                                        ++it;
                                    }
                                }
                                socket->deleteLater();
                            });
                }
            });
}
ControlService::~ControlService()
{
    // 先断开回调，再销毁连接，避免 Impl 成员释放时访问请求表。
    impl_->server.disconnect(this);
    for (auto* socket : impl_->server.findChildren<QLocalSocket*>())
    {
        socket->disconnect(this);
        socket->abort();
    }
}

void ControlService::reply(const QString& id, bool ok, const QJsonObject& value)
{
    if (id.isEmpty())
    {
        return;
    } // 自动任务没有等待答复的客户端。
    if (!impl_->daemon)
    {
        const auto line = response(id, ok, value);
        std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()),
                    stdout);
        std::fflush(stdout);
        return;
    }
    const auto request = impl_->requests.take(id);
    if (request.socket)
    {
        if (request.socket->bytesToWrite() > kAgentReplyBytes * 2)
        {
            request.socket->abort();
            return;
        }
        request.socket->write(response(request.original, ok, value));
    }
}

int runControlProxy(const QString& directory)
{
    const auto path = endpoint(directory);
    QLocalSocket socket;
    auto connect = [&]
    {
        socket.abort();
        socket.connectToServer(path);
        return socket.waitForConnected(100);
    };
    if (!connect())
    {
        QProcess daemon;
        daemon.setProgram(QCoreApplication::applicationFilePath());
        daemon.setArguments({"--daemon", "--state-dir", directory});
        daemon.setStandardInputFile(QProcess::nullDevice());
        daemon.setStandardOutputFile(QProcess::nullDevice());
        daemon.setStandardErrorFile(QProcess::nullDevice());
        require(daemon.startDetached(), "Cannot start background Agent");
        for (int attempt = 0; attempt < 100 && !connect(); ++attempt)
        {
            QThread::msleep(50);
        }
        require(socket.state() == QLocalSocket::ConnectedState,
                "Cannot connect to background Agent; check state "
                "lock/configuration");
    }
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &socket,
                     [&]
                     {
                         char chunk[8192];
                         const auto count =
                             ::read(STDIN_FILENO, chunk, sizeof(chunk));
                         if (count < 0 && errno == EINTR)
                         {
                             return;
                         }
                         if (count <= 0)
                         {
                             QCoreApplication::quit();
                             return;
                         }
                         if (socket.bytesToWrite() > kMaxJsonBytes * 2)
                         {
                             QCoreApplication::exit(2);
                             return;
                         }
                         socket.write(chunk, count);
                     });
    QObject::connect(&socket, &QLocalSocket::readyRead, &socket,
                     [&]
                     {
                         const auto bytes = socket.readAll();
                         std::fwrite(bytes.constData(), 1,
                                     static_cast<size_t>(bytes.size()), stdout);
                         std::fflush(stdout);
                     });
    QObject::connect(&socket, &QLocalSocket::disconnected,
                     QCoreApplication::instance(),
                     [] { QCoreApplication::exit(2); });
    return QCoreApplication::exec();
}
} // 命名空间 backup::agent
