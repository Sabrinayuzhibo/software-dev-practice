#pragma once

#include "backup/server/remote_auth.h"
#include "backup/server/repository.h"
#include <QObject>
#include <QSet>
#include <QSslServer>

namespace backup::server
{

class BackupServer : public QObject
{
    Q_OBJECT

  public:
    explicit BackupServer(QObject* parent = nullptr);
    bool prepareDataRoot(const QString& path, QString* error);
    bool listen(const QHostAddress& address, quint16 port, QString* error);
    void enableRemote(const QString& cert, const QString& key,
                      const QString& account, int lifetimeSeconds);

    quint16 serverPort() const;

  private:
    struct Session;
    QJsonObject dispatch(Session& session, const QString& action,
                         const QJsonObject& args);
    void release(Session& session);
    QJsonObject handleRequest(const QString& action, const QJsonObject& args,
                              bool* uploading, QSet<QString>* reading);
    void acceptFrom(QTcpServer& listener);
    Repository repository_;
    std::unique_ptr<RemoteAuth> remote_;
    std::unique_ptr<QTcpServer> listener_;
};

} // namespace backup::server
