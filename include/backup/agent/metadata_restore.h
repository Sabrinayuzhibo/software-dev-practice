#pragma once

#include <QByteArray>
#include <QJsonObject>

namespace backup::agent
{
// entry 来自已验证的清单；旧版本缺少 mode 时跳过。
// 借用 descriptor，不取得所有权；status 需初始化 metadata_warning_count。
// 按属主、权限、mtime 顺序恢复，普通属性失败累计警告并继续。
void restoreDescriptorMetadata(int descriptor, const QJsonObject& entry,
                               QJsonObject& status);
// 借用 parent，leaf 是非空单个名称；不跟随软链接，Linux 软链接权限位跳过。
void restorePathMetadata(int parent, const QByteArray& leaf,
                         const QJsonObject& entry, QJsonObject& status);
} // 命名空间 backup::agent
