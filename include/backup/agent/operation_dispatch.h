#pragma once

#include "backup/agent/operations.h"
#include <QJsonArray>

namespace backup::agent
{
// 主线程冻结的任务输入；只在工作线程执行，引用必须覆盖本次同步调用。
struct OperationContext
{
    const QJsonObject& args;
    const QJsonObject& task;
    const QJsonObject& target;
    const QJsonArray& tasks;
    const QJsonArray& targets;
    const QString& stateDirectory;
};

// 数据操作使用同一白名单；查询命令不进入工作任务分发。
bool isDataOperation(const QString& action);
// 同步执行已冻结的数据操作，失败抛出 core::Error；不负责线程或历史落盘。
QJsonObject executeDataOperation(const QString& action,
                                 const OperationContext& context,
                                 const Progress& progress);
} // 命名空间 backup::agent
