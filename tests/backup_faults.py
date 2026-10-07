#!/usr/bin/env python3
"""Inject failures at real protocol boundaries, without production test hooks."""

import json
from pathlib import Path
import socket
import threading
import tempfile
import sys

from backup_integration import Fixture, receive_exact, stop, tree


class Proxy:
    def __init__(self, port, hook):
        self.port = port
        self.hook = hook
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen()
        self.socket.settimeout(0.2)
        self.listen_port = self.socket.getsockname()[1]
        self.stopped = threading.Event()
        self.errors = []
        self.workers = []
        self.thread = threading.Thread(target=self.accept)
        self.thread.start()

    def accept(self):
        while not self.stopped.is_set():
            try:
                connection, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            worker = threading.Thread(target=self.forward, args=(connection,))
            worker.start()
            self.workers.append(worker)

    def forward(self, connection):
        try:
            with connection, socket.create_connection(("127.0.0.1", self.port), timeout=5) as upstream:
                connection.settimeout(10)
                while True:
                    length = receive_exact(connection, 4)
                    data = receive_exact(connection, int.from_bytes(length, "big"))
                    request = json.loads(data)
                    upstream.sendall(length + data)
                    reply_length = receive_exact(upstream, 4)
                    reply = receive_exact(upstream, int.from_bytes(reply_length, "big"))
                    if self.hook(request, json.loads(reply)):
                        return
                    connection.sendall(reply_length + reply)
        except (OSError, AssertionError):
            pass
        except Exception as error:
            self.errors.append(error)

    def close(self):
        self.stopped.set()
        self.socket.close()
        self.thread.join(timeout=2)
        for worker in self.workers:
            worker.join(timeout=12)
            assert not worker.is_alive(), "proxy worker leaked"
        assert not self.errors, self.errors


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-faults-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent, server)
        proxy = None
        try:
            source = root / "source"
            source.mkdir()
            contents = source / "content"
            contents.write_bytes(b"original" * 200000)
            task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
            first = fixture.backup(task["id"])
            original_tree = tree(source)

            def attach(hook):
                nonlocal proxy
                if proxy:
                    proxy.close()
                proxy = Proxy(fixture.port, hook)
                fixture.request("save_target", {
                    "id": "local", "name": "Fault proxy", "host": "127.0.0.1", "port": proxy.listen_port,
                })

            dropped = False

            def drop_commit(request, _reply):
                nonlocal dropped
                if request["type"] == "commit" and not dropped:
                    dropped = True
                    return True
                return False

            attach(drop_commit)
            committed = fixture.backup(task["id"])
            assert dropped and committed["id"] != first["id"]
            assert fixture.request("versions", {"target_id": "local"})["total"] == 2
            print("PASS: lost commit reply resolved by operation ID, no duplicate version (AC-34)", flush=True)

            changed = False

            def change_source(request, _reply):
                nonlocal changed
                if request["type"] == "chunk" and not changed:
                    changed = True
                    with contents.open("ab") as output:
                        output.write(b"changed")
                return False

            attach(change_source)
            operation = fixture.request("start_backup", {"task_id": task["id"]})
            result = fixture.wait(operation["operation_id"], False)
            assert changed and "changed" in result["error"].lower(), result
            assert fixture.request("versions", {"target_id": "local"})["total"] == 2
            print("PASS: source changed during upload fails without publishing (AC-08)", flush=True)

            def deny_storage(request, _reply):
                if request["type"] == "begin":
                    staging = root / "repository/storage/staging" / request["payload"]["operation_id"]
                    staging.chmod(0o500)
                return False

            attach(deny_storage)
            operation = fixture.request("start_backup", {"task_id": task["id"]})
            fixture.wait(operation["operation_id"], False)
            for staging in (root / "repository/storage/staging").iterdir():
                staging.chmod(0o700)
            fixture.restore(first["id"], root / "after-write-failure")
            assert tree(root / "after-write-failure") == original_tree
            print("PASS: storage write failure preserves old versions (AC-31)", flush=True)

            interrupted = False

            def kill_server(request, _reply):
                nonlocal interrupted
                if request["type"] == "chunk" and not interrupted:
                    interrupted = True
                    fixture.server.kill()
                    fixture.server.wait(timeout=5)
                    return True
                return False

            attach(kill_server)
            operation = fixture.request("start_backup", {"task_id": task["id"]})
            fixture.wait(operation["operation_id"], False)
            proxy.close()
            proxy = None
            stop(fixture.server)
            fixture.start_server()
            assert fixture.request("versions", {"target_id": "local"})["total"] == 2
            fixture.restore(first["id"], root / "after-crash")
            assert tree(root / "after-crash") == original_tree
            print("PASS: server killed mid-upload; only committed versions survive restart (AC-07)", flush=True)

            blocked = threading.Event()
            release = threading.Event()

            def pause_upload(request, _reply):
                if request["type"] == "chunk" and not blocked.is_set():
                    blocked.set()
                    assert release.wait(10), "test release timeout"
                return False

            attach(pause_upload)
            operation = fixture.request("start_backup", {"task_id": task["id"]})
            assert blocked.wait(5)
            fixture.request("start_backup", {"task_id": task["id"]}, ok=False)
            fixture.request("remove_task", {"id": task["id"]}, ok=False)
            assert fixture.request("operation", {"id": operation["operation_id"]})["state"] == "RUNNING"
            fixture.agent.kill()
            fixture.agent.wait(timeout=5)
            release.set()
            proxy.close()
            proxy = None
            stop(fixture.agent)
            fixture.start_agent()
            record = fixture.request("operation", {"id": operation["operation_id"]})
            assert record["state"] == "INTERRUPTED", record
            print("PASS: busy rejection, live progress queries and interrupted Agent recovery", flush=True)

            partial_restore = threading.Event()

            def kill_download(request, _reply):
                if request["type"] == "download" and int(request["payload"]["offset"]) > 0:
                    partial_restore.set()
                    return True
                return False

            attach(kill_download)
            fixture.restore(first["id"], root / "partial-restore", False)
            assert partial_restore.is_set()
            assert not (root / "partial-restore/content").exists()
            assert not list((root / "partial-restore").glob(".backup-*"))
            print("PASS: interrupted restore reports failure and removes unverified temporary file", flush=True)

            committed_without_reply = threading.Event()
            allow_confirmation = threading.Event()

            def unavailable_confirmation(request, _reply):
                if request["type"] == "commit" and not committed_without_reply.is_set():
                    committed_without_reply.set()
                    return True
                return committed_without_reply.is_set() and not allow_confirmation.is_set()

            attach(unavailable_confirmation)
            operation = fixture.request("start_backup", {"task_id": task["id"]})
            record = fixture.wait(operation["operation_id"], False)
            assert record["state"] == "WAITING", record
            allow_confirmation.set()
            fixture.request("confirm", {"id": operation["operation_id"]})
            assert fixture.request("operation", {"id": operation["operation_id"]})["state"] == "SUCCEEDED"
            print("PASS: unavailable commit confirmation remains WAITING and resolves later", flush=True)

            commit_crash = threading.Event()

            def kill_after_commit(request, _reply):
                if request["type"] == "commit" and not commit_crash.is_set():
                    fixture.agent.kill()
                    fixture.agent.wait(timeout=5)
                    commit_crash.set()
                    return True
                return False

            attach(kill_after_commit)
            operation = fixture.request("start_backup", {"task_id": task["id"]})
            assert commit_crash.wait(5)
            stop(fixture.agent)
            fixture.start_agent()
            record = fixture.request("operation", {"id": operation["operation_id"]})
            assert record["state"] == "WAITING", record
            fixture.request("confirm", {"id": operation["operation_id"]})
            assert fixture.request("operation", {"id": operation["operation_id"]})["state"] == "SUCCEEDED"
            print("PASS: Agent crash after commit recovers without misreporting failed backup (AC-34)", flush=True)
        finally:
            fixture.close()
            if proxy:
                proxy.close()


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
