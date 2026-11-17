# 接口与模块职责检查

2026-11-17，按 [huawei-coding](../.agents/skills/huawei-coding/SKILL.md) 检查项目自写模块，并按实际替换需求精简抽象。当前保留三组业务虚接口；任务分发、特殊节点创建和元数据恢复使用普通函数及内部辅助结构。协议、磁盘格式、界面和功能范围沿用现有约定。

## 保留的虚接口

| 接口 | 保留依据 | 代码入口 |
| --- | --- | --- |
| `ICompressionAlgorithm` | Huffman / LZ77 有真实不同实现，共用压缩、解压和有界块封装；调用方根据策略统一调用 | [接口](../include/backup/core/compression_algorithm.h)、[调用](../src/core/compression.cpp) |
| `IBackupFilter` | Path/Type/Name/Time/Size/User 六类规则通过统一判断接口组合，支持同类或、跨类且、排除优先 | [接口](../include/backup/core/backup_filter.h)、[实现](../src/core/backup_filter.cpp) |
| `ISourceSink` | 同一遍历器需要扫描与上传两种输出/错误策略：扫描汇总失败并继续，上传经 Channel 同步发送且遇错停止 | [接口](../include/backup/agent/source_sink.h)、[实现](../src/agent/source_sink.cpp)、[装配](../src/agent/operations.cpp) |

这三组接口都具有虚析构，具体实现使用 `final`、`override`。算法与规则的工厂集中做配置到对象的映射；虚函数负责行为调用，外部边界仍负责命令白名单、字段与清单校验。

## 改用普通函数的部分

| 模块 | 当前实现与取舍 | 代码入口 |
| --- | --- | --- |
| 后台任务 | 移除 `IAgentOperation` 和四个只转调函数的实现类。`isDataOperation()` 共用数据操作白名单，`executeDataOperation()` 集中分发四种任务并保留来源/状态目录重叠检查。没有独立任务生命周期或重试实现需要多态 | [声明](../include/backup/agent/operation_dispatch.h)、[实现](../src/agent/operation_dispatch.cpp)、[主程序](../apps/backup-agent/main.cpp) |
| 特殊节点 | 移除 `IRestoreNode` 及各节点实现类。`restoreNode()` 在一个短函数中按固定类型创建目录、链接、FIFO，设备及历史 socket 的复杂逻辑用独立辅助函数承载；返回计数字段和元数据恢复时机 | [声明](../include/backup/agent/restore_node.h)、[实现](../src/agent/restore_node.cpp) |
| 元数据恢复 | 移除 `IMetadataTarget` 和两个适配类。两个公开恢复函数直接调用 fd / parent+leaf 系统调用，共用 `MetadataValues` 字段解析和完整警告汇总 | [声明](../include/backup/agent/metadata_restore.h)、[实现](../src/agent/metadata_restore.cpp) |

任务分发仍是独立模块。主程序负责冻结输入、线程生命周期、进度及结果落盘；`OperationContext` 在同步执行期间借用输入，不保存到任务对象。查询命令由原查询入口处理，未知数据操作明确失败。

`restoreTree()` 负责还原日志、目录 fd、无覆盖发布、持久化和统计。`restoreNode()` 只处理无内容流的节点；普通文件下载/解压/校验/发布、硬链接对本次已验证代表文件的引用各自保持明确流程。目录返回延后应用元数据，软链接/FIFO 返回立即应用；设备及旧 socket 保持已有范围。全部硬链接验证完成后，再从子目录到父目录恢复目录属性，最后恢复根目录。

两条元数据恢复路径均按属主/属组、权限、mtime 顺序调用系统函数，立即保存各次 errno，再逐字段汇总全部失败；普通权限失败不阻止其余属性恢复。路径调用不跟随软链接，按 Linux 语义跳过软链接 chmod。普通文件仍先刷新内容缓冲，再恢复属性、同步并发布。旧清单没有 mode 时继续跳过属性恢复。

## 保留的模块拆分与组合

`sourceTree()` 负责校验并打开来源根目录。内部 `SourceWalker` 管理一次遍历的统计、去重、父目录补齐、递归及最终一致性核验；[SourceEntries](../src/agent/source_entries.h) 负责编码、硬链接分组、逐块散列和读取前后状态核验。输出交给 `ISourceSink`，二者不依赖网络 Channel。

| 模块 | 处理依据 |
| --- | --- |
| `SourceWalker` / `SourceEntries` | 分别持有遍历与编码状态，采用具体对象组合和借用引用 |
| `ISourceSink` | Scan/Upload 接收者在 operations.cpp 栈上装配，覆盖同步遍历；上传接收者借用 Channel，不接管其所有权 |
| 警告 / 写入明细 | [usePagedDetails](../apps/backup-desktop/src/use-paged-details.ts) 共用惰性加载、分页合并、防重复请求、失败重试和卸载保护；调用处按资源 key 重新挂载，两类面板状态独立 |
| React、玻璃控件与 Electron 桥接 | 保留 props、Hook、回调及现有类型接口；Main/Preload 保持白名单 IPC 和子进程管理 |
| Descriptor、文件、临时对象 | 通过 RAII 表达资源所有权，不作为行为多态基类 |
| Manifest、SourceScope、JSON 帧、详情分页 | 数据模型、组合和校验使用具体类型与函数，类型判断、字段映射保留明确条件 |
| Repository、Channel、AgentState、RestoreJournal | 当前各有一套具体实现，保持既有存储/通信职责 |
| AgentClient、BackupServer | 沿用 Qt QObject 的虚析构、信号槽及对象线程归属 |
| 删除、提交确认、历史记录 | 保留明确状态机及不确定结果语义；重复中断检查使用 [worker_support.h](../include/backup/agent/worker_support.h)，删除的 DELETE_UNCERTAIN 处理独立保留 |

## 验证边界

[agent_module_test.cpp](../tests/agent_module_test.cpp) 通过保留的 ISourceSink 观察真实分块遍历及硬链接输出，直接测试任务分发、节点创建和拒绝覆盖。元数据改用真实 fd 与软链接检查权限、纳秒 mtime、不跟随链接，以及坏 fd 的三字段失败汇总和历史缺失字段兼容；部分权限失败、完整警告分页、权限收紧顺序由现有故障注入及真实属主集成套件验证。

实际命令和结果见[测试报告](test-report.md)。初版四组新增虚接口的历史测试记录仍保留，当前代码以上述精简结果为准。接口取舍不改变需求，不据此宣称加密、远端 TLS、物理掉电或其他平台已完成验收；后来交付的归档与本地实时备份以各自测试记录为准。

## 独立归档接入（2026-11-17）

归档使用具体 ArchiveWriter/内部解析器、VersionVerifier、普通导出/导入函数及同步回调，复用已有压缩虚接口和 UploadSourceSink；任务分发白名单新增 export_archive / import_archive。只有一个实际归档格式，不增加抽象归档基类。版本校验器同时用于普通还原，避免复制路径、元数据、筛选和清单摘要规则。实现与校验边界见[归档说明](archive.md)。

## 实时备份接入（2026-11-17）

ControlService 负责同用户 Unix socket 与控制代理，Realtime 负责监听状态、合并和重试；现有单工作线程负责执行与持久化。两者使用具体类、组合与回调，复用现有业务虚接口，未引入只有一个实现的抽象监听工厂。详见[实现说明](realtime.md)。
