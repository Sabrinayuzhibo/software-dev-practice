#!/usr/bin/env python3
"""Independent BSAR archives, policy conversion and hostile input acceptance."""

import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import sys
import tempfile

from backup_integration import Fixture, stop


def compact(value):
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode()


def run_operation(fixture, action, payload, success=True):
    result = fixture.request("start_" + action, {"target_id": "local", **payload})
    record = fixture.wait(result["operation_id"], success, timeout=90)
    if not success:
        assert record["state"] == "FAILED", record
    return record


def policy(fixture, algorithm):
    fixture.request(
        "save_target",
        {
            "id": "local",
            "name": "Archive target",
            "host": "127.0.0.1",
            "port": fixture.port,
            "compression": algorithm,
        },
    )


def snapshot(root):
    result = {}
    for path in [root, *root.rglob("*")]:
        info = path.lstat()
        if stat.S_ISSOCK(info.st_mode):
            continue
        key = path.relative_to(root).as_posix()
        value = [
            stat.S_IFMT(info.st_mode),
            stat.S_IMODE(info.st_mode),
            info.st_uid,
            info.st_gid,
            info.st_mtime_ns,
        ]
        if stat.S_ISREG(info.st_mode):
            value.append(hashlib.sha256(path.read_bytes()).hexdigest())
        if stat.S_ISLNK(info.st_mode):
            value.append(os.readlink(os.fsencode(path)))
        result[key] = value
    return result


def records(data):
    offset = 8
    result = []
    while offset < len(data):
        kind, size = struct.unpack_from(">IQ", data, offset)
        payload = data[offset + 12 : offset + 12 + size]
        assert len(payload) == size
        if kind == 6:
            break
        result.append((kind, payload))
        offset += 12 + size
    return result


def pack(items):
    body = b"BSAR\0\1\0\0" + b"".join(
        struct.pack(">IQ", kind, len(value)) + value for kind, value in items
    )
    final = struct.pack(">QQ", len(items), len(body)) + hashlib.sha256(body).digest()
    return body + struct.pack(">IQ", 6, len(final)) + final


def corrupt_cases(data):
    items = records(data)
    yield "truncated-header", data[:7]
    yield "truncated-trailer", data[:-1]
    yield "extra-tail", data + b"x"
    yield "wrong-magic", b"XXXX" + data[4:]
    yield "unknown-version", data[:5] + b"\2" + data[6:]
    yield "unknown-flags", data[:7] + b"\1" + data[8:]
    yield "oversized-record", data[:8] + struct.pack(">IQ", 1, 2**64 - 1) + data[20:]
    changed = bytearray(data)
    changed[-1] ^= 1
    yield "wrong-trailer-digest", bytes(changed)
    yield "duplicate-summary", pack(items[:1] + items)
    yield "unknown-record", pack([items[0], (99, b"x"), *items[1:]])
    yield "data-outside-file", pack([items[0], (4, b"x"), *items[1:]])
    yield "unexpected-file-end", pack([items[0], (5, b""), *items[1:]])
    entry_position = next(i for i, (kind, _) in enumerate(items) if kind == 3)
    for label, field, value in [
        ("path-traversal", "path", "../outside"),
        ("absolute-path", "path", "/tmp/outside"),
        ("invalid-mode", "mode", 4096),
        ("invalid-uid", "uid", "4294967296"),
        ("reserved-uid", "uid", "4294967295"),
        ("reserved-gid", "gid", "4294967295"),
        ("invalid-nanoseconds", "mtime_nsec", "1000000000"),
    ]:
        modified = list(items)
        entry = json.loads(modified[entry_position][1])
        entry[field] = value
        modified[entry_position] = (3, compact(entry))
        yield label, pack(modified)
    modified = list(items)
    warning = next((value for kind, value in items if kind == 2), None)
    if warning:
        modified.append((2, warning))
        yield "misplaced-warning", pack(modified)
    first_data = next(i for i, (kind, _) in enumerate(items) if kind == 4)
    modified = list(items)
    raw = bytearray(modified[first_data][1])
    raw[-1] ^= 0x40
    modified[first_data] = (4, bytes(raw))
    yield "content-corruption-valid-envelope", pack(modified)
    yield "missing-data", pack(items[:first_data] + items[first_data + 1 :])
    end = next(i for i, (kind, _) in enumerate(items) if kind == 5)
    yield "missing-file-end", pack(items[:end] + items[end + 1 :])
    modified = list(items)
    summary = json.loads(modified[0][1])
    summary["warning_count"] += 1
    modified[0] = (1, compact(summary))
    yield "wrong-warning-count", pack(modified)
    hard = next(
        i
        for i, (kind, value) in enumerate(items)
        if kind == 3 and json.loads(value)["type"] == "hardlink"
    )
    modified = list(items)
    entry = json.loads(modified[hard][1])
    entry["link_to"] = "missing"
    modified[hard] = (3, compact(entry))
    yield "hardlink-outside-group", pack(modified)


