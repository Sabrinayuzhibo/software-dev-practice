#pragma once

#include <QObject>
#include <QTcpServer>

namespace backup::server {

class BackupServer : public QObject {
    Q_OBJECT

public:
    explicit BackupServer(QObject* parent = nullptr);
    bool prepareDataRoot(const QString& path, QString* error);
    bool listen(quint16 port, QString* error);
    quint16 serverPort() const;

private:
    QTcpServer listener_;
};

}  // namespace backup::server
