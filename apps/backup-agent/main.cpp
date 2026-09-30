#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <unistd.h>

#include "backup/agent/agent_client.h"

namespace {

constexpr qsizetype kMaxCommandBytes = 1024 * 1024;
constexpr int kSampleLimit = 100;

void reply(const QString& id, bool ok, const QJsonObject& value) {
    const QJsonObject object{{"id", id}, {"ok", ok},
                             {ok ? "result" : "error", value}};
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

void fail(const QString& id, const QString& reason) {
    reply(id, false, {{"message", reason}});
}

void scanDirectory(const QString& id, const QString& path) {
    const QFileInfo rootInfo(path);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        fail(id, QStringLiteral("目录不存在"));
        return;
    }

    const QDir root(rootInfo.absoluteFilePath());
    qint64 files = 0;
    qint64 directories = 0;
    qint64 bytes = 0;
    qint64 hashedFiles = 0;
    qint64 unreadableFiles = 0;
    QJsonArray sample;
    QDirIterator iterator(root.absolutePath(),
                          QDir::AllEntries | QDir::NoDotAndDotDot |
                              QDir::Hidden | QDir::System | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isDir()) {
            ++directories;
            continue;
        }
        if (!info.isFile()) {
            continue;
        }
        ++files;
        bytes += info.size();
        QFile file(info.absoluteFilePath());
        QString digest;
        if (file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&file)) {
                digest = QString::fromLatin1(hash.result().toHex());
                ++hashedFiles;
            } else {
                ++unreadableFiles;
            }
        } else {
            ++unreadableFiles;
        }
        if (sample.size() < kSampleLimit) {
            sample.append(QJsonObject{{"path", root.relativeFilePath(info.absoluteFilePath())},
                                      {"size", info.size()},
                                      {"sha256", digest},
                                      {"modified_at", info.lastModified().toUTC().toString(Qt::ISODate)}});
        }
    }
    reply(id, true, {{"root", root.absolutePath()},
                     {"files", files}, {"directories", directories},
                     {"bytes", bytes}, {"hashed_files", hashedFiles},
                     {"unreadable_files", unreadableFiles}, {"sample", sample}});
}

class CommandReader final : public QObject {
public:
    explicit CommandReader(QCoreApplication* app)
        : QObject(app), app_(app),
          notifier_(STDIN_FILENO, QSocketNotifier::Read, this) {
        connect(&notifier_, &QSocketNotifier::activated, this,
                [this] { readAvailable(); });
    }

private:
    void readAvailable() {
        char chunk[8192];
        const ssize_t count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count <= 0) {
            notifier_.setEnabled(false);
            app_->quit();
            return;
        }
        buffer_.append(chunk, static_cast<qsizetype>(count));
        qsizetype end = 0;
        while ((end = buffer_.indexOf('\n')) >= 0) {
            const QByteArray line = buffer_.left(end);
            buffer_.remove(0, end + 1);
            if (line.size() > kMaxCommandBytes) {
                fail(QString(), QStringLiteral("命令过长"));
            } else {
                handle(line);
            }
        }
        if (buffer_.size() > kMaxCommandBytes) {
            fail(QString(), QStringLiteral("命令过长"));
            buffer_.clear();
        }
    }

    void handle(const QByteArray& line) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QString(), QStringLiteral("无效的 JSON 命令"));
            return;
        }
        const QJsonObject command = document.object();
        const QString id = command.value("id").toString();
        const QString action = command.value("action").toString();
        const QJsonValue payload = command.value("payload");
        if (id.isEmpty() || action.isEmpty() || !payload.isObject()) {
            fail(id, QStringLiteral("命令缺少 id、action 或 payload"));
            return;
        }
        const QJsonObject args = payload.toObject();
        if (action == QStringLiteral("scan")) {
            const QString path = args.value("path").toString();
            if (path.isEmpty()) {
                fail(id, QStringLiteral("未指定目录"));
            } else {
                scanDirectory(id, path);
            }
            return;
        }
        if (action == QStringLiteral("ping")) {
            const QString host = args.value("host").toString(QStringLiteral("127.0.0.1"));
            const int port = args.value("port").toInt(9000);
            if (port < 1 || port > 65535 || host != QStringLiteral("127.0.0.1")) {
                fail(id, QStringLiteral("当前仅支持 127.0.0.1 和有效端口"));
                return;
            }
            auto* client = new backup::agent::AgentClient(this);
            auto done = std::make_shared<bool>(false);
            connect(client, &backup::agent::AgentClient::pingSucceeded, this,
                    [id, client, done](const QString& serverName) {
                        if (*done) return;
                        *done = true;
                        reply(id, true, {{"server", serverName}});
                        client->deleteLater();
                    });
            connect(client, &backup::agent::AgentClient::failed, this,
                    [id, client, done](const QString& reason) {
                        if (*done) return;
                        *done = true;
                        fail(id, reason);
                        client->deleteLater();
                    });
            QTimer::singleShot(3000, client, [id, client, done] {
                if (*done) return;
                *done = true;
                fail(id, QStringLiteral("连接超时"));
                client->deleteLater();
            });
            client->connectAndPing(host, static_cast<quint16>(port));
            return;
        }
        fail(id, QStringLiteral("不支持的命令"));
    }

    QCoreApplication* app_;
    QSocketNotifier notifier_;
    QByteArray buffer_;
};

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("backup-agent"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Backup Agent JSON-line bridge"));
    parser.addHelpOption();
    parser.process(app);
    CommandReader reader(&app);
    return app.exec();
}