def legacy_socket(fixture, version):
    folder = fixture.root / "repository/storage/versions/legacy-archive-socket"
    shutil.copytree(
        fixture.root / "repository/storage/versions" / version["id"], folder
    )
    summary = json.loads((folder / "summary.json").read_text())
    entries = [
        json.loads(line)
        for line in (folder / "entries.jsonl").read_bytes().splitlines()
    ]
    # 格式 3 早于元数据扩展，所有条目都不含格式 4 才引入的属性。
    for entry in entries:
        for field in ("mode", "uid", "gid", "mtime_sec", "mtime_nsec"):
            entry.pop(field, None)
    summary.pop("root_metadata", None)
    summary["file_types"] = ["file", "directory", "symlink", "fifo", "socket"]
    summary.setdefault("rules", {})["file_types"] = summary["file_types"]
    entries.append(
        {"path": "legacy.socket", "type": "socket", "index": str(len(entries))}
    )
    manifest = b"".join(compact(entry) + b"\n" for entry in entries)
    summary.update(
        id=folder.name,
        format=3,
        entries=str(len(entries)),
        sockets="1",
        manifest_sha256=hashlib.sha256(manifest).hexdigest(),
    )
    (folder / "entries.jsonl").write_bytes(manifest)
    (folder / "summary.json").write_bytes(compact(summary))
    return folder.name


