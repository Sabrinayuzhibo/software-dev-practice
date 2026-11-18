# 远端备份与实时任务恢复

Server 默认使用本机模式，监听 `127.0.0.1`。要从另一台机器备份，请在 Server 上准备 PEM 格式的证书链及其私钥。证书的 SAN 应包含客户端使用的主机名或 IP。受信第三方签发的证书可使用系统信任库，私有 CA 签发的证书需将 CA 证书交给客户端。

在 Server 主机的项目目录执行以下命令，先初始化单账户，再以 TLS 模式启动。目录及证书路径按实际部署替换。

```bash
IFS= read -rsp '远端账号密码: ' backup_remote_password
printf '\n'
printf '%s\n' "$backup_remote_password" |
    ./scripts/run-server.sh --init-remote-account backup-user --account-file /path/to/account.json
unset backup_remote_password

BACKUP_DATA_DIR=/path/to/repository ./scripts/run-server.sh \
    --listen-address 0.0.0.0 --port 9001 \
    --tls-cert /path/to/server-chain.pem --tls-key /path/to/server-key.pem \
    --account-file /path/to/account.json
```

在桌面「备份目标」选择「远端仓库（TLS）」，填写证书 SAN 对应的主机名或 IP、端口及可选 CA 证书路径，保存后点击目标行的「登录」。然后为该目标创建任务，便可备份和还原。仓库本身启用了加密时，完成远端登录后还需单独解锁仓库。

| 状态 | 触发条件与如何恢复 |
| --- | --- |
| 待连接 | Server 不可达或传输断开。保留待处理变化，继续按原间隔自动重试。 |
| 待认证 | 尚未登录、会话自然到期或 Server 重启使会话作废。重新登录后补扫整个任务范围并继续。 |
| 待解锁 | 仓库已加密且密钥未载入。输入仓库密码后补扫。 |
| 证书校验失败 | 证书不可信或主机名无法验证。检查目标主机及证书配置。 |
| 等待重试 | 来源读取、存储等其他可重试错误。 |
| 等待确认提交结果 | 连接在提交期间中断，无法确定版本是否已经发布。先核实原操作回执。 |

关闭窗口时常驻 Agent 继续使用有效会话。Server 会话默认 30 分钟，到期重新登录。可在启动时通过 `--session-seconds` 配置期限。Server 和 Agent 重启均清除各自的内存会话。

[Qt TLS 连接](https://doc.qt.io/qt-6/qsslsocket.html)验证信任链和主机身份，TLS 至少使用 1.2。密码文件以 0600 保存随机盐和 scrypt 验证值，密码只用于登录请求。Server 发放随机令牌，仅在内存保存摘要及期限，每次受保护请求检查期限。Agent 仅在内存缓存令牌。配置保存目标地址、CA 路径和必要仓库身份字段。

相关实现见[远端会话](../src/agent/remote_session.cpp)和[实时调度](../src/agent/realtime.cpp)。集成验收在本机不同地址上运行真实 Client/Server，通过 TLS 上传并还原。
