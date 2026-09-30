#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QTextStream>

#include "backup/server/backup_server.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("backup-server"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Backup Server skeleton"));
    parser.addHelpOption();
    const QCommandLineOption dataDirOption(
        QStringList{QStringLiteral("data-dir")},
        QStringLiteral("Absolute BackupSystem data directory"),
        QStringLiteral("path"));
    const QCommandLineOption portOption(
        QStringList{QStringLiteral("port")},
        QStringLiteral("TCP port; 0 lets the OS select one"),
        QStringLiteral("port"), QStringLiteral("9000"));
    parser.addOptions({dataDirOption, portOption});
    parser.process(app);

    bool ok = false;
    const uint portValue = parser.value(portOption).toUInt(&ok);
    if (!ok || portValue > 65535) {
        qCritical() << "Invalid --port";
        return 2;
    }

    backup::server::BackupServer server;
    QString error;
    if (!server.prepareDataRoot(parser.value(dataDirOption), &error)) {
        qCritical().noquote() << "DATA_DIR_ERROR" << error;
        return 2;
    }
    if (!server.listen(static_cast<quint16>(portValue), &error)) {
        qCritical().noquote() << "LISTEN_ERROR" << error;
        return 3;
    }

    QTextStream(stdout) << "LISTENING 127.0.0.1:" << server.serverPort()
                        << Qt::endl;
    return app.exec();
}
