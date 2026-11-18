#pragma once

// Version storage: staged bytes become visible only at directory publication.
#include "backup/core/manifest.h"
#include "backup/core/source_scope.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <memory>
#include <optional>

namespace backup::server
{
class Repository
{
  public:
    void open(const QString& root);
    QString root() const;
    QJsonObject encryption() const;
    void initializeEncryption(const QJsonObject& header);
    QJsonObject beginSealed(const QJsonObject& request);
    void appendSealed(const QJsonObject& request);
    QJsonObject commitSealed(const QJsonObject& request);
    QJsonObject readSealed(const QJsonObject& request) const;
    QString token() const;
    QJsonObject health() const;
    QJsonObject version(const QString& id) const;
    QJsonObject warnings(const QJsonObject& request) const;
    QJsonObject list(const QJsonObject& request) const;
    QJsonObject entries(const QJsonObject& request) const;
    QJsonObject download(const QJsonObject& request) const;

    // Hold versions for a reader connection until it disconnects.
    void acquireVersion(const QString& id);
    void releaseVersion(const QString& id);
    // Atomically hide a version, then reclaim one bounded batch. Idempotent
    // across retries/restarts; done is true only after durable completion.
    QJsonObject deleteVersion(const QString& id);

    // Exactly one connection owns an upload until commit or disconnect.
    QJsonObject begin(const QJsonObject& request, bool archiveImport = false);
    void addEntry(const QJsonObject& entry);
    void append(const QJsonObject& request);
    void finishFile(const QString& digest);
    void appendWarnings(const QJsonObject& request);
    QJsonObject commit(const QString& id, const QJsonArray& warnings);
    void abort();

  private:
    QString versionPath(const QString& id) const;
    void recordEntry(const QJsonObject& entry);
    QString root_;
    QString token_;
    QJsonObject encryption_;
    bool sealedUpload_ = false;
    QString activeId_;
    QString stagingPath_;
    QHash<QString, int> readers_;
    QJsonObject summary_;
    QJsonObject currentEntry_;
    core::Manifest entries_;
    QSet<QString> allowedEmptyDirectories_;
    std::optional<core::SourceScope> source_;
    QFile manifest_;
    QFile content_;
    QFile warnings_;
    QCryptographicHash warningHash_{QCryptographicHash::Sha256};
    qint64 warningCount_ = 0;
    QCryptographicHash fileHash_{QCryptographicHash::Sha256};
    QCryptographicHash storedHash_{QCryptographicHash::Sha256};
    qint64 receivedBytes_ = 0;
    bool importingArchive_ = false;
    QCryptographicHash manifestHash_{QCryptographicHash::Sha256};
    std::unique_ptr<QLockFile> lock_;
};
} // namespace backup::server
