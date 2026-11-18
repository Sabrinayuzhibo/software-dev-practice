#pragma once

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>

namespace backup::server
{
class RemoteAuth final
{
  public:
    static void create(const QString& path, const QString& account,
                       const QString& password);
    RemoteAuth(const QString& path, int lifetimeSeconds);
    QJsonObject login(const QJsonObject& args);
    static QByteArray digest(const QString& token);
    bool valid(const QByteArray& session) const;

  private:
    QString account_;
    QByteArray salt_;
    QByteArray verifier_;
    int lifetimeSeconds_;
    QElapsedTimer lastAttempt_;
    QHash<QByteArray, QDeadlineTimer> sessions_;
};
} // 命名空间 backup::server
