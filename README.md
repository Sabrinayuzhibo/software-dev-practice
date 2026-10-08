# Backup System

软件开发综合实验项目。支持 Linux 本地单个文件、多个文件、文件夹以及混合来源的备份与还原，并已实现 FR-10 文件类型支持、扫描预览、历史版本和持久化执行记录。Electron + React 提供操作界面，C++ Agent 读写源文件和还原目录，独立 C++ Server 保存备份。

编码规范见项目 [Huawei coding skill](.agents/skills/huawei-coding/SKILL.md)，依据 `ref/软件编程规范-华为.doc` 整理，由 [AGENTS.md](AGENTS.md) 关联；C++ 使用 `.clang-format`，TypeScript 使用 `.prettierrc.json`。

## 进程与目录

```text
Electron Renderer (React + TypeScript)
        │ 受限 IPC
Electron Main Process (TypeScript：目录选择、受限 IPC、Agent 启动)
        │ JSON Lines / 标准输入输出
C++ Backup Agent (Qt：任务/记录、工作线程、扫描、上传、还原)
        │ 本地认证、带长度的 JSON 帧、256 KiB 内容块
C++ Backup Server (Qt：校验、暂存、版本发布、读取)
        │
../BackupSystem/{database,storage}
```

Electron 源码在 `apps/backup-desktop/`，Agent 在 `apps/backup-agent/`，Server 在 `apps/backup-server/`。服务端运行数据默认位于源码项目旁的 `BackupSystem/`。Agent 配置与执行记录位于 Electron 的 `userData/agent/`，可用 `BACKUP_AGENT_STATE_DIR` 指定；原有 `tasks.json` 首次启动时导入并保留原文件。迁移冲突或配置损坏会报告错误，不能静默重置。基础版使用原子保存的 JSON 配置和版本清单，尚未采用设计文档中的 SQLite。

## 环境与启动

需要 Node.js 22.12+、npm、Meson 1.3+、Ninja、G++、Python 3 和 Qt 6 Core/Network 开发包。Electron 在 Ubuntu 24.04 还需要 `libnspr4`、`libnss3`、`libasound2t64` 等图形运行库；WSL 中还需要可用的 WSLg 图形环境，默认使用 Wayland。当前机器已有用户级 Qt、Electron 缺失的运行库和中文字体；脚本会自动加载它们。

第一次运行：

```bash
./scripts/build.sh
```

在两个终端分别启动服务端和界面：

```bash
./scripts/run-server.sh
./scripts/run-agent.sh
```

`run-agent.sh` 会启动 Electron 界面及其 C++ Agent 子进程；**不会自动启动 Backup Server**。服务端未运行时，界面会显示连接失败，仍可添加并扫描本地目录。服务端只监听 `127.0.0.1:9000`。运行脚本可接受额外 Electron 参数。

进入“备份任务”并点击“新建任务”，可多次添加文件或文件夹、移除误选项、填写可选名称及选择目标，核对还原路径后创建。文件类型默认选择普通文件（含硬链接成员）、软链接和 FIFO，至少选一种；字符设备、块设备和 socket 需显式勾选，也可输入节点绝对路径直接选择。文件夹始终递归查找匹配内容，目录层级自动重建。“保留空目录”默认开启，关闭后仅保留匹配内容的父目录。直接选中的条目须逐项符合规则才能创建任务。相同路径和已被选中文件夹包含的条目会合并。单个文件按原文件名还原；单个文件夹仍将其内容直接还原到目标；多个来源按共同父目录保留相对层级，不平铺同名文件。每个任务最多显式选择 100 项，选中文件夹内的后代不占此上限；选择范围的序列化记录最多 16 KiB，超出时明确提示，可拆分任务。