def prepare_source(root):
    source = root / "source"
    (source / "只读/empty").mkdir(parents=True)
    (source / "data").write_bytes(b"abcdef" * (64 * 1024))
    (source / "random").write_bytes(os.urandom(64 * 1024 + 9))
    (source / "empty").touch()
    (source / ".hidden").write_bytes(b"hidden")
    long_dir = source / ("a" * 90) / ("b" * 90)
    long_dir.mkdir(parents=True)
    (long_dir / ("文" * 70)).write_bytes(b"long path")
    os.link(source / "data", source / "hard")
    (source / "link").symlink_to("data")
    (source / "outside-link").symlink_to("/does-not-exist")
    os.symlink(b"raw-\xff", os.fsencode(source / "raw-link"))
    os.mkfifo(source / "pipe")
    for index in range(103):
        with socket.socket(socket.AF_UNIX) as node:
            node.bind(str(source / f"socket{index}"))
    for index, path in enumerate([*source.rglob("*"), source]):
        if stat.S_ISSOCK(path.lstat().st_mode):
            continue
        if not path.is_symlink():
            path.chmod(0o500 if path.is_dir() else 0o640)
        os.utime(
            path,
            ns=(946684800123456789 + index, 946684800123456789 + index),
            follow_symlinks=False,
        )
    expected = snapshot(source)
    return source, expected


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-archive-") as temporary:
        root = Path(temporary)
        original = Fixture(root / "original", agent, server)
        target = Fixture(root / "target", agent, server)
        try:
            source, expected = prepare_source(root)
            task = original.request(
                "add_task", {"path": str(source), "target_id": "local"}
            )
            archives = {}
            versions = {}
            for algorithm in ("none", "huffman", "lz77"):
                policy(original, algorithm)
                version = original.backup(task["id"])
                versions[algorithm] = version
                archive = root / f"standalone-{algorithm}.bsar"
                run_operation(
                    original,
                    "export_archive",
                    {"version_id": version["id"], "archive_path": str(archive)},
                )
                assert archive.read_bytes()[:8] == b"BSAR\0\1\0\0"
                archives[algorithm] = archive
            sentinel = root / "existing.bsar"
            sentinel.write_bytes(b"keep")
            run_operation(
                original,
                "export_archive",
                {"version_id": version["id"], "archive_path": str(sentinel)},
                False,
            )
            assert sentinel.read_bytes() == b"keep"
            (root / "archive-link").symlink_to(sentinel)
            for destination in (
                root / "archive-link",
                source / "bad.bsar",
                original.root / "repository/bad.bsar",
                original.root / "agent-state/bad.bsar",
            ):
                run_operation(
                    original,
                    "export_archive",
                    {"version_id": version["id"], "archive_path": str(destination)},
                    False,
                )
            selected_source = root / "selected-source"
            (selected_source / "sub").mkdir(parents=True)
            (selected_source / "keep.txt").write_text("selected")
            (selected_source / "skip.log").write_text("excluded")
            (selected_source / "sub/nested.txt").write_text("nested")
            selected_task = original.request(
                "add_task",
                {
                    "sources": [
                        str(selected_source / "keep.txt"),
                        str(selected_source / "skip.log"),
                        str(selected_source / "sub"),
                    ],
                    "target_id": "local",
                    "file_types": ["file"],
                    "preserve_empty_dirs": False,
                    "filters": {
                        "include": [{"category": "name", "pattern": "*.txt"}],
                        "exclude": [],
                    },
                },
            )
            selected_version = original.backup(selected_task["id"])
            selected_archive = root / "selected.bsar"
            run_operation(
                original,
                "export_archive",
                {
                    "version_id": selected_version["id"],
                    "archive_path": str(selected_archive),
                },
            )
            legacy = legacy_socket(original, versions["none"])
            legacy_archive = root / "legacy.bsar"
            run_operation(
                original,
                "export_archive",
                {"version_id": legacy, "archive_path": str(legacy_archive)},
            )
            original.close()
            original.agent = original.server = None
            (original.root / "repository").rename(
                original.root / "unavailable-repository"
            )
            source.rename(root / "unavailable-source")
            selected_source.rename(root / "unavailable-selected-source")
            imported_ids = []
            for source_policy, archive in archives.items():
                for destination_policy in ("none", "huffman", "lz77"):
                    policy(target, destination_policy)
                    record = run_operation(
                        target, "import_archive", {"archive_path": str(archive)}
                    )
                    imported = record["result"]["version"]
                    assert imported["id"] != versions[source_policy]["id"]
                    assert imported.get("compression", "none") == destination_policy
                    assert (
                        imported["warning_count"] == 103
                        and record["warning_count"] == 103
                    )
                    assert (
                        imported["imported_from"]["version_id"]
                        == versions[source_policy]["id"]
                    )
                    output = root / f"restored-{source_policy}-{destination_policy}"
                    target.restore(imported["id"], output)
                    assert snapshot(output) == expected, (
                        source_policy,
                        destination_policy,
                    )
                    assert (output / "data").stat().st_ino == (
                        output / "hard"
                    ).stat().st_ino
                    imported_ids.append(imported["id"])
            policy(target, "none")
            legacy_result = run_operation(
                target, "import_archive", {"archive_path": str(legacy_archive)}
            )
            legacy_output = root / "restored-legacy"
            target.restore(legacy_result["result"]["version"]["id"], legacy_output)
            assert stat.S_ISSOCK((legacy_output / "legacy.socket").lstat().st_mode)
            policy(target, "huffman")
            selected = run_operation(
                target, "import_archive", {"archive_path": str(selected_archive)}
            )["result"]["version"]
            for field in ("selection", "filters", "file_types", "preserve_empty_dirs"):
                assert selected[field] == selected_version[field], field
            selected_output = root / "selected-restored"
            target.restore(selected["id"], selected_output)
            assert sorted(
                path.relative_to(selected_output).as_posix()
                for path in selected_output.rglob("*")
                if path.is_file()
            ) == ["keep.txt", "sub/nested.txt"]
            empty_source = root / "empty-source"
            empty_source.mkdir()
            empty_task = target.request(
                "add_task", {"path": str(empty_source), "target_id": "local"}
            )
            empty_version = target.backup(empty_task["id"])
            empty_archive = root / "empty.bsar"
            run_operation(
                target,
                "export_archive",
                {"version_id": empty_version["id"], "archive_path": str(empty_archive)},
            )
            empty = run_operation(
                target, "import_archive", {"archive_path": str(empty_archive)}
            )["result"]["version"]
            target.restore(empty["id"], root / "empty-restored")
            assert snapshot(root / "empty-restored") == snapshot(empty_source)
            input_fifo = root / "input-fifo"
            os.mkfifo(input_fifo)
            for invalid in (
                root / "missing.bsar",
                root / "archive-link",
                root,
                input_fifo,
            ):
                run_operation(
                    target, "import_archive", {"archive_path": str(invalid)}, False
                )
            # 支持的属性保留数值语义，未支持的扩展不进入目标版本摘要。
            extended = records(archives["none"].read_bytes())
            extended_summary = json.loads(extended[0][1])
            extended_summary["root_metadata"]["ignored_extension"] = "x" * 50000
            extended[0] = (1, compact(extended_summary))
            extended_path = root / "extended-metadata.bsar"
            extended_path.write_bytes(pack(extended))
            normalized = run_operation(
                target, "import_archive", {"archive_path": str(extended_path)}
            )["result"]["version"]
            assert "ignored_extension" not in normalized["root_metadata"]
            target.restore(normalized["id"], root / "normalized-metadata")
            assert snapshot(root / "normalized-metadata") == expected
            before = target.request("versions", {"target_id": "local"})["total"]
            checked = 0
            for name, data in corrupt_cases(archives["none"].read_bytes()):
                path = root / f"invalid-{name}.bsar"
                path.write_bytes(data)
                run_operation(
                    target, "import_archive", {"archive_path": str(path)}, False
                )
                assert (
                    target.request("versions", {"target_id": "local"})["total"]
                    == before
                ), name
                checked += 1
            for algorithm in ("huffman", "lz77"):
                bad = dict(corrupt_cases(archives[algorithm].read_bytes()))[
                    "content-corruption-valid-envelope"
                ]
                path = root / f"corrupt-{algorithm}.bsar"
                path.write_bytes(bad)
                run_operation(
                    target, "import_archive", {"archive_path": str(path)}, False
                )
            # 原仓库和来源仍不可用，Agent/Server 重启后只依赖导入的新版本还原。
            stop(target.agent)
            target.agent = None
            stop(target.server)
            target.server = None
            target.start_server()
            target.start_agent()
            target.restore(imported_ids[0], root / "after-restart")
            assert snapshot(root / "after-restart") == expected
            print(
                f"PASS: standalone archives, 9 codec conversions, legacy socket, 103 warnings, metadata/links, {checked + 2} malformed archives, protected outputs and restart"
            )
        finally:
            # 测试样本中的只读目录仅在清理前恢复遍历/写入权限。
            for path in root.rglob("*"):
                if not path.is_symlink() and path.is_dir():
                    path.chmod(0o700)
            if original.agent or original.server:
                original.close()
            target.close()


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()))
