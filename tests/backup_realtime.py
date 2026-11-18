#!/usr/bin/env python3
"""真实常驻 Agent：自动版本、断线补偿、代理退出、重启与关闭监控。"""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from backup_integration import Fixture, stop


def eventually(check, timeout=35):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = check()
            if last:
                return last
        except (AssertionError, KeyError) as error:
            last = error
        time.sleep(0.15)
    raise AssertionError(f"condition timed out: {last}")


with tempfile.TemporaryDirectory(prefix="backup-realtime-") as temp:
    root = Path(temp)
    fixture = Fixture(root, sys.argv[1], sys.argv[2])
    daemon_pid = None
    try:
        source = root / "source"
        source.mkdir()
        (source / "a.txt").write_text("initial")
        task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
        stop(fixture.agent)

        def proxy():
            fixture.agent = subprocess.Popen(
                [
                    fixture.agent_binary,
                    "--connect",
                    "--state-dir",
                    str(root / "agent-state"),
                ],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            return fixture.request("config")["agent_pid"]

        daemon_pid = proxy()
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": True})

        def records():
            return fixture.request("operations")["operations"]

        def successes():
            return [
                r
                for r in records()
                if r["action"] == "backup" and r["state"].startswith("SUCCEEDED")
            ]

        eventually(lambda: len(successes()) == 1)
        time.sleep(2)
        assert len(successes()) == 1, "idle must not create periodic versions"
        # 连续写入只产生一个稳定版本；新增目录、重命名、删除也进入完整版本。
        (source / "nested").mkdir()
        for i in range(12):
            (source / "nested" / "new.txt").write_text(str(i))
            time.sleep(0.03)
        (source / "a.txt").rename(source / "renamed.txt")
        eventually(lambda: len(successes()) == 2)
        time.sleep(2)
        assert len(successes()) == 2
        latest = successes()[0]
        restored = root / "restored"
        fixture.restore(latest["id"], restored)
        assert (restored / "nested/new.txt").read_text() == "11"
        assert not (restored / "a.txt").exists()
        assert (restored / "renamed.txt").read_text() == "initial"
        # 关闭代理模拟 GUI 退出，Agent 必须继续运行并保存新版本。
        stop(fixture.agent)
        (source / "renamed.txt").unlink()
        (source / "offline.txt").write_text("GUI closed")
        time.sleep(4)
        assert proxy() == daemon_pid
        eventually(lambda: len(successes()) == 3)
        fixture.restore(successes()[0]["id"], root / "closed-restored")
        assert (root / "closed-restored/offline.txt").read_text() == "GUI closed"
        assert not (root / "closed-restored/renamed.txt").exists()
        # 服务停止时显示重试且不新增成功记录；恢复连接后核对完整范围。
        stop(fixture.server)
        (source / "offline.txt").write_text("server offline")
        eventually(
            lambda: fixture.request("config")["realtime_status"][task["id"]]["state"]
            == "WAITING_CONNECTION"
        )
        assert len(successes()) == 3
        fixture.start_server()
        eventually(lambda: len(successes()) == 4)
        # 崩溃后重启重新核对，即使离线期间没有留下通知。
        stop(fixture.agent)
        os.kill(daemon_pid, signal.SIGKILL)
        (source / "restart.txt").write_text("reconcile")
        time.sleep(0.2)
        previous_pid = daemon_pid
        daemon_pid = proxy()
        assert daemon_pid != previous_pid
        eventually(lambda: len(successes()) == 5)
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": False})
        time.sleep(1)
        baseline = len(successes())
        (source / "disabled.txt").write_text("manual only")
        time.sleep(3)
        assert len(successes()) == baseline
        # 内核事件队列真实溢出：暂停监听进程，在同一目录连续重命名。
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": True})
        eventually(lambda: len(successes()) > baseline)
        eventually(
            lambda: fixture.request("config")["realtime_status"][task["id"]]["state"]
            == "WATCHING"
        )
        baseline = len(successes())
        os.kill(daemon_pid, signal.SIGSTOP)
        try:
            event_limit = int(
                Path("/proc/sys/fs/inotify/max_queued_events").read_text()
            )
            assert event_limit <= 1000000, "unexpected host inotify limit"
            for _ in range(event_limit // 2 + 100):
                (source / "offline.txt").rename(source / "moving.txt")
                (source / "moving.txt").rename(source / "offline.txt")
            (source / "overflow.txt").write_text("after overflow")
        finally:
            os.kill(daemon_pid, signal.SIGCONT)
        eventually(lambda: len(successes()) > baseline)
        assert (
            fixture.request("config")["realtime_status"][task["id"]]["overflow_count"]
            > 0
        )
        eventually(lambda: not fixture.request("config")["agent_busy"])
        fixture.restore(successes()[0]["id"], root / "overflow-restored")
        assert (root / "overflow-restored/overflow.txt").read_text() == "after overflow"
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": False})
        # 配置格式不能被压缩策略保存降级。
        fixture.request(
            "save_target",
            {
                "id": "local",
                "name": "local",
                "host": "127.0.0.1",
                "port": fixture.port,
                "compression": "lz77",
            },
        )
        assert json.loads((root / "agent-state/config.json").read_text())["format"] == 8
        # 单文件原子替换需要重新绑定监听，筛选规则同样用于自动版本。
        selected = root / "selected"
        selected.mkdir()
        leaf = selected / "one.txt"
        leaf.write_text("old")
        picked = fixture.request(
            "add_task", {"sources": [str(leaf)], "target_id": "local"}
        )
        fixture.request("set_task_realtime", {"id": picked["id"], "enabled": True})

        def picked_successes():
            return [row for row in successes() if row["task_id"] == picked["id"]]

        eventually(lambda: len(picked_successes()) == 1)
        replacement = selected / "replacement"
        replacement.write_text("atomic replacement")
        replacement.replace(leaf)
        eventually(lambda: len(picked_successes()) == 2)
        fixture.restore(picked_successes()[0]["id"], root / "selected-restored")
        assert (root / "selected-restored/one.txt").read_text() == "atomic replacement"
        fixture.request("set_task_realtime", {"id": picked["id"], "enabled": False})
        fixture.request(
            "update_task_filters",
            {
                "id": task["id"],
                "filters": {
                    "include": [],
                    "exclude": [{"category": "name", "pattern": "*.tmp"}],
                },
            },
        )
        (source / "excluded.tmp").write_text("excluded")
        baseline = len(successes())
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": True})
        eventually(lambda: len(successes()) > baseline)
        eventually(lambda: not fixture.request("config")["agent_busy"])
        fixture.restore(successes()[0]["id"], root / "filtered-restored")
        assert not (root / "filtered-restored/excluded.tmp").exists()
        assert (root / "filtered-restored/overflow.txt").exists()
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": False})
        print(
            "PASS realtime: initial/burst/nested/rename/delete/restore, closed client, offline retry, restart reconciliation, disable, config format"
        )
    finally:
        fixture.close()
        if daemon_pid:
            try:
                os.kill(daemon_pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
