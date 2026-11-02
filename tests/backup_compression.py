#!/usr/bin/env python3
"""通过真实备份、还原和故障注入验证课件中的 Huffman/LZ77 算法。"""

import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

from backup_faults import Proxy
from backup_integration import Fixture, rpc, stop, tree


def policy(fixture, algorithm, port=None):
    return fixture.request("save_target", {
        "id": "local", "name": "Compression repository", "host": "127.0.0.1",
        "port": port or fixture.port, "compression": algorithm,
    })


def restart_agent(fixture):
    stop(fixture.agent)
    fixture.agent = subprocess.Popen(
        [fixture.agent_binary, "--state-dir", str(fixture.root / "agent-state")],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )


def corruptions(fixture, version, algorithm):
    directory = fixture.root / "repository/storage/versions" / version["id"]
    entries = [json.loads(line) for line in (directory / "entries.jsonl").read_text().splitlines()]
    entry = next(item for item in entries if item["type"] == "file" and int(item["size"]) > 1000000)
    content = directory / (entry["index"] + ".data")
    original = content.read_bytes()
    damaged = bytearray(original)
    damaged[13] ^= 1
    oversized = bytearray(original)
    oversized[5:9] = (0xffffffff).to_bytes(4, "big")
    for label, data in [("checksum", damaged), ("truncated", original[:-1]),
                        ("trailing", original + b"x"), ("oversized", oversized)]:
        destination = fixture.root / f"{algorithm}-{label}"
        content.write_bytes(data)
        failed = fixture.restore(version["id"], destination, False)
        assert failed["state"] == "FAILED", failed
        assert not (destination / entry["path"]).exists()
        content.write_bytes(original)
    summary_path = directory / "summary.json"
    manifest_path = directory / "entries.jsonl"
    summary = summary_path.read_bytes()
    manifest = manifest_path.read_bytes()
    try:
        entry["compression"] = "unknown"
        manifest_path.write_text("".join(json.dumps(item, separators=(",", ":")) + "\n" for item in entries))
        changed = json.loads(summary)
        changed["manifest_sha256"] = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
        summary_path.write_text(json.dumps(changed))
        failed = fixture.restore(version["id"], fixture.root / f"{algorithm}-bad-codec", False)
        assert "compression" in failed["error"].lower(), failed
    finally:
        summary_path.write_bytes(summary)
        manifest_path.write_bytes(manifest)


