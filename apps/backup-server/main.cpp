#include "backup/core/io.h"
#include "backup/server/backup_server.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QTextStream>
#include <unistd.h>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("backup-server");

    QCommandLineParser parser;
    parser.setApplicationDescription("Backup Server");
    parser.addHelpOption();
    parser.addOptions({
        {"data-dir", "Absolute BackupSystem data directory", "path"},
        {"port", "TCP port; 0 lets the OS select one", "port", "9000"},
        {"listen-address", "Bind address; nonloopback requires TLS", "address",
         "127.0.0.1"},
        {"tls-cert", "PEM server certificate chain", "path"},
        {"tls-key", "PEM server private key", "path"},
        {"account-file", "Private remote account verifier", "path"},
        {"session-seconds", "Remote session lifetime", "seconds", "1800"},
        {"init-remote-account",
         "Initialize account file; read password from stdin", "name"},
    });
    parser.process(app);

    using namespace backup;
    try
    {
        if (parser.isSet("init-remote-account"))
        {
            core::require(parser.isSet("account-file"),
                          "--account-file is required");
            QFile input;
            core::require(input.open(STDIN_FILENO, QIODevice::ReadOnly),
                          "Cannot read account password");
            auto password = input.readLine(1026);
            if (password.endsWith('\n'))
            {
                password.chop(1);
            }
            server::RemoteAuth::create(parser.value("account-file"),
                                       parser.value("init-remote-account"),
                                       QString::fromUtf8(password));
            return 0;
        }
        bool ok = false;
        const auto port = parser.value("port").toUInt(&ok);
        core::require(ok && port <= 65535, "Invalid --port");
        const QHostAddress address(parser.value("listen-address"));
        core::require(!address.isNull(), "Invalid --listen-address");
        server::BackupServer service;
        if (parser.isSet("tls-cert") || parser.isSet("tls-key") ||
            parser.isSet("account-file"))
        {
            core::require(
                parser.isSet("tls-cert") && parser.isSet("tls-key") &&
                    parser.isSet("account-file"),
                "TLS requires --tls-cert, --tls-key and --account-file");
            const auto seconds = parser.value("session-seconds").toInt(&ok);
            core::require(ok && seconds > 0 && seconds <= 86400,
                          "Invalid --session-seconds");
            service.enableRemote(parser.value("tls-cert"),
                                 parser.value("tls-key"),
                                 parser.value("account-file"), seconds);
        }
        QString error;
        if (!service.prepareDataRoot(parser.value("data-dir"), &error))
        {
            qCritical().noquote() << "DATA_DIR_ERROR" << error;
            return 2;
        }
        if (!service.listen(address, static_cast<quint16>(port), &error))
        {
            qCritical().noquote() << "LISTEN_ERROR" << error;
            return 3;
        }
        QTextStream(stdout) << "LISTENING " << address.toString() << ':'
                            << service.serverPort() << Qt::endl;
        return app.exec();
    }
    catch (const std::exception& error)
    {
        qCritical().noquote() << error.what();
        return 2;
    }
}
