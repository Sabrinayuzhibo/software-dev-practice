#!/usr/bin/env python3
"""真实 Agent/Server 验证加密仓库、独立归档和实时解锁。"""

import os
from pathlib import Path
import sys
import socket
import tempfile
import time

from backup_archive import policy, run_operation, snapshot
from backup_faults import Proxy
from backup_integration import Fixture, stop

PASSWORD = "本仓库-Scrypt-密码 40!"
OTHER_PASSWORD = "另一仓库的独立密码 27!"
MARKER = b"private-file-content-Jz4dw79Z"
NAME = "private-filename-d52a.txt"


def wait_for(check, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.15)
    raise AssertionError("condition timed out")


def assert_no_plain(directory):
    for file in directory.rglob("*"):
        if file.is_file():
            data = file.read_bytes()
            for marker in (
                MARKER,
                NAME.encode(),
                PASSWORD.encode(),
                OTHER_PASSWORD.encode(),
            ):
                assert marker not in data, file


def unlock(fixture, password=PASSWORD):
    return fixture.request(
        "unlock_repository", {"target_id": "local", "password": password}
    )


def initialize(fixture, password=PASSWORD):
    return fixture.request(
        "initialize_encryption", {"target_id": "local", "password": password}
    )


def restore(fixture, version, output):
    fixture.restore(version["id"], output)
    assert (output / NAME).read_bytes().startswith(MARKER)
    assert (output / "empty").stat().st_size == 0
    assert (output / "hard").stat().st_ino == (output / NAME).stat().st_ino
    assert os.readlink(output / "link") == NAME


