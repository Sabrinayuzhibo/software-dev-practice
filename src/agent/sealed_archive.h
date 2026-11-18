#pragma once

#include "backup/agent/vault.h"
#include <QFile>

namespace backup::agent
{
// 内层仍为 BSAR1。外层头绑定随机归档 ID，每帧验证顺序及长度，终帧认证 EOF。
class SealedArchiveOutput final : public QIODevice
{
  public:
    SealedArchiveOutput(QFile& file, VaultAccess access);
    void finish();

  protected:
    bool isSequential() const override;
    qint64 readData(char*, qint64) override;
    qint64 writeData(const char* data, qint64 size) override;

  private:
    void frame(QByteArrayView bytes);
    QFile& file_;
    VaultAccess access_;
    QByteArray context_;
    QByteArray pending_;
    quint64 sequence_ = 0;
};

class SealedArchiveInput final : public QIODevice
{
  public:
    SealedArchiveInput(QFile& file, const QString& password,
                       const VaultAccess& existing = {});
    static bool detects(QFile& file);

  protected:
    bool isSequential() const override;
    qint64 readData(char* output, qint64 size) override;
    qint64 writeData(const char*, qint64) override;

  private:
    void frame();
    QFile& file_;
    VaultAccess access_;
    QByteArray context_;
    QByteArray pending_;
    qsizetype position_ = 0;
    quint64 sequence_ = 0;
    bool finished_ = false;
};
} // 命名空间 backup::agent
