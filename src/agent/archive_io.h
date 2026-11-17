#pragma once

#include "backup/agent/operations.h"
#include <QTemporaryFile>

namespace backup::agent
{
// 路径必须为绝对路径；解析已有父目录，最终打开仍逐级禁止跟随链接。
QString archivePath(const QString& supplied);
// 将固定的普通文件复制为私有快照，检测源变化；调用者持有快照直至上传完成。
void snapshotArchive(const QString& path, QTemporaryFile& snapshot,
                     const Progress& progress);
} // 命名空间 backup::agent
