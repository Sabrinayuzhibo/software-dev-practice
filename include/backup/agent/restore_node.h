#pragma once

#include <QByteArray>
#include <QJsonObject>

namespace backup::agent
{
enum class MetadataTiming
{
    None,
    Immediate,
    Deferred
};
struct RestoredNode
{
    const char* counter;
    MetadataTiming metadata;
};

// 创建已验证清单中的目录、软链接、FIFO、设备或历史 socket 节点。
// parent 是借用的固定目录 fd，leaf 是单个名称；不覆盖，不读取节点内容。
// 返回计数字段和元数据恢复时机；不支持的类型或创建失败抛出 core::Error。
RestoredNode restoreNode(int parent, const QByteArray& leaf,
                         const QJsonObject& entry);
} // 命名空间 backup::agent