扫描进度、预览和警告显示在所属任务卡片内，同一窗口会分别保留各任务的预览，文件明细可独立展开或收起；预览支持直接备份该任务，SHA-256 默认缩略、展开可查看完整值。备份准备、进度及结果也保留在所属任务卡片，切页后返回仍可查看；备份完成后可直接进入对应任务的版本列表。任务、版本与还原确认均可展开所选来源及还原路径。移除任务需确认，源文件和历史版本保留。“执行记录”点击“查看”后显示所选执行的进度、错误、警告和中断结果，不随后台执行自动展开；“备份目标”不显示执行面板。还原目标必须是新建或空目录，且不能与实际选定来源、仓库或 Agent 状态目录重叠。备份完成前版本不会出现在列表中。

执行记录默认按任务折叠扫描历史，显示最新状态、扫描次数、异常及有警告的次数，展开后分页查看每次执行；备份与还原分别展示。列表及扫描进度只读取摘要，点击“查看”才打开执行详情，展开警告后分页加载路径和原因。记录与警告每页最多 100 项，并限制每页内容大小，重复扫描不会把所有历史警告一次传给界面。

Agent 将相同的警告明细按内容指纹共用保存在 `userData/agent/warnings/*.jsonl`，各次执行保留独立的时间、状态、统计和扫描样本。启动时自动整理旧记录：共享明细先持久保存并校验，再原子更新记录引用；整理失败保留旧记录并继续兼容读取。共享警告文件不自动回收。查询响应过大时只报告该次请求失败，不终止 Agent 或正在进行的扫描。

使用其他本地仓库或端口：`BACKUP_DATA_DIR=/absolute/backup ./scripts/run-server.sh --port 9001`，然后在“备份目标”中配置名称及端口。每个 Server 管理一个仓库，并生成仅所属用户可读的本地访问令牌。把仓库放在与源文件不同的磁盘才能抵御源磁盘损坏。

客户端使用自定义标题栏：按住顶部深色区域可拖动窗口，右侧按钮分别用于最小化、最大化或还原、关闭。

WSLg 启动时优先使用原生 Wayland，避开 XWayland 对无边框窗口最大化和鼠标坐标的处理问题；没有 Wayland socket 时才使用 X11。需要诊断 X11 时可运行 `BACKUP_ELECTRON_PLATFORM=x11 ./scripts/run-agent.sh`。

WSLg 中没有 `/dev/dri/renderD*` 时，脚本在现有软件渲染模式下传入空的 `--render-node-override=`，避免 Chromium 探测不存在的 DRM 设备；不屏蔽其他错误日志。最小化期间继续更新任务状态，恢复窗口时同步焦点、最大化状态并请求重绘。修改启动参数和主进程代码后，需要关闭旧窗口并重新运行启动脚本。

若终端显示 `built` 后看不到窗口，先区分编译完成和窗口显示：单独运行 `build/backup-agent` 没有图形窗口，`run-agent.sh` 才会同时启动 Electron。若 Electron、Renderer 与 Agent 均在运行，仍看不到窗口，可检查 `/mnt/wslg/weston.log`。出现 `rdp_allocate_shared_memory` 的 `Input/output error` 及窗口标题 `[WARN:COPY MODE]` 时，WSLg 已回退到图像复制模式，应检查宿主显示链路，不能只靠 Electron 的 `isVisible()` 或页面截图判断窗口正常。可在保存编辑内容、等待备份/还原结束并停止服务后，在 **Windows PowerShell** 执行 `wsl --shutdown`，再重新打开 WSL 并分别运行两个启动脚本验证。该命令会停止所有 WSL 发行版及其中的服务，也会断开 VS Code 的 WSL 会话；不由应用自动执行。

窗口回归检查：`npm run test:window --prefix apps/backup-desktop`。它使用独立测试仓库和临时配置，检查窗口鼠标输入、目标配置、添加目录、真实扫描/备份/还原、确认对话框、执行记录、重载持久化及连接无响应时的交互。目录选择器由测试提供临时路径，因此不覆盖原生文件对话框；截图保存到 `/tmp/backup-desktop-tasks.png`、`/tmp/backup-desktop-narrow.png` 和 `/tmp/backup-desktop-restore.png`。

