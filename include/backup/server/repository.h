#pragma once

// Version storage: staged bytes become visible only at directory publication.
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QSet>
#include <memory>

namespace backup::server
{
class Repository
{
  public:
    void open(const QString& root);
    QString root() const;
    QString token() const;
    QJsonObject version(const QString& id) const;
    QJsonObject list(const QJsonObject& request) const;
    QJsonObject entries(const QJsonObject& request) const;
    QJsonObject download(const QJsonObject& request) const;

    // Exactly one connection owns an upload until commit or disconnect.
    QJsonObject begin(const QJsonObject& request);
    void addEntry(const QJsonObject& entry);
    void append(const QJsonObject& request);
    void finishFile(const QString& digest);
    QJsonObject commit(const QString& id, const QJsonArray& warnings);
    void abort();

  private:
    QString versionPath(const QString& id) const;
    void recordEntry(const QJsonObject& entry);
    QString root_;
    QString token_;
    QString activeId_;
    QString stagingPath_;
    QJsonObject summary_;
    QJsonObject currentEntry_;
    QSet<QString> paths_;
    QSet<QString> directories_;
    QFile manifest_;
    QFile content_;
    QCryptographicHash fileHash_{QCryptographicHash::Sha256};
    QCryptographicHash manifestHash_{QCryptographicHash::Sha256};
    qint64 files_ = 0;
    qint64 directoriesCount_ = 0;
    qint64 bytes_ = 0;
    qint64 entryCount_ = 0;
    std::unique_ptr<QLockFile> lock_;
};
} // namespace backup::server
