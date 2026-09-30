# 系统设计文档（草稿）

## 进程与数据流

Electron Renderer 使用 React + TypeScript 提供 GUI；Electron Main Process 负责目录选择、任务清单持久化和管理 C++ Agent 子进程。Main 与 Agent 使用标准输入输出上的 JSON Lines 命令/响应通信。C++ Agent 负责扫描目录、计算 SHA-256，并通过 TCP 连接 Backup Server。开发阶段都在 WSL/Linux，服务端仅监听 `127.0.0.1:9000`。服务端数据根目录由 `--data-dir` 指定，`storage/` 保存未来的备份内容，`database/` 预留元数据数据库。

## 当前协议

四字节网络序消息长度，后接 UTF-8 JSON 对象。对象包含 `type`、`request_id`、`payload`；当前只支持 `ping` / `pong` 和不支持请求的错误响应。文件传输协议、任务与版本模型仍待设计。

Main → Agent 命令为一行 UTF-8 JSON：`{"id":"...","action":"ping|scan","payload":{...}}`。Agent 返回同 ID 的 `{"id":"...","ok":true,"result":{...}}` 或 `{"id":"...","ok":false,"error":{"message":"..."}}`。扫描结果仅在界面展示，不等于已完成备份。任务清单保存在 Electron 的 userData/tasks.json。
