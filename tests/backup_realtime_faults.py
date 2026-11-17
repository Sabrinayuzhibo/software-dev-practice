#!/usr/bin/env python3
"""备份期间再修改，以及不确定提交暂停、确认、恢复调度。"""

from pathlib import Path
import sys
import tempfile
import threading
import time

from backup_faults import Proxy
from backup_integration import Fixture


def eventually(check, timeout=35):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError("condition timed out")


with tempfile.TemporaryDirectory(prefix="backup-realtime-fault-") as temporary:
    root = Path(temporary)
    fixture = Fixture(root, sys.argv[1], sys.argv[2])
    proxy = None
    release = threading.Event()
    try:
        source = root / "source"
        source.mkdir()
        (source / "first.txt").write_text("first")
        task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
        paused = threading.Event()

        def pause(request, reply):
            if request["type"] == "commit" and not paused.is_set():
                paused.set()
                assert release.wait(8)
            return False

        def attach(hook):
            global proxy
            if proxy:
                proxy.close()
            proxy = Proxy(fixture.port, hook)
            fixture.request(
                "save_target",
                {
                    "id": "local",
                    "name": "fault",
                    "host": "127.0.0.1",
                    "port": proxy.listen_port,
                },
            )

        def rows():
            return fixture.request("operations")["operations"]

        def successes():
            return [
                row
                for row in rows()
                if row["state"].startswith("SUCCEEDED") and row["action"] == "backup"
            ]

        attach(pause)
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": True})
        assert paused.wait(8)
        (source / "later.txt").write_text("changed during backup")
        release.set()
        eventually(lambda: len(successes()) >= 2)
        eventually(lambda: not fixture.request("config")["agent_busy"])
        fixture.restore(successes()[0]["id"], root / "restored")
        assert (root / "restored/later.txt").read_text() == "changed during backup"
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": False})
        committed = threading.Event()
        allow = threading.Event()

        def uncertain(request, reply):
            if request["type"] == "commit":
                committed.set()
            return committed.is_set() and not allow.is_set()

        attach(uncertain)
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": True})
        eventually(
            lambda: fixture.request("config")["realtime_status"][task["id"]]["state"]
            == "WAITING_CONFIRMATION"
        )
        waiting = next(row for row in rows() if row["state"] == "WAITING")
        count = len(rows())
        (source / "after-uncertain.txt").write_text("queued")
        # 关闭再开启不能绕过提交确认，避免重复发布。
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": False})
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": True})
        time.sleep(3)
        assert len(rows()) == count
        allow.set()
        fixture.request("confirm", {"id": waiting["id"]})
        eventually(lambda: len(rows()) > count)
        eventually(
            lambda: fixture.request("config")["realtime_status"][task["id"]]["state"]
            == "WATCHING"
        )
        fixture.restore(successes()[0]["id"], root / "confirmed-restored")
        assert (root / "confirmed-restored/after-uncertain.txt").read_text() == "queued"
        print(
            "PASS: changes during backup queued; uncertain commit pauses across toggle; same operation confirmed then pending changes backed up"
        )
    finally:
        release.set()
        fixture.close()
        if proxy:
            proxy.close()