def exercise(agent, server, root):
    fixture = Fixture(root, agent, server)
    proxy = None
    skipped = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        source = root / "source"
        source.mkdir()
        path = source / NAME
        path.write_bytes(MARKER * 30000 + os.urandom(400000))
        path.chmod(0o640)
        os.utime(path, ns=(1_000_000_000, 999_999_999_123_456_789))
        (source / "empty").touch()
        (source / "nested").mkdir()
        (source / "nested" / "keep.txt").write_text("nested")
        (source / "ignored.tmp").write_text("excluded")
        skipped.bind(str(source / "skipped"))
        deep = source / "nested" / ("a" * 200) / ("b" * 200) / ("c" * 200)
        deep.mkdir(parents=True)
        for i in range(340):
            (deep / f"{i:04d}-{'x' * 144}.txt").touch()
        os.symlink(NAME, source / "link")
        os.link(path, source / "hard")
        task = fixture.request(
            "add_task",
            {
                "path": str(source),
                "target_id": "local",
                "filters": {
                    "include": [],
                    "exclude": [{"category": "name", "pattern": "*.tmp"}],
                },
            },
        )

        observed = []
        violations = []

        def inspect(request, _reply):
            # Server 能见到动作、密文和结构字段，不能收到私有文件路径、密码或明文。
            payload = str(request).encode()
            observed.append(request["type"])
            if any(
                item in payload for item in (PASSWORD.encode(), NAME.encode(), MARKER)
            ) or request["type"] in {"begin", "entry", "chunk", "warnings"}:
                violations.append(request["type"])
            return False

        proxy = Proxy(fixture.port, inspect)
        port = fixture.port
        fixture.port = proxy.listen_port
        policy(fixture, "huffman")
        created = initialize(fixture)
        assert created["unlocked"]
        assert not fixture.request("ping", {"target_id": "local"})["repository_locked"]
        first = fixture.backup(task["id"])
        assert first["compression"] == "huffman"
        assert first["warnings"][0]["path"] == "skipped"
        assert {"sealed_begin", "sealed_chunk", "sealed_commit"} <= set(observed)
        assert not violations, violations
        restore(fixture, first, root / "restored")
        actual = snapshot(root / "restored")
        assert "ignored.tmp" not in actual
        assert actual[NAME] == snapshot(source)[NAME]
        assert_no_plain(root / "repository")
        assert PASSWORD.encode() not in (root / "agent-state/config.json").read_bytes()
        summary_path = (
            root / "repository/storage/versions" / first["id"] / "sealed.data"
        )
        original = summary_path.read_bytes()
        with summary_path.open("r+b") as f:
            f.seek(200)
            f.write(bytes([original[200] ^ 1]))
        failed = fixture.restore(first["id"], root / "tampered", False)
        assert "Authentication failed" in failed["error"], failed
        # 任一损坏内容在发布前验证，硬链接组的明文不会落入目标路径。
        assert not (root / "tampered" / NAME).exists()
        assert not (root / "tampered" / "hard").exists()
        summary_path.write_bytes(original)
        with summary_path.open("r+b") as f:
            # 末尾包含密文警告和清单。仅翻转清单中的字节，以验证写入前拒绝。
            header = root / "repository/storage/versions" / first["id"] / "summary.json"
            assert NAME.encode() not in header.read_bytes()
            position = len(original) - 512
            f.seek(position)
            f.write(bytes([original[position] ^ 1]))
        invalid_manifest = fixture.restore(first["id"], root / "bad-manifest", False)
        assert "Authentication failed" in invalid_manifest["error"], invalid_manifest
        assert not (root / "bad-manifest").exists()
        summary_path.write_bytes(original)

        archive = root / "portable.bsar"
        run_operation(
            fixture,
            "export_archive",
            {"version_id": first["id"], "archive_path": str(archive)},
        )
        assert archive.read_bytes().startswith(b"BSAE")
        assert (
            NAME.encode() not in archive.read_bytes()
            and MARKER not in archive.read_bytes()
        )
        fixture.request("lock_repository", {"target_id": "local"})
        error = unlock_wrong(fixture)
        assert "Authentication failed" in error["message"], error
        locked = fixture.request("versions", {"target_id": "local"}, ok=False)
        assert "REPOSITORY_LOCKED" in locked["message"]
        unlock(fixture)
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": True})
        wait_for(
            lambda: fixture.request("config")["realtime_status"][task["id"]]["state"]
            == "WATCHING"
        )
        stop(fixture.agent)
        (source / "restart.txt").write_text("update while Agent stopped")
        fixture.start_agent()
        wait_for(
            lambda: fixture.request("config")["realtime_status"][task["id"]]["state"]
            == "WAITING_UNLOCK"
        )
        before = len(list((root / "repository/storage/versions").iterdir()))
        time.sleep(2)
        assert len(list((root / "repository/storage/versions").iterdir())) == before
        unlock(fixture)
        wait_for(
            lambda: len(list((root / "repository/storage/versions").iterdir())) > before
        )
        wait_for(
            lambda: fixture.request("config")["realtime_status"][task["id"]]["state"]
            == "WATCHING"
        )
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": False})
        fixture.port = port
    finally:
        skipped.close()
        fixture.close()
        if proxy:
            proxy.close()

    # 原服务及仓库均不可用，使用另一仓库的密码和压缩策略导入。
    (root / "repository").rename(root / "offline-repository")
    destination = root / "other"
    destination.mkdir()
    fixture = Fixture(destination, agent, server)
    try:
        args = {"archive_path": str(archive), "archive_password": PASSWORD}
        assert (
            "ENCRYPTED_IMPORT_CONFIRM"
            in run_operation(fixture, "import_archive", args, False)["error"]
        )
        policy(fixture, "lz77")
        initialize(fixture, OTHER_PASSWORD)
        assert (
            "Authentication failed"
            in run_operation(
                fixture, "import_archive", {**args, "archive_password": "wrong"}, False
            )["error"]
        )
        corrupt = root / "truncated.bsar"
        corrupt.write_bytes(archive.read_bytes()[:-1])
        assert (
            "Truncated"
            in run_operation(
                fixture, "import_archive", {**args, "archive_path": str(corrupt)}, False
            )["error"]
        )
        assert fixture.request("versions", {"target_id": "local"})["versions"] == []
        imported = run_operation(fixture, "import_archive", args)["result"]["version"]
        assert imported["compression"] == "lz77"
        restore(fixture, imported, root / "offline-restored")
        assert_no_plain(destination / "repository")
        run_operation(fixture, "delete_version", {"version_id": imported["id"]})
        assert fixture.request("versions", {"target_id": "local"})["versions"] == []
        fixture.request(
            "initialize_encryption",
            {"target_id": "local", "password": PASSWORD},
            ok=False,
        )
        # 独立验证 Server 重启的仓库策略持久化。
        stop(fixture.server)
        fixture.start_server()
        assert fixture.request("ping", {"target_id": "local"})[
            "repository_encryption_id"
        ]
    finally:
        fixture.close()

    plain = root / "explicit-plain"
    plain.mkdir()
    fixture = Fixture(plain, agent, server)
    try:
        # 未加密目标必须由用户明确同意，且已有备份的仓库不能直接标为加密。
        record = run_operation(
            fixture, "import_archive", {**args, "allow_plain_import": True}
        )
        version = record["result"]["version"]
        restore(fixture, version, root / "explicit-plain-restored")
        rejected = fixture.request(
            "initialize_encryption",
            {"target_id": "local", "password": PASSWORD},
            ok=False,
        )
        assert "new, empty repository" in rejected["message"], rejected
    finally:
        fixture.close()


def unlock_wrong(fixture):
    return fixture.request(
        "unlock_repository", {"target_id": "local", "password": "wrong"}, ok=False
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="backup-encryption-") as directory:
        exercise(sys.argv[1], sys.argv[2], Path(directory))
        print(
            "PASS: encrypted backup, restore, authentication, restart unlock, offline archive, policy conversion and deletion"
        )