def transfer_failures(fixture, task, source, algorithm):
    changed = False
    current_file = ""

    def mutate(request, reply):
        nonlocal changed, current_file
        if request["type"] == "entry":
            current_file = request["payload"]["path"]
        if (request["type"] == "chunk" and not changed and
                current_file in ("compressible.bin", "hardlink")):
            changed = True
            with (source / "compressible.bin").open("ab") as output:
                output.write(b"changed")
        return False

    before = fixture.request("versions", {"target_id": "local"})["total"]
    proxy = Proxy(fixture.port, mutate)
    try:
        policy(fixture, algorithm, proxy.listen_port)
        operation = fixture.request("start_backup", {"task_id": task["id"]})
        failed = fixture.wait(operation["operation_id"], False)
        assert changed and "changed" in failed["error"].lower(), failed
        assert fixture.request("versions", {"target_id": "local"})["total"] == before
    finally:
        proxy.close()
        policy(fixture, algorithm)
        with (source / "compressible.bin").open("r+b") as output:
            output.truncate(4 * 1024 * 1024)

    proxy = Proxy(fixture.port, lambda request, reply: request["type"] == "chunk")
    try:
        policy(fixture, algorithm, proxy.listen_port)
        operation = fixture.request("start_backup", {"task_id": task["id"]})
        fixture.wait(operation["operation_id"], False)
        assert fixture.request("versions", {"target_id": "local"})["total"] == before
    finally:
        proxy.close()
        policy(fixture, algorithm)

    dropped = False

    def drop_commit(request, reply):
        nonlocal dropped
        if request["type"] == "commit" and not dropped:
            dropped = True
            return True
        return False

    proxy = Proxy(fixture.port, drop_commit)
    try:
        policy(fixture, algorithm, proxy.listen_port)
        version = fixture.backup(task["id"])
        assert dropped and version["compression"] == algorithm
        assert fixture.request("versions", {"target_id": "local"})["total"] == before + 1
    finally:
        proxy.close()
        policy(fixture, algorithm)


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-compression-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent, server)
        try:
            source = root / "source"
            (source / "空目录").mkdir(parents=True)
            (source / "compressible.bin").write_bytes(b"a" * (4 * 1024 * 1024))
            (source / "random.bin").write_bytes(os.urandom(512 * 1024 + 7))
            (source / "all-bytes.bin").write_bytes(bytes(range(256)) * 1025)
            (source / "中文 file.txt").write_text("备份压缩\n" * 1000)
            (source / "empty").touch()
            (source / "link").symlink_to("中文 file.txt")
            os.link(source / "compressible.bin", source / "hardlink")
            os.mkfifo(source / "pipe")
            task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
            expected = tree(source)
            plain = fixture.backup(task["id"])
            assert plain["format"] == 4 and "compression" not in plain
            for algorithm in ("huffman", "lz77"):
                policy(fixture, algorithm)
                # 添加支持设备类型的任务时，不得降低压缩配置的格式版本。
                selected = fixture.request("add_task", {
                    "sources": [str(source)], "target_id": "local",
                    "file_types": ["file", "character_device"],
                    "preserve_empty_dirs": True,
                })
                assert fixture.request("config")["format"] == 7
                fixture.request("remove_task", {"id": selected["id"]})
                restart_agent(fixture)
                assert fixture.request("config")["targets"][0]["compression"] == algorithm
                version = fixture.backup(task["id"])
                assert version["format"] == 5 and version["compression"] == algorithm
                directory = root / "repository/storage/versions" / version["id"]
                entries = [json.loads(line) for line in (directory / "entries.jsonl").read_text().splitlines()]
                physical = 0
                for entry in entries:
                    if entry["type"] != "file":
                        assert "compression" not in entry
                        continue
                    content = directory / (entry["index"] + ".data")
                    assert entry["compression"] == algorithm
                    assert content.stat().st_size == int(entry["stored_size"])
                    assert hashlib.sha256(content.read_bytes()).hexdigest() == entry["stored_sha256"]
                    physical += content.stat().st_size
                assert physical == int(version["stored_bytes"]) < int(plain["stored_bytes"]) / 2
                policy(fixture, "none")
                output = root / (algorithm + "-restore")
                fixture.restore(version["id"], output)
                assert tree(output) == expected
                assert (output / "compressible.bin").stat().st_ino == (output / "hardlink").stat().st_ino
                fixture.restore(plain["id"], root / (algorithm + "-legacy"))
                assert tree(root / (algorithm + "-legacy")) == expected
                corruptions(fixture, version, algorithm)
                transfer_failures(fixture, task, source, algorithm)
                assert tree(source) == expected
                print(f"PASS: {algorithm} saves {physical}/{plain['stored_bytes']} bytes; binary/types, legacy, restart, corruption and lost commit verified", flush=True)

            policy(fixture, "none")
            original_config = (root / "agent-state/config.json").read_bytes()
            for value in ("zstd", 1, True, [], None):
                fixture.request("save_target", {
                    "id": "local", "name": "Invalid", "host": "127.0.0.1",
                    "port": fixture.port, "compression": value,
                }, ok=False)
                assert (root / "agent-state/config.json").read_bytes() == original_config
            token = json.loads((root / "repository/database/access.json").read_text())["token"]
            with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as client:
                rpc(client, "authenticate", {"token": token})
                rpc(client, "begin", {"operation_id": "invalid-codec", "source": str(source),
                    "task_id": task["id"], "compression": "unknown"}, ok=False)
            assert not (root / "repository/storage/staging/invalid-codec").exists()
            fixture.restore(plain["id"], root / "final-legacy")
            assert tree(root / "final-legacy") == expected
            print("PASS: invalid policies fail atomically; raw and compressed versions coexist", flush=True)
        finally:
            fixture.close()


if __name__ == "__main__":
    run(*sys.argv[1:])
