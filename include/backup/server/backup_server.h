#pragma once

#include "backup/server/repository.h"
#include <QObject>
#include <QTcpServer>

namespace backup::server
{

class BackupServer : public QObject
{
    Q_OBJECT

  public:
    explicit BackupServer(QObject* parent = nullptr);
    bool prepareDataRoot(const QString& path, QString* error);
    bool listen(quint16 port, QString* error);
    quint16 serverPort() const;

  private:
    QJsonObject handleRequest(const QString& action, const QJsonObject& args,
                              bool* uploading);
    QTcpServer listener_;
    Repository repository_;
};

} // namespace backup::server
