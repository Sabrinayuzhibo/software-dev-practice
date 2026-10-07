#!/usr/bin/env python3
"""Real processes and filesystem checks for the P0 backup/restore contract."""

import hashlib
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import time


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream:
            stream.close()


class Fixture:
    def __init__(self, root, agent_binary, server_binary):
        self.root = root
        self.agent_binary = agent_binary
        self.server_binary = server_binary
        self.server = None
        self.agent = None
        self.sequence = 0
        self.start_server()
        self.start_agent()

    def start_server(self):
        self.server = subprocess.Popen(
            [self.server_binary, "--data-dir", str(self.root / "repository"), "--port", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        assert select.select([self.server.stdout], [], [], 5)[0], "server startup timed out"
        line = self.server.stdout.readline().strip()
        assert line.startswith("LISTENING 127.0.0.1:"), (line, self.server.stderr.read())
        self.port = int(line.rsplit(":", 1)[1])
        if self.agent:
            self.configure_target()

    def start_agent(self):
        self.agent = subprocess.Popen(
            [self.agent_binary, "--state-dir", str(self.root / "agent-state")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.configure_target()

    def configure_target(self):
        self.request("save_target", {
            "id": "local", "name": "Test repository", "host": "127.0.0.1", "port": self.port,
        })

    def request(self, action, payload=None, ok=True):
        self.sequence += 1
        request_id = str(self.sequence)
        self.agent.stdin.write(json.dumps({"id": request_id, "action": action, "payload": payload or {}}) + "\n")
        self.agent.stdin.flush()
        assert select.select([self.agent.stdout], [], [], 15)[0], f"agent timeout: {action}"
        line = self.agent.stdout.readline()
        assert line, f"agent exited: {self.agent.poll()}"
        reply = json.loads(line)
        assert reply["id"] == request_id, reply
        assert reply["ok"] == ok, reply
        return reply["result"] if ok else reply["error"]

    def wait(self, operation_id, success=True, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            record = self.request("operation", {"id": operation_id})
            if record["state"] not in ("RUNNING",):
                assert record["state"].startswith("SUCCEEDED") == success, record
                return record
            time.sleep(0.02)
        raise AssertionError("operation timeout: " + operation_id)

    def backup(self, task_id):
        started = self.request("start_backup", {"task_id": task_id})
        operation_id = started["operation_id"]
        version = self.wait(operation_id)["result"]["version"]
        version["warnings"] = []
        offset = 0
        while offset is not None:
            page = self.request("operation_warnings", {"id": operation_id, "offset": offset})
            version["warnings"].extend(page["warnings"])
            offset = page["next_offset"]
        return version

    def restore(self, version_id, destination, success=True):
        started = self.request("start_restore", {
            "target_id": "local", "version_id": version_id, "destination": str(destination),
        })
        return self.wait(started["operation_id"], success)

    def close(self):
        if self.agent:
            stop(self.agent)
        if self.server:
            stop(self.server)


def tree(root):
    result = {}
    for item in root.rglob("*"):
        relative = item.relative_to(root).as_posix()
        if item.is_symlink():
            continue
        result[relative] = "directory" if item.is_dir() else hashlib.sha256(item.read_bytes()).hexdigest()
    return result


def receive_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        assert chunk, "incomplete server reply"
        data.extend(chunk)
    return bytes(data)


def rpc(connection, action, payload, ok=True):
    message = json.dumps({"type": action, "request_id": action, "payload": payload}).encode()
    connection.sendall(len(message).to_bytes(4, "big") + message)
    length = int.from_bytes(receive_exact(connection, 4), "big")
    reply = json.loads(receive_exact(connection, length))
    assert (reply["type"] != "error") == ok, reply
    return reply["payload"]


def run(agent_binary, server_binary):
    with tempfile.TemporaryDirectory(prefix="backup-p0-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent_binary, server_binary)
        checks = 0

        def passed(label):
            nonlocal checks
            checks += 1
            print(f"PASS {checks:02d}: {label}", flush=True)

        try:
            source = root / "source"
            (source / "sub" / "empty directory").mkdir(parents=True)
            (source / "sub" / "中文 名称.txt").write_text("first version\n", encoding="utf8")
            (source / ".hidden").write_bytes(b"hidden\x00data")
            (source / "empty-file").touch()
            task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
            repeated = fixture.request("add_task", {"path": str(source / "sub" / ".."), "target_id": "local"})
            assert repeated["id"] == task["id"]
            fixture.request("add_task", {"path": str(root / "missing"), "target_id": "local"}, ok=False)
            passed("task validation and canonical duplicate suppression (AC-01)")

            fixture.request("import_tasks", {"tasks": [{"id": "legacy-id", "path": str(root / "legacy-missing"), "createdAt": "2026-01-01"}]})
            assert fixture.request("config")["tasks"][1]["id"] == "legacy-id"
            fixture.request("remove_task", {"id": "legacy-id"})
            fixture.request("import_tasks", {"tasks": [{"id": "legacy-id", "path": str(root / "legacy-missing"), "createdAt": "2026-01-01"}]})
            assert len(fixture.request("config")["tasks"]) == 1
            passed("legacy IDs preserved and removed legacy tasks not reimported")

            scan = fixture.request("scan", {"task_id": task["id"]})
            assert scan["files"] == 3 and scan["directories"] == 2 and scan["complete"]
            assert fixture.request("versions", {"target_id": "local"})["versions"] == []
            passed("scan includes hidden/empty/UTF-8 paths without publishing a version (AC-03)")

            first_tree = tree(source)
            first = fixture.backup(task["id"])
            (source / "sub" / "中文 名称.txt").write_text("second version", encoding="utf8")
            (source / ".hidden").unlink()
            (source / "new-file").write_text("new")
            second_tree = tree(source)
            second = fixture.backup(task["id"])
            fixture.restore(first["id"], root / "restore-a")
            fixture.restore(second["id"], root / "restore-b")
            assert tree(root / "restore-a") == first_tree
            assert tree(root / "restore-b") == second_tree
            passed("two historical versions restore exact independent trees (AC-04/05)")

            result = fixture.restore(first["id"], root / "restore-a", False)
            assert "empty" in result["error"]
            assert tree(root / "restore-a") == first_tree
            fixture.restore(first["id"], source, False)
            fixture.restore(first["id"], source / "new-output", False)
            fixture.restore(first["id"], root / "repository" / "output", False)
            (root / "restore-link").symlink_to(root / "restore-a")
            fixture.restore(first["id"], root / "restore-link", False)
            passed("nonempty, source/repository overlap and symlink restore roots rejected (AC-06/27)")

            empty_source = root / "empty-source"
            empty_source.mkdir()
            empty_task = fixture.request("add_task", {"path": str(empty_source), "target_id": "local"})
            empty_version = fixture.backup(empty_task["id"])
            fixture.restore(empty_version["id"], root / "empty-restore")
            fixture.restore(first["id"], empty_source, False)
            assert not any((root / "empty-restore").iterdir())
            passed("empty trees preserved; configured empty source protected")

            (source / "external-link").symlink_to(root / "restore-a")
            os.mkfifo(source / "fifo")
            warning_version = fixture.backup(task["id"])
            assert len(warning_version["warnings"]) == 2
            (source / "fifo").unlink()
            (source / "external-link").unlink()
            passed("unsupported symlink/FIFO reported without following or blocking")

            unreadable = source / "restricted"
            unreadable.write_text("unreadable")
            unreadable.chmod(0)
            failed = fixture.request("start_backup", {"task_id": task["id"]})
            assert "restricted" in fixture.wait(failed["operation_id"], False)["error"]
            unreadable.chmod(0o600)
            unreadable.unlink()
            passed("unreadable source fails with a path and leaves old versions intact (AC-08)")

            token = json.loads((root / "repository/database/access.json").read_text())["token"]
            with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as connection:
                rpc(connection, "versions", {}, ok=False)
                rpc(connection, "authenticate", {"token": "incorrect"}, ok=False)
                rpc(connection, "authenticate", {"token": token})
                again = rpc(connection, "commit", {"operation_id": first["id"]})
                assert again["id"] == first["id"]
                rpc(connection, "begin", {"operation_id": "partial", "source": str(source), "task_id": task["id"]})
                rpc(connection, "entry", {"path": "partial", "type": "file", "size": "100"})
            time.sleep(0.05)
            versions = fixture.request("versions", {"target_id": "local"})["versions"]
            assert all(item["id"] != "partial" for item in versions)
            passed("local authentication, idempotent commit and abandoned upload invisibility (AC-07/34)")

            stop(fixture.server)
            fixture.start_server()
            fixture.restore(first["id"], root / "after-server-restart")
            assert tree(root / "after-server-restart") == first_tree
            stop(fixture.agent)
            fixture.start_agent()
            assert fixture.request("config")["tasks"][0]["id"] == task["id"]
            assert fixture.request("operations")["total"] > 10
            fixture.request("remove_task", {"id": task["id"]})
            fixture.restore(first["id"], root / "after-task-delete")
            assert source.exists() and tree(root / "after-task-delete") == first_tree
            passed("server/Agent restart, persistent records and task-independent versions (AC-01/05)")

            version_path = root / "repository/storage/versions" / first["id"]
            entries = [json.loads(line) for line in (version_path / "entries.jsonl").read_text().splitlines()]
            content_entry = next(item for item in entries if item["type"] == "file" and int(item["size"]) > 0)
            content = version_path / (str(content_entry["index"]) + ".data")
            original = content.read_bytes()
            content.write_bytes(b"x" * len(original))
            result = fixture.restore(first["id"], root / "corrupt-output", False)
            assert "checksum" in result["error"]
            assert not (root / "corrupt-output" / content_entry["path"]).exists()
            content.write_bytes(original)
            passed("corrupt content rejected before the affected file is published (AC-09)")

            manifest = version_path / "entries.jsonl"
            original_manifest = manifest.read_bytes()
            poisoned = dict(entries[0], path="../sentinel")
            manifest.write_text(json.dumps(poisoned) + "\n", encoding="utf8")
            sentinel = root / "sentinel"
            sentinel.write_text("safe")
            result = fixture.restore(first["id"], root / "unsafe-output", False)
            assert "Unsafe" in result["error"] and sentinel.read_text() == "safe"
            assert not (root / "unsafe-output").exists()
            manifest.write_bytes(original_manifest)
            passed("malicious manifest rejected before any restore writes (AC-27)")

            denied = root / "denied"
            denied.mkdir(mode=0o500)
            fixture.restore(first["id"], denied, False)
            denied.chmod(0o700)
            passed("restore permission errors produce a failed execution (AC-31)")

            stop(fixture.server)
            fixture.request("ping", {"target_id": "local"}, ok=False)
            assert fixture.request("scan", {"path": str(source)})["complete"]
            passed("offline service does not prevent local scanning and task access (AC-30)")
        finally:
            fixture.close()
        print(f"PASS: {checks} P0 integration scenarios")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