通信边界回归：构建后运行 `npm run test:bridge --prefix apps/backup-desktop`，检查超长 UTF-8 响应、分段响应、合并响应和超时后的迟到响应，验证后续查询及后台工作继续运行。核心测试中的 `operation-history` 覆盖旧记录整理、警告去重、分页、损坏明细和重启。

WSLg 环境还可运行 `BACKUP_TEST_WINDOWS_INPUT=1 npm run test:window --prefix apps/backup-desktop`。它通过 `powershell.exe` 对唯一命名的测试窗口执行 Windows 窗口恢复及鼠标事件，覆盖宿主系统到 WSLg 的输入路径；会短暂移动鼠标并切换测试窗口焦点，运行期间请避免操作测试窗口。这仍不等同于物理鼠标或人工任务栏点击测试。

首次启动若缺少 Electron 运行时，脚本会下载对应的 Linux x64 版本并校验 SHA-256；这与 npm 安装的前端依赖是两步。

手动构建 C++：`source scripts/qt-env.sh && meson setup build --backend=ninja && ninja -C build && meson test -C build --print-errorlogs`。手动构建界面：`npm ci --prefix apps/backup-desktop && npm run build --prefix apps/backup-desktop`。C++ 构建使用 Meson + Ninja；前端 TypeScript/React 使用 npm + Vite。

## 当前边界

保存普通文件、隐藏文件、空文件及完整目录层级，也支持软链接、硬链接和 FIFO。软链接保存原始目标文本，不跟随目标，悬空或外部目标也保留；同一备份内共享设备号/inode 的普通文件只上传、保存一份内容，还原后重建组内硬链接；FIFO 只记录和重建节点，不读取流。字符设备、块设备和 socket 默认警告跳过；显式选择后，设备仅保存节点类型与主/次设备号，不读取设备内容，socket 仅保存路径节点，不保存连接或服务状态。还原需要相应权限与文件系统支持，不自动提权；重建 socket 节点不能恢复监听进程，服务重新绑定前可能需要删除该节点。

选定文件不会扫描同目录的其他文件。文件类型与空目录规则同时用于扫描和实际备份；文件夹总会递归遍历子目录，关闭空目录保留时仅保存入选条目的必要父目录。完整扫描若无匹配文件会明确提示，仍可创建任务，以便后续新增文件进入范围。每次执行和版本保存当时的选择范围与规则；任务根被替换成软链接、所选条目缺失或类型改变时明确失败，不扩大备份范围。文件夹内新建的匹配条目仍属于下次备份范围。旧目录任务和旧类型筛选任务继续使用原来的路径、标识与规则；首次保存文件或多项来源任务时，Agent 配置原子升级到格式 2，旧类型筛选任务使用格式 3，空目录规则任务升级到格式 4，含设备或 socket 类型的任务升级到格式 5，以防旧 Agent 忽略规则。客户端与 Server 需同时更新并重启。

扫描预览包含条目类型、软链接目标、硬链接同组文件及 SHA-256，最多展示 100 项、64 KiB；统计覆盖整个范围，不可读条目汇总为问题并标记扫描不完整。文件数和逻辑大小包含硬链接的每个路径；“内容存储”只累计实际保存的文件内容，不包含清单、警告及文件系统开销，也不代表跨版本去重。还原不跟随软链接写出目标目录，硬链接只连接本次版本中已经校验的文件；跨文件系统或不支持该类型时明确失败，不自动改为复制。权限、属主、时间等元数据恢复仍属于 FR-12；网络远端、实时备份、FR-11 的模式/时间/大小等条件筛选、导入导出、压缩、加密和自动回收尚未实现。

