#pragma once

#include "backup/agent/operations.h"

namespace backup::agent
{
class ISourceSink;
// 共用筛选、类型处理和内容校验；错误处理及输出由接收者决定。
QJsonObject sourceTree(const QJsonObject& source, ISourceSink& sink,
                       const Progress& progress);
} // namespace backup::agent
