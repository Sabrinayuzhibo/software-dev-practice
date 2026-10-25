#!/usr/bin/env python3
"""Delete isolated versions through real Agent/Server and faulted RPC replies."""

import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading

from backup_faults import Proxy
from backup_integration import Fixture, rpc, stop, tree


def connection(fixture, authenticate=True):
    client = socket.create_connection(("127.0.0.1", fixture.port), timeout=5)
    if authenticate:
        token = json.loads((fixture.root / "repository/database/access.json").read_text())["token"]
        rpc(client, "authenticate", {"token": token})
    return client


def delete(fixture, identifier, success=True, **payload):
    started = fixture.request("start_delete_version", {
        "target_id": "local", "version_id": identifier, **payload,
    })
    return fixture.wait(started["operation_id"], success)


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-delete-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent, server)
        proxy = None
        try:
            source = root / "source"
            source.mkdir()
            (source / "空目录").mkdir()
            (source / "中文 file.txt").write_text("first version")
            os.link(source / "中文 file.txt", source / "hardlink")
            (source / "link").symlink_to("missing")
            task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
            first = fixture.backup(task["id"])
            (source / "中文 file.txt").write_text("second version")
            second = fixture.backup(task["id"])
            expected = tree(source)
            versions = root / "repository/storage/versions"
            receipts = root / "repository/database/deletions"
            deleting = root / "repository/storage/deleting"

            with connection(fixture, authenticate=False) as client:
                rpc(client, "delete_version", {"version_id": first["id"]}, ok=False)
            for identifier in ["../source", "/tmp", "", "no-such-version"]:
                with connection(fixture) as client:
                    rpc(client, "delete_version", {"version_id": identifier}, ok=False)
            failed = delete(fixture, first["id"], False, repository_path=str(root / "wrong"))
            assert "Repository" in failed["error"]
            assert (versions / first["id"]).is_dir()
            print("PASS: authentication, invalid IDs and wrong repository cannot delete data", flush=True)

            with connection(fixture) as reader:
                rpc(reader, "version", {"version_id": first["id"]})
                held = delete(fixture, first["id"], False)
                assert "Version is in use" in held["error"], held
                assert not (receipts / (first["id"] + ".json")).exists()
                rpc(reader, "entries", {"version_id": first["id"], "offset": 0})
            fixture.request("remove_task", {"id": task["id"]})
            removed = delete(fixture, first["id"])
            assert removed["action"] == "delete_version"
            assert removed["version_id"] == first["id"]
            assert removed["task_id"] == task["id"]
            assert not (versions / first["id"]).exists()
            assert not (deleting / first["id"]).exists()
            assert fixture.request("versions", {"target_id": "local"})["total"] == 1
            assert tree(source) == expected
            fixture.restore(second["id"], root / "restored")
            assert tree(root / "restored") == expected
            fixture.restore(first["id"], root / "deleted-restore", False)
            delete(fixture, first["id"])
            with connection(fixture) as client:
                denied = rpc(client, "begin", {"operation_id": first["id"]}, ok=False)
                assert "deleted version" in denied["reason"]
            print("PASS: live readers protected; task removal, deletion, repeat and surviving restore", flush=True)

            task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
            for index in range(300):
                (source / f"file-{index:04d}").write_text(str(index))
            large = fixture.backup(task["id"])
            with connection(fixture) as client:
                result = rpc(client, "delete_version", {"version_id": large["id"]})
            assert result["done"] is False
            assert not (versions / large["id"]).exists()
            assert (deleting / large["id"]).is_dir()
            stop(fixture.server)
            fixture.start_server()
            delete(fixture, large["id"])
            assert not (deleting / large["id"]).exists()
            stop(fixture.agent)
            fixture.start_agent()
            assert fixture.request("operation", {"id": removed["id"]})["state"] == "SUCCEEDED"
            print("PASS: batch deletion and durable receipts resume after Server/Agent restart", flush=True)

            victim = fixture.backup(task["id"])
            drop_replies = True
            def drop(request, reply):
                return drop_replies and request["type"] == "delete_version"
            proxy = Proxy(fixture.port, drop)
            fixture.request("save_target", {
                "id": "local", "name": "Deletion proxy", "host": "127.0.0.1", "port": proxy.listen_port,
            })
            uncertain = delete(fixture, victim["id"], False)
            assert uncertain["state"] == "WAITING", uncertain
            drop_replies = False
            fixture.configure_target()
            retry = fixture.request("start_delete_version", {"retry_operation_id": uncertain["id"]})
            fixture.wait(retry["operation_id"])
            assert not (deleting / victim["id"]).exists()
            assert (versions / second["id"]).exists()
            proxy.close()
            proxy = None
            print("PASS: lost replies produce a retryable record with the original target snapshot", flush=True)

            victim = fixture.backup(task["id"])
            interrupted = threading.Event()
            def interrupt(request, reply):
                if request["type"] == "delete_version" and not interrupted.is_set():
                    fixture.agent.kill()
                    interrupted.set()
                    return True
                return False
            proxy = Proxy(fixture.port, interrupt)
            fixture.request("save_target", {
                "id": "local", "name": "Crash proxy", "host": "127.0.0.1", "port": proxy.listen_port,
            })
            started = fixture.request("start_delete_version", {"target_id": "local", "version_id": victim["id"]})
            assert interrupted.wait(5)
            stop(fixture.agent)
            fixture.start_agent()
            recovered = fixture.request("operation", {"id": started["operation_id"]})
            assert recovered["state"] == "WAITING", recovered
            retry = fixture.request("start_delete_version", {"retry_operation_id": recovered["id"]})
            fixture.wait(retry["operation_id"])
            assert not (deleting / victim["id"]).exists()
            proxy.close()
            proxy = None
            print("PASS: process crash records pending deletion and can continue after reopening", flush=True)

            outside = root / "outside"
            outside.mkdir()
            sentinel = outside / "keep"
            sentinel.write_text("do not delete")
            (versions / "escape").symlink_to(outside, target_is_directory=True)
            with connection(fixture) as client:
                rpc(client, "delete_version", {"version_id": "escape"}, ok=False)
            (versions / "escape").unlink()
            victim = fixture.backup(task["id"])
            content = next((versions / victim["id"]).glob("*.data"))
            content.unlink()
            content.symlink_to(sentinel)
            delete(fixture, victim["id"])
            assert sentinel.read_text() == "do not delete"
            print("PASS: deletion rejects linked roots and never follows content links", flush=True)

            victim = fixture.backup(task["id"])
            receipts.chmod(0o500)
            try:
                delete(fixture, victim["id"], False)
                assert (versions / victim["id"]).exists()
            finally:
                receipts.chmod(0o700)
            with connection(fixture) as client:
                pending = rpc(client, "delete_version", {"version_id": victim["id"]})
            assert not pending["done"]
            (deleting / victim["id"]).chmod(0o500)
            failed = delete(fixture, victim["id"], False)
            assert (deleting / victim["id"]).exists()
            (deleting / victim["id"]).chmod(0o700)
            retry = fixture.request("start_delete_version", {"retry_operation_id": failed["id"]})
            fixture.wait(retry["operation_id"])
            fixture.restore(second["id"], root / "after-failures")
            assert tree(root / "after-failures") == expected
            print("PASS: permission failures before/after hiding remain recoverable; other data intact", flush=True)
        finally:
            if proxy:
                proxy.close()
            fixture.close()


if __name__ == "__main__":
    run(*sys.argv[1:])
