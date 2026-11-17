#include "backup/agent/metadata_restore.h"
#include "backup/core/io.h"

#include <QJsonArray>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
int resultCode(int result)
{
    return result == 0 ? 0 : errno;
}

// 两条系统调用路径共用字段解析和结果汇总，不引入运行时多态。
struct MetadataValues
{
    uid_t owner;
    gid_t group;
    mode_t permissions;
    timespec times[2];
};

MetadataValues metadataValues(const QJsonObject& entry)
{
    return {static_cast<uid_t>(number(entry, "uid")),
            static_cast<gid_t>(number(entry, "gid")),
            static_cast<mode_t>(number(entry, "mode")),
            {{0, UTIME_OMIT},
             {static_cast<time_t>(signedNumber(entry, "mtime_sec")),
              static_cast<long>(number(entry, "mtime_nsec"))}}};
}

void recordFailure(QJsonObject& status, const QString& path,
                   const QString& field, int error)
{
    if (error == 0)
    {
        return;
    }
    const auto count = number(status, "metadata_warning_count") + 1;
    status.insert("metadata_warning_count", count);
    auto warnings = status.value("warnings").toArray();
    warnings.append(QJsonObject{
        {"path", path},
        {"type", "metadata"},
        {"field", field},
        {"reason", field + ": " + QString::fromLocal8Bit(strerror(error))}});
    status.insert("warnings", warnings);
}
void recordFailures(QJsonObject& status, const QJsonObject& entry,
                    int ownerError, int permissionsError, int timeError)
{
    const auto path = text(entry, "path");
    recordFailure(status, path, "owner/group", ownerError);
    recordFailure(status, path, "permissions", permissionsError);
    recordFailure(status, path, "modification time", timeError);
}
} // 匿名命名空间

void restoreDescriptorMetadata(int descriptor, const QJsonObject& entry,
                               QJsonObject& status)
{
    if (!entry.contains("mode"))
    {
        return;
    }
    const auto values = metadataValues(entry);
    // 属主变更可能清除权限位；写入和属性操作完成后再恢复修改时间。
    const auto ownerError =
        resultCode(::fchown(descriptor, values.owner, values.group));
    const auto permissionsError =
        resultCode(::fchmod(descriptor, values.permissions));
    const auto timeError = resultCode(::futimens(descriptor, values.times));
    recordFailures(status, entry, ownerError, permissionsError, timeError);
}

void restorePathMetadata(int parent, const QByteArray& leaf,
                         const QJsonObject& entry, QJsonObject& status)
{
    if (!entry.contains("mode"))
    {
        return;
    }
    const auto values = metadataValues(entry);
    const auto ownerError =
        resultCode(::fchownat(parent, leaf.constData(), values.owner,
                              values.group, AT_SYMLINK_NOFOLLOW));
    const auto permissionsError =
        entry.value("type") == "symlink"
            ? 0
            : resultCode(::fchmodat(parent, leaf.constData(),
                                    values.permissions, AT_SYMLINK_NOFOLLOW));
    const auto timeError = resultCode(::utimensat(
        parent, leaf.constData(), values.times, AT_SYMLINK_NOFOLLOW));
    recordFailures(status, entry, ownerError, permissionsError, timeError);
}
} // 命名空间 backup::agent
