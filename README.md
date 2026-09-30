# Backup System

软件开发综合实验项目。当前完成了 Electron + React 客户端、C++ Backup Agent 和独立 C++ Backup Server 的连通骨架。客户端可以保存待备份目录任务、检查服务端状态，并调用 C++ Agent 扫描目录和计算 SHA-256。**文件上传、备份版本、恢复、增量判断、压缩和文件监控尚未实现。**

## 进程与目录

```text
Electron Renderer (React + TypeScript)
        │ 受限 IPC
Electron Main Process (TypeScript：目录选择、任务记录、Agent 启动)
        │ JSON Lines / 标准输入输出
C++ Backup Agent (Qt Core/Network：扫描、哈希、连接)
        │ 带长度的 TCP JSON 帧
C++ Backup Server (Qt Core/Network：当前支持 ping/pong)
        │
../BackupSystem/{database,storage}
```

Electron 源码在 `apps/backup-desktop/`，Agent 在 `apps/backup-agent/`，Server 在 `apps/backup-server/`。运行数据位于源码项目旁的 `BackupSystem/`。Electron 的目录任务记录在其本机 userData 目录的 `tasks.json`，与服务端备份数据分开；服务端尚不会生成 `metadata.db`。

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

客户端使用自定义标题栏：按住顶部深色区域可拖动窗口，右侧按钮分别用于最小化、最大化或还原、关闭。

WSLg 启动时优先使用原生 Wayland，避开 XWayland 对无边框窗口最大化和鼠标坐标的处理问题；没有 Wayland socket 时才使用 X11。需要诊断 X11 时可运行 `BACKUP_ELECTRON_PLATFORM=x11 ./scripts/run-agent.sh`。

窗口回归检查：`npm run test:window --prefix apps/backup-desktop`。它会在当前图形会话中打开一个独立测试窗口，检查多次最大化/还原后的鼠标输入、服务端检查、目录选择入口、最小化和关闭，再退出。测试窗口使用临时配置目录；原生目录选择器在此测试中返回取消，不会改动实际任务。

首次启动若缺少 Electron 运行时，脚本会下载对应的 Linux x64 版本并校验 SHA-256；这与 npm 安装的前端依赖是两步。

手动构建 C++：`source scripts/qt-env.sh && meson setup build --backend=ninja && ninja -C build && meson test -C build --print-errorlogs`。手动构建界面：`npm ci --prefix apps/backup-desktop && npm run build --prefix apps/backup-desktop`。C++ 构建使用 Meson + Ninja；前端 TypeScript/React 使用 npm + Vite。

## 当前边界

“添加目录”只保存任务路径；“扫描目录”读取目录里的普通文件并计算 SHA-256，不上传文件。扫描结果最多展示 100 个文件，符号链接不会跟随。文件读取失败会显示数量。完整备份和恢复需要后续扩展协议、服务端存储及版本管理。

课程 PDF 位于 `ref/`，需求与设计草稿位于 `docs/`。
