#pragma once

#include "backup/agent/vault.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>

namespace backup::agent
{
struct SealedSection
{
    qint64 offset = 0;
    qint64 size = 0;
    QJsonObject json() const;
    static SealedSection parse(const QJsonValue& value);
};

class SealedUpload final
{
  public:
    SealedUpload(Channel& channel, VaultAccess access,
                 const QJsonObject& request);
    QJsonObject receipt() const;
    QJsonObject dispatch(const QString& action, const QJsonObject& value);
    void prepareCommit();

  private:
    void entry(const QJsonObject& value);
    void chunk(const QJsonObject& value);
    void finishFile(const QString& digest);
    void warnings(const QJsonObject& value);
    QJsonObject commit(const QJsonObject& value);

    void append(const QByteArray& data);
    void record();
    void flushFileBlocks();
    SealedSection writeLines(const QList<QByteArray>& lines);
    Channel& channel_;
    VaultAccess access_;
    std::shared_ptr<const core::Secret> versionKey_;
    QString id_;
    QString codec_;
    QJsonObject summary_;
    QJsonObject commitRequest_;
    core::Manifest manifest_;
    QList<QByteArray> entries_;
    QList<QByteArray> warnings_;
    QJsonObject file_;
    QByteArray pending_;
    SealedSection fileSection_;
    QCryptographicHash originalHash_{QCryptographicHash::Sha256};
    QCryptographicHash encodedHash_{QCryptographicHash::Sha256};
    QCryptographicHash storageHash_{QCryptographicHash::Sha256};
    qint64 originalBytes_ = 0;
    qint64 position_ = 0;
};

class SealedDownload final
{
  public:
    SealedDownload(Channel& channel, VaultAccess access, QJsonObject header);
    QJsonObject dispatch(const QString& action, const QJsonObject& request);
    static QJsonObject privateSummary(const VaultAccess& access,
                                      const QJsonObject& header);

  private:
    QJsonObject summary() const;
    QJsonObject entries(qint64 offset);
    QJsonObject warnings(qint64 offset);
    QJsonObject data(const QJsonObject& request);
    QByteArray block(SealedSection section, qint64 offset);
    QByteArray line(SealedSection section, qint64& offset);
    Channel& channel_;
    VaultAccess access_;
    std::shared_ptr<const core::Secret> versionKey_;
    QString id_;
    QJsonObject summary_;
    QByteArray cached_;
    qint64 cachedAt_ = -1;
    qint64 warningIndex_ = 0;
    qint64 warningOffset_ = 0;
    QHash<qint64, SealedSection> files_;
};

class RepositoryLocked : public core::Error
{
  public:
    RepositoryLocked() : core::Error("REPOSITORY_LOCKED: unlock the repository")
    {
    }
};
} // 命名空间 backup::agent
