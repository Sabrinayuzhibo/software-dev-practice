#!/usr/bin/env python3
"""创建保留在磁盘上的混合样本，逐项核对扫描、版本清单和真实还原。"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys

from backup_integration import Fixture, stop, tree
from backup_custom_filter import filters, rule


BASE_TIME = 1767225600


def make_source(root):
    source = root / "source"
    for name in ["docs/nested", "cache", "empty", "中文 目录/空目录", "links", "special", "binary"]:
        (source / name).mkdir(parents=True, exist_ok=True)
    specifications = {
        "docs/a.txt": (10, 0), "docs/b.txt": (20, 100),
        "docs/below.txt": (9, -1), "docs/above.txt": (21, 101),
        "docs/zero.txt": (0, 0), "docs/UPPER.TXT": (15, 0),
        "docs/readme.md": (15, 0), "docs/.hidden.txt": (15, 0),
        "docs/nested/c.txt": (15, 50), "docs/line\nbreak.txt": (15, 0),
        "中文 目录/报告.txt": (15, 0), "cache/a.txt": (15, 0),
        "binary/data.bin": (1024, 0), "binary/big.bin": (1024 * 1024, 0),
        "large.txt": (65536, 0), "root.txt": (15, 0),
    }
    for name, (size, offset) in specifications.items():
        path = source / name
        content = (name.encode() + b"\x00\xff\n")
        path.write_bytes((content * (size // len(content) + 1))[:size])
        os.utime(path, (BASE_TIME + offset, BASE_TIME + offset))
    os.link(source / "docs/a.txt", source / "docs/a-hard.txt")
    for name, target in [("file-link", "../docs/a.txt"), ("dir-link", "../docs"),
                         ("dangling", "missing"), ("cycle", "../links")]:
        (source / "links" / name).symlink_to(target)
    os.mkfifo(source / "special/events.pipe")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as endpoint:
        endpoint.bind(str(source / "special/app.socket"))
    for directory, _, _ in os.walk(source):
        os.utime(directory, (BASE_TIME, BASE_TIME))
    inventory = []
    for path in sorted(source.rglob("*")):
        info = path.lstat()
        entry = {"path": str(path.relative_to(source)), "uid": info.st_uid,
                 "mtime": info.st_mtime_ns, "bytes": info.st_size,
                 "mode": oct(info.st_mode)}
        if stat.S_ISREG(info.st_mode):
            entry["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        elif path.is_symlink():
            entry["target"] = os.readlink(path)
        inventory.append(entry)
    (root / "source-inventory.json").write_text(json.dumps(inventory, ensure_ascii=False, indent=2))
    return source


def cases_for(source):
    originals = tree(source)
    regular = {p for p, value in originals.items() if isinstance(value, str) and len(value) == 64}
    plain = regular - {"docs/a.txt", "docs/a-hard.txt"}
    links = {"links/file-link", "links/dir-link", "links/dangling", "links/cycle"}
    pipes = {"special/events.pipe"}
    all_files = regular | links | pipes
    text = {p for p in regular if p.endswith(".txt")}
    docs = {p for p in regular if p.startswith("docs/")}
    small = {"docs/a.txt", "docs/a-hard.txt", "docs/b.txt", "docs/UPPER.TXT",
             "docs/readme.md", "docs/.hidden.txt", "docs/nested/c.txt",
             "docs/line\nbreak.txt", "中文 目录/报告.txt", "cache/a.txt", "root.txt"}
    uid = os.getuid()
    return [
        ("01-all", filters(), all_files),
        ("02-path-subtree", filters([rule("path", pattern="docs/")]), docs),
        ("03-path-no-cross", filters([rule("path", pattern="docs/*.txt")]), {p for p in text & docs if p.count("/") == 1}),
        ("04-path-exclude", filters([], [rule("path", pattern="docs/")]), all_files - docs),
        ("05-type-file", filters([rule("type", value="file")]), plain),
        ("06-type-links", filters([rule("type", value="symlink")]), links),
        ("07-type-fifo", filters([rule("type", value="fifo")]), pipes),
        ("08-type-hardlink", filters([rule("type", value="hardlink")]), {"docs/a.txt", "docs/a-hard.txt"}),
        ("09-type-exclude", filters([], [rule("type", value="symlink"), rule("type", value="fifo")]), regular),
        ("10-name-txt", filters([rule("name", pattern="*.txt")]), text),
        ("11-name-question", filters([rule("name", pattern="?.txt")]), {"docs/a.txt", "docs/b.txt", "docs/nested/c.txt", "cache/a.txt"}),
        ("12-name-or", filters([rule("name", pattern="a.txt"), rule("name", pattern="readme.md")]), {"docs/a.txt", "cache/a.txt", "docs/readme.md"}),
        ("13-name-chinese", filters([rule("name", pattern="报告.txt")]), {"中文 目录/报告.txt"}),
        ("14-name-hidden", filters([rule("name", pattern=".hidden.txt")]), {"docs/.hidden.txt"}),
        ("15-name-case", filters([rule("name", pattern="*.TXT")]), {"docs/UPPER.TXT"}),
        ("16-name-exclude-subtree", filters([], [rule("name", pattern="cache")]), all_files - {"cache/a.txt"}),
        ("17-size-closed", filters([rule("size", min=10, max=20)]), small),
        ("18-size-zero", filters([rule("size", min=0, max=0)]), {"docs/zero.txt"}),
        ("19-size-min-only", filters([rule("size", min=65536)]), {"large.txt", "binary/big.bin"}),
        ("20-size-max-only", filters([rule("size", max=9)]), {"docs/zero.txt", "docs/below.txt"}),
        ("21-size-exclude", filters([], [rule("size", min=10, max=20)]), all_files - small),
        ("22-time-closed", filters([rule("type", value="file"), rule("mtime", min=BASE_TIME, max=BASE_TIME + 100)]), plain - {"docs/below.txt", "docs/above.txt"}),
        ("23-time-exact", filters([rule("type", value="file"), rule("mtime", min=BASE_TIME + 100, max=BASE_TIME + 100)]), {"docs/b.txt"}),
        ("24-time-min-only", filters([rule("type", value="file"), rule("mtime", min=BASE_TIME + 101)]), {"docs/above.txt"}),
        ("25-time-max-only", filters([rule("type", value="file"), rule("mtime", max=BASE_TIME - 1)]), {"docs/below.txt"}),
        ("26-time-exclude", filters([], [rule("mtime", min=BASE_TIME + 100, max=BASE_TIME + 101)]), all_files - {"docs/b.txt", "docs/above.txt"}),
        ("27-user-current", filters([rule("user", value=uid)]), all_files),
        ("28-user-other", filters([rule("user", value=uid + 1)]), set()),
        ("29-user-exclude-current", filters([], [rule("user", value=uid)]), set()),
        ("30-combined-six", filters([rule("path", pattern="docs/"), rule("type", value="file"), rule("name", pattern="?.txt"), rule("size", min=10, max=20), rule("mtime", min=BASE_TIME, max=BASE_TIME + 100), rule("user", value=uid)], [rule("name", pattern="b.txt")]), {"docs/nested/c.txt"}),
        ("31-empty-match", filters([rule("name", pattern="does-not-exist")]), set()),
        ("32-include-exclude-same", filters([rule("path", pattern="docs/")], [rule("path", pattern="docs/")]), set()),
        ("33-directory-only", filters([rule("type", value="directory"), rule("name", pattern="空目录")]), set()),
    ]


def verify_case(fixture, task, source, label, rules, expected):
    fixture.request("update_task_filters", {"id": task["id"], "filters": rules})
    scan = fixture.request("scan", {"task_id": task["id"]})
    assert scan["complete"], scan
    scanned = {e["path"] for e in scan["sample"] if e["type"] != "directory" and "reason" not in e}
    assert scanned == expected, (label, "scan", scanned, expected)
    version = fixture.backup(task["id"])
    entries = fixture.root / "repository/storage/versions" / version["id"] / "entries.jsonl"
    manifest = [json.loads(line) for line in entries.read_text().splitlines()]
    assert {e["path"] for e in manifest if e["type"] != "directory"} == expected, label
    destination = fixture.root / "restored" / label
    destination.parent.mkdir(exist_ok=True)
    fixture.restore(version["id"], destination)
    original = tree(source)
    restored = tree(destination)
    actual = {p for p, value in restored.items() if value != "directory"}
    assert actual == expected, (label, "restore", actual, expected)
    by_path = {entry["path"]: entry for entry in manifest}
    for path in expected:
        assert restored[path] == original[path], (label, "content/type", path)
        if (source / path).is_file() and not (source / path).is_symlink():
            assert int(by_path[path]["mtime_sec"]) == (source / path).stat().st_mtime_ns // 10**9
            assert (destination / path).stat().st_mtime_ns == (source / path).stat().st_mtime_ns, (label, "mtime", path)
    if label in ("01-all", "08-type-hardlink"):
        assert (destination / "docs/a.txt").stat().st_ino == (destination / "docs/a-hard.txt").stat().st_ino
    if label == "01-all":
        assert (destination / "empty").is_dir()
        assert not os.path.lexists(destination / "special/app.socket")
        assert any(w["type"] == "socket" for w in version["warnings"])
    if label == "33-directory-only":
        assert restored == {"中文 目录": "directory", "中文 目录/空目录": "directory"}
    result = {"case": label, "filters": rules, "expected": sorted(expected),
              "actual": sorted(actual), "version": version["id"], "passed": True,
              "mtime_verified": True}
    print(f"PASS {label}: {len(expected)} 个非目录条目", flush=True)
    return result


def check_ownership(root, agent, server):
    """只在私有用户/挂载命名空间绑定只读样本，不更改宿主文件属主。"""
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    root.mkdir()
    source = root / "source"
    source.mkdir()
    local = source / "local.txt"
    local.write_text("namespace local owner\n")
    foreign = source / "foreign.txt"
    foreign.touch()
    subprocess.run(["mount", "--bind", "/etc/hostname", str(foreign)], check=True)
    fixture = None
    try:
        subprocess.run(["mount", "-o", "remount,bind,ro,nosuid,nodev,noexec", str(foreign)], check=True)
        local_uid, foreign_uid = local.stat().st_uid, foreign.stat().st_uid
        assert local_uid != foreign_uid
        fixture = Fixture(root, agent, server)
        task = fixture.request("add_task", {"sources": [str(source)], "target_id": "local", "file_types": ["file"], "preserve_empty_dirs": False})
        results = []
        for label, rules, expected in [
            ("34-foreign-user", filters([rule("user", value=foreign_uid)]), {"foreign.txt"}),
            ("35-user-or", filters([rule("user", value=local_uid), rule("user", value=foreign_uid)]), {"local.txt", "foreign.txt"}),
            ("36-user-exclude-foreign", filters([], [rule("user", value=foreign_uid)]), {"local.txt"}),
        ]:
            results.append(verify_case(fixture, task, source, label, rules, expected))
        (root / "results.json").write_text(json.dumps({"uids": [local_uid, foreign_uid], "cases": results}, indent=2))
    finally:
        if fixture:
            fixture.close()
        subprocess.run(["umount", str(foreign)], check=True)


def run(root, agent, server):
    root.mkdir(parents=True)
    source = make_source(root)
    fixture = Fixture(root, agent, server)
    results = []
    try:
        task = fixture.request("add_task", {"sources": [str(source)], "target_id": "local", "file_types": ["file", "symlink", "fifo"], "preserve_empty_dirs": True})
        for label, rules, expected in cases_for(source):
            results.append(verify_case(fixture, task, source, label, rules, expected))
        compressed_rules = filters([rule("name", pattern="large.txt")])
        for algorithm in ["huffman", "lz77"]:
            fixture.request("save_target", {"id": "local", "name": "Test", "host": "127.0.0.1", "port": fixture.port, "compression": algorithm})
            label = f"compression-{algorithm}"
            results.append(verify_case(fixture, task, source, label, compressed_rules, {"large.txt"}))
        stop(fixture.agent)
        fixture.agent = None
        fixture.start_agent()
        assert fixture.request("config")["tasks"][0]["filters"] == compressed_rules
        historical = results[1]
        fixture.restore(historical["version"], root / "restored/historical-after-restart")
        historical_tree = tree(root / "restored/historical-after-restart")
        assert {p for p, value in historical_tree.items() if value != "directory"} == set(historical["expected"])
        print("PASS restart-history: 重启后规则保留，旧版本按历史规则还原", flush=True)
    finally:
        fixture.close()
        (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2))
    print(f"证据目录：{root}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("agent")
    parser.add_argument("server")
    parser.add_argument("output", type=Path)
    parser.add_argument("--ownership", action="store_true")
    args = parser.parse_args()
    action = check_ownership if args.ownership else run
    action(args.output.resolve(), str(Path(args.agent).resolve()), str(Path(args.server).resolve()))
