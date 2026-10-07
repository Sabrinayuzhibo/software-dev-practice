#pragma once

// Agent-owned configuration; save a validated replacement before changing
// memory.
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <memory>

namespace backup::agent
{
class State
{
  public:
    explicit State(const QString& directory);
    QJsonObject config() const;
    QJsonObject configure(const QString& action, const QJsonObject& args);
    QJsonObject task(const QString& id) const;
    QJsonObject target(const QString& id) const;
    void rememberTargetRoot(const QString& id, const QString& root);
    QString directory() const;
    // Return summaries with optional scan grouping/filter and a next offset.
    QJsonObject operations(const QJsonObject& args) const;
    // Publish verified shared warnings before atomically replacing a record.
    void saveOperation(const QJsonObject& operation) const;
    QJsonObject operation(const QString& id) const;
    // Page {id, offset}; invalid offsets or damaged warning data throw Error.
    QJsonObject operationWarnings(const QJsonObject& args) const;

  private:
    void save(const QJsonObject& next);
    QJsonObject addTask(const QJsonObject& args);
    QJsonObject importTasks(const QJsonArray& tasks);
    void compactOperationWarnings() const;
    QString directory_;
    QJsonObject config_;
    std::unique_ptr<QLockFile> lock_;
};
} // namespace backup::agent
