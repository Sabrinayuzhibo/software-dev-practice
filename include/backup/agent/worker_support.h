#pragma once

#include "backup/core/io.h"
#include <QThread>

namespace backup::agent
{
// 仅在执行任务的工作线程调用；取消按现有失败/中断记录语义传播。
inline void checkOperationCancelled()
{
    core::require(!QThread::currentThread()->isInterruptionRequested(),
                  "Operation interrupted");
}
} // namespace backup::agent