目标可配置名称、本机端口和可选仓库路径；填写路径时会校验它与所连接 Server 的仓库是否一致，不迁移 Server 数据。实际仓库位置仍由 `--data-dir` / `BACKUP_DATA_DIR` 设置。连接检查区分服务未连接、存储不可用和可用；存储检查实际读取、写入并同步临时文件。执行中可以保存目标设置，当前操作继续使用开始时的配置，下次执行使用新配置。

版本列表只返回摘要、保存的范围及警告数量，警告按需分页；新版本的警告保存在校验过的 `warnings.jsonl`，旧版本的内嵌警告仍可读取。还原写入明细持久保存在 Agent 的 `restores/<id>.jsonl`，失败或重启后可在执行详情分页查看；中断时无法确认的条目标为待核对，并列出可能残留的临时文件。连续三次读取执行状态失败后，界面显示失联、暂停自动重试并保留操作锁，可手动重新查询或在窗口重新获得焦点时尝试恢复。

文件按块读取并校验；发现读取期间变化时备份失败，不提供原子文件系统快照。每次版本独立保存文件内容，组内硬链接共用内容；失败暂存保留在 `storage/staging/`，不进入可还原列表，也不自动删除旧版本腾出空间。关闭 GUI 会停止当前 Agent；重新打开后未完成记录显示中断，提交结果不确定的备份可在记录中再次确认。新备份使用格式 3，继续读取格式 1、2 的历史版本，不改写历史数据；旧版本中未保存的链接或节点信息无法补回。需同时更新并重启客户端与 Server，旧程序不能读取格式 3。

`./scripts/build.sh` 执行 C++ 单元/集成测试与前端构建；其中大文件测试使用临时目录，比较 64 MiB 与 1 GiB 备份/还原及进程峰值内存，需要约 4 GiB 可用临时空间。实际验证记录见 [测试报告](docs/test-report.md)。

`backup-p0-boundaries` 覆盖反复备份大量警告、存储权限、配置快照及还原中断；`backup-disk-full` 在独立 user/mount namespace 内挂载限容 tmpfs，验证真实 ENOSPC，不写满宿主磁盘。不支持非特权挂载的环境会明确跳过该套件；物理断电仍需单独验收。本次更新后需同时重启客户端与 Server 以加载新接口，已有配置和版本继续保留。

`backup-file-types` 验证链接原始字节、inode 关系、内容只存一份、FIFO、旧版本兼容、恶意清单、路径替换及失败日志；测试用动态库只注入指定创建操作的“不支持”错误，不进入产品代码。`backup-file-type-mounts` 在私有挂载空间中通过只读、nodev 的设备节点绑定检查类型和编号，并用嵌套 tmpfs 验证真实跨设备硬链接失败；还覆盖显式选择设备/socket 后的节点保存及权限不足反馈，不读取设备内容。窗口回归也覆盖类型预览、版本统计、还原与写入明细，截图为 `/tmp/backup-types-*.png`。

`backup-sources` 验证单文件、多文件、多目录和混合来源、选择去重、同名路径、范围隔离、所选内容变化、特殊类型及配置/版本持久化。窗口回归包含新建任务、原生选择取消、移除、错误恢复、长名称与真实文件还原；选择器仍由测试提供路径，截图为 `/tmp/backup-sources-*.png`。

`backup-type-filter` 验证文件夹按类型筛选、空目录开关、直接选中文件逐项检查、空结果版本、Server 拒绝越界条目及历史规则持久化。窗口回归覆盖类型复选框、无匹配提示、错误恢复和实际扫描/备份/还原，截图为 `/tmp/backup-type-filter-*.png`。

课程资料位于 `ref/`。目标文档见 [需求分析说明书](docs/requirements.md)、[系统设计文档](docs/design.md)，可编辑 UML 和 SVG 见 [文档图说明](docs/diagrams/README.md)。文档中的 P1/P2 和完整架构图包含尚未实现的后续目标。
