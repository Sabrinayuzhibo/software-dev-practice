#pragma once

#include "backup/core/encryption.h"

#include <QHash>

namespace backup::agent
{
class Channel;

struct VaultAccess
{
    QJsonObject header;
    std::shared_ptr<const core::Secret> key;
};

// 一个 Agent worker 同步处理一次请求。此作用域只借用 Agent 拥有的密钥表，
// 使同一请求中的重连、确认和归档使用相同的内存解锁状态。
class VaultKeys
{
  public:
    class Scope
    {
      public:
        explicit Scope(VaultKeys& keys);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
    static VaultAccess access(const QJsonObject& header);
    QJsonObject configure(const QString& action, const QJsonObject& args,
                          Channel& channel);

  private:
    QHash<QString, VaultAccess> unlocked_;
};

VaultAccess newVault(const QString& password);
VaultAccess unlockVault(const QJsonObject& header, const QString& password);
QByteArray vaultContext(const VaultAccess& access, const QString& version,
                        const QByteArray& purpose);
} // 命名空间 backup::agent
