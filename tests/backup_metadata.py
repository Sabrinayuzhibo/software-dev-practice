#!/usr/bin/env python3
"""FR-12：真实元数据往返、受限目录、历史兼容及失败警告持久化。"""

import argparse
import socket
import uuid
import json
import os
from pathlib import Path
import stat
import tempfile

from backup_file_types import manifest_path, rewrite_manifest
from backup_integration import Fixture, rpc, stop, tree

STAMP = 946684800123456789


def attributes(path):
    info = path.lstat()
    return (stat.S_IMODE(info.st_mode), info.st_uid, info.st_gid, info.st_mtime_ns)


def samples(root):
    source = root / "source"
    (source / "readonly/nested").mkdir(parents=True)
    (source / "empty-dir").mkdir()
    for name, size, mode in [
        ("small.txt", 17, 0o640),
        ("empty", 0, 0o400),
        ("large", 300123, 0o751),
        ("readonly/nested/member", 8191, 0o444),
    ]:
        path = source / name
        payload = b"metadata payload\x00"
        path.write_bytes((payload * (size // len(payload) + 1))[:size])
        path.chmod(mode)
    os.link(source / "readonly/nested/member", source / "readonly/alias")
    os.mkfifo(source / "pipe", 0o600)
    outside = root / "outside.txt"
    outside.write_text("outside stays unchanged")
    (source / "outside-link").symlink_to(outside)
    (source / "dangling-link").symlink_to("missing")
    # 同时覆盖小文件缓冲、跨块文件、纳秒、纪元前时间和不同可恢复属组。
    groups = [group for group in os.getgroups() if group != os.getgid()]
    if groups:
        os.chown(source / "small.txt", -1, groups[0])
    for index, path in enumerate(sorted(source.rglob("*"))):
        stamp = -123456789 if path.name == "empty" else STAMP + index * 29
        os.utime(path, ns=(stamp, stamp), follow_symlinks=False)
    (source / "readonly").chmod(0o500)
    (source / "readonly/nested").chmod(0o500)
    source.chmod(0o750)
    os.utime(source, ns=(STAMP, STAMP))
    expected = {".": attributes(source)}
    expected.update(
        {str(path.relative_to(source)): attributes(path) for path in source.rglob("*")}
    )
    return source, expected, outside, attributes(outside)


def warnings(fixture, record):
    result, offset = [], 0
    while offset is not None:
        page = fixture.request(
            "operation_warnings", {"id": record["id"], "offset": offset}
        )
        result.extend(page["warnings"])
        offset = page["next_offset"]
    assert len(result) == record["warning_count"]
    return result


def roundtrips(fixture, source, expected, outside, outside_before):
    task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
    original = tree(source)
    last = None
    for algorithm in ("none", "huffman", "lz77"):
        fixture.request(
            "save_target",
            {
                "id": "local",
                "name": "Metadata",
                "host": "127.0.0.1",
                "port": fixture.port,
                "compression": algorithm,
            },
        )
        version = fixture.backup(task["id"])
        destination = fixture.root / ("restored-" + algorithm)
        record = fixture.restore(version["id"], destination)
        assert record["state"] == "SUCCEEDED", record
        assert tree(destination) == original
        for name, wanted in expected.items():
            actual = attributes(destination if name == "." else destination / name)
            assert actual == wanted, (
                algorithm,
                name,
                "mode/uid/gid/mtime",
                actual,
                wanted,
            )
        assert (destination / "readonly/alias").stat().st_ino == (
            destination / "readonly/nested/member"
        ).stat().st_ino
        assert attributes(outside) == outside_before
        assert outside.read_text() == "outside stays unchanged"
        last = version
        print(
            f"PASS: {algorithm} 全部类型和根目录的 rwx/UID/GID/mtime（纳秒）一致，外部链接目标未变",
            flush=True,
        )
    return last


def restricted_directories(fixture, version):
    directory = manifest_path(fixture, version)
    manifest = (directory / "entries.jsonl").read_bytes()
    summary = (directory / "summary.json").read_bytes()
    entries = [json.loads(line) for line in manifest.splitlines()]
    # 使用有效清单模拟其他权限环境中生成的版本，覆盖无法再遍历的目录。
    for entry in entries:
        if entry["path"] == "readonly":
            entry["mode"] = 0o400
    try:
        # 此场景只改元数据，保留原格式和压缩字段。
        from hashlib import sha256

        data = b"".join(
            json.dumps(entry, sort_keys=True, separators=(",", ":")).encode() + b"\n"
            for entry in entries
        )
        (directory / "entries.jsonl").write_bytes(data)
        changed = json.loads(summary)
        changed["manifest_sha256"] = sha256(data).hexdigest()
        (directory / "summary.json").write_text(json.dumps(changed))
        destination = fixture.root / "restricted-restore"
        try:
            record = fixture.restore(version["id"], destination)
            assert record["state"] == "SUCCEEDED", record
            assert stat.S_IMODE((destination / "readonly").stat().st_mode) == 0o400
        finally:
            if (destination / "readonly").exists():
                (destination / "readonly").chmod(0o700)
        print("PASS: 硬链接在收紧目录遍历权限前完成校验", flush=True)
    finally:
        (directory / "entries.jsonl").write_bytes(manifest)
        (directory / "summary.json").write_bytes(summary)


def fault_warnings(fixture, library):
    source = fixture.root / "warning-source"
    source.mkdir()
    for index in range(105):
        (source / f"file-{index:03}").write_text("verified data")
    (source / "directory").mkdir()
    os.mkfifo(source / "fifo")
    (source / "symlink").symlink_to("file-000")
    with socket.socket(socket.AF_UNIX) as endpoint:
        endpoint.bind(str(source / "socket"))
    task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
    version = fixture.backup(task["id"])
    stop(fixture.agent)
    fixture.agent = None
    previous = {
        key: os.environ.get(key) for key in ("LD_PRELOAD", "BACKUP_TEST_METADATA_FAULT")
    }
    try:
        os.environ["LD_PRELOAD"] = library
        os.environ["BACKUP_TEST_METADATA_FAULT"] = "1"
        fixture.start_agent()
        record = fixture.restore(
            version["id"], fixture.root / "metadata-warning-restore"
        )
    finally:
        stop(fixture.agent)
        fixture.agent = None
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        fixture.start_agent()
    assert record["state"] == "SUCCEEDED_WITH_WARNINGS", record
    # 每个文件和根目录的属主、权限、mtime 均注入失败，超过单页条数。
    expected_count = 106 * 3 + 3 + 3 + 2
    assert record["result"]["metadata_warning_count"] == expected_count, record
    items = warnings(fixture, record)
    assert len(items) == expected_count + 1
    metadata = [item for item in items if item["type"] == "metadata"]
    assert len(metadata) == expected_count
    assert {item["path"] for item in metadata} == {
        ".",
        "directory",
        "fifo",
        "symlink",
    } | {f"file-{i:03}" for i in range(105)}
    assert all(
        item["type"] == "metadata" and item["field"] and item["reason"]
        for item in metadata
    )
    assert tree(fixture.root / "metadata-warning-restore") == tree(source)
    persisted = fixture.request("operation", {"id": record["id"]})
    assert persisted["state"] == record["state"]
    assert persisted["result"]["metadata_warning_count"] == expected_count
    assert warnings(fixture, persisted) == items
    print(
        "PASS: 326 项元数据失败 + 1 项源 socket 警告完整分页，含根目录/链接/管道/目录，重启后可查",
        flush=True,
    )


def legacy_and_corruption(fixture):
    source = fixture.root / "legacy-source"
    source.mkdir()
    (source / "file").write_bytes(b"legacy data")
    os.link(source / "file", source / "alias")
    fixture.request(
        "save_target",
        {
            "id": "local",
            "name": "Legacy",
            "host": "127.0.0.1",
            "port": fixture.port,
            "compression": "none",
        },
    )
    task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
    version = fixture.backup(task["id"])
    folder = manifest_path(fixture, version)
    manifest, summary = (folder / "entries.jsonl").read_bytes(), (
        folder / "summary.json"
    ).read_bytes()
    entries = [json.loads(line) for line in manifest.splitlines()]
    bad_entries = []
    for field, value in [
        ("mode", 4095),
        ("uid", "4294967296"),
        ("gid", "-1"),
        ("mtime_nsec", "1000000000"),
        ("mtime_sec", "NaN"),
    ]:
        changed = [dict(entry) for entry in entries]
        changed[0][field] = value
        bad_entries.append(changed)
    incomplete = [dict(entry) for entry in entries]
    incomplete[0].pop("mtime_nsec")
    bad_entries.append(incomplete)
    conflict = [dict(entry) for entry in entries]
    conflict[1]["mode"] = 0o400 if conflict[0]["mode"] != 0o400 else 0o600
    bad_entries.append(conflict)
    try:
        for index, changed in enumerate(bad_entries):
            rewrite_manifest(folder, changed)
            destination = fixture.root / f"bad-entry-{index}"
            fixture.restore(version["id"], destination, success=False)
            assert not destination.exists()
        (folder / "entries.jsonl").write_bytes(manifest)
        for index, bad in enumerate(
            [
                {},
                [],
                {"mode": 4095},
                {**json.loads(summary)["root_metadata"], "uid": "4294967296"},
            ]
        ):
            changed = json.loads(summary)
            changed["root_metadata"] = bad
            (folder / "summary.json").write_text(json.dumps(changed))
            destination = fixture.root / f"bad-root-{index}"
            fixture.restore(version["id"], destination, success=False)
            assert not destination.exists()
        (folder / "summary.json").write_bytes(summary)
        for version_format in (2, 3, 4):
            rewrite_manifest(folder, entries, version_format)
            if version_format == 4:
                # 格式 4 也曾允许缺少整组元数据。
                legacy = [
                    {
                        key: value
                        for key, value in entry.items()
                        if key not in ("mode", "uid", "gid", "mtime_sec", "mtime_nsec")
                    }
                    for entry in entries
                ]
                rewrite_manifest(folder, legacy, version_format)
            changed = json.loads((folder / "summary.json").read_text())
            changed.pop("root_metadata", None)
            (folder / "summary.json").write_text(json.dumps(changed))
            destination = fixture.root / f"legacy-{version_format}"
            record = fixture.restore(version["id"], destination)
            assert record["state"] == "SUCCEEDED", record
            assert tree(destination) == tree(source)
    finally:
        (folder / "entries.jsonl").write_bytes(manifest)
        (folder / "summary.json").write_bytes(summary)
    token = json.loads((fixture.root / "repository/database/access.json").read_text())[
        "token"
    ]
    for bad in ({}, {"mode": 4095}, []):
        with socket.create_connection(
            ("127.0.0.1", fixture.port), timeout=5
        ) as connection:
            rpc(connection, "authenticate", {"token": token})
            rpc(
                connection,
                "begin",
                {
                    "source": str(source),
                    "task_id": task["id"],
                    "operation_id": str(uuid.uuid4()),
                    "root_metadata": bad,
                },
                ok=False,
            )
    print(
        "PASS: 无元数据的格式 2/3/4 兼容；11 组损坏/冲突元数据在创建还原目标前拒绝，Server 拒绝非法根元数据",
        flush=True,
    )


def run(root, agent, server, library):
    source, expected, outside, outside_before = samples(root)
    fixture = Fixture(root, agent, server)
    try:
        version = roundtrips(fixture, source, expected, outside, outside_before)
        restricted_directories(fixture, version)
        legacy_and_corruption(fixture)
        fault_warnings(fixture, library)
    finally:
        fixture.close()


def make_writable(root):
    # 临时样本清理前放开目录写权限，保留的验收目录不执行此步骤。
    for parent, directories, _ in os.walk(root):
        for name in directories:
            path = Path(parent) / name
            if not path.is_symlink():
                path.chmod(0o700)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("agent")
    parser.add_argument("server")
    parser.add_argument("library")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    binaries = [
        str(Path(item).resolve()) for item in (args.agent, args.server, args.library)
    ]
    if args.output:
        args.output.mkdir(parents=True)
        run(args.output.resolve(), *binaries)
    else:
        with tempfile.TemporaryDirectory(prefix="backup-metadata-") as folder:
            try:
                run(Path(folder), *binaries)
            finally:
                make_writable(Path(folder))
