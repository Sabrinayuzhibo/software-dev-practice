#pragma once

#include <QJsonObject>
#include <QString>
#include <memory>

namespace backup::core
{
enum class FilterCategory
{
    Path,
    Type,
    Name,
    Mtime,
    Size,
    User,
    Count
};

// 由 lstat 或已验证的清单提供；时间为 UTC 秒，符号链接不跟随目标。
struct FilterEntry
{
    QString path;
    QString type;
    qint64 size = 0;
    qint64 uid = 0;
    qint64 mtimeSeconds = 0;
};

// 各类规则共用的只读接口；实现不访问文件系统，可供扫描与服务端校验共用。
class IBackupFilter
{
  public:
    virtual ~IBackupFilter() = default;
    virtual FilterCategory category() const noexcept = 0;
    virtual bool matches(const FilterEntry& entry) const = 0;
};

// 校验单条规则并创建实现对象；字段、模式或区间非法时抛出 Error。
// 返回共享只读所有权，便于 SourceScope 快照安全复制。
std::shared_ptr<const IBackupFilter> makeBackupFilter(const QJsonObject& rule);
} // 命名空间 backup::core
