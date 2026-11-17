#pragma once

#include "backup/core/manifest.h"
#include "backup/core/source_scope.h"
#include <QCryptographicHash>

namespace backup::core
{
// 校验已提交版本的清单及选择范围；逐条接收，不缓存文件内容。
class VersionVerifier
{
  public:
    explicit VersionVerifier(const QJsonObject& summary);
    // 返回参与清单摘要的规范 JSONL；非法类型、路径或属性抛出 Error。
    QByteArray append(const QJsonObject& entry);
    void finish();

  private:
    QJsonObject summary_;
    SourceScope scope_;
    Manifest manifest_;
    QSet<QString> allowedEmptyDirectories_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
};
} // 命名空间 backup::core
