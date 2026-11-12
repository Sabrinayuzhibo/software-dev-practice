#!/usr/bin/env python3
"""六类规则的真实扫描、存储、还原、编辑及持久化边界。"""

import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import uuid

from backup_integration import Fixture, rpc, stop, tree
from backup_file_types import rewrite_manifest


def rule(category, **values):
    return {"category": category, **{key: str(value) for key, value in values.items()}}


def filters(include=(), exclude=()):
    return {"include": list(include), "exclude": list(exclude)}


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-custom-filter-") as temporary:
        root = Path(temporary)
        source = root / "source"
        (source / "sub").mkdir(parents=True)
        (source / "cache").mkdir()
        uid = os.getuid()
        other_uid = uid + 1
        m1, m2 = 1788220800, 1788307200
        samples = {
            "sub/a.txt": (10, m1), "sub/b.txt": (20, m2),
            "sub/c.txt": (9, m1), "sub/d.txt": (21, m2),
            "sub/e.log": (15, m1), "sub/f.txt": (15, m1),
            "sub/old.txt": (15, m1 - 1), "cache/a.txt": (15, m1),
        }
        for path, (size, mtime) in samples.items():
            item = source / path
            item.write_bytes(b"x" * size)
            os.utime(item, (mtime, mtime))
        alternate_owner = True
        try:
            os.chown(source / "sub/f.txt", other_uid, -1)
        except PermissionError:
            alternate_owner = False
            print("LIMIT: 无 chown 权限，B-02 的其他 UID 真文件场景未执行；单测覆盖不同 UID 匹配。", flush=True)
        (source / "link.txt").symlink_to("sub/a.txt")
        os.mkfifo(source / "pipe.txt")
        combined = [rule("path", pattern="sub/"), rule("path", pattern="cache/"),
                    rule("type", value="file"), rule("name", pattern="*.txt"),
                    rule("size", min=10, max=20), rule("mtime", min=m1, max=m2),
                    rule("user", value=uid)]
        excluded = [rule("path", pattern="cache/")]
        ordinary = {path for path in samples if path.startswith("sub/")}
        cases = [
            ("B-01", filters(combined, excluded), {"sub/a.txt", "sub/b.txt"} | (set() if alternate_owner else {"sub/f.txt"})),
            ("B-02", filters([item if item["category"] != "user" else rule("user", value=other_uid) for item in combined], excluded), {"sub/f.txt"} if alternate_owner else set()),
            ("B-03", filters([rule("type", value="symlink"), rule("type", value="fifo")]), {"link.txt", "pipe.txt"}),
            ("B-04", filters([rule("path", pattern="sub/"), rule("type", value="file"), rule("name", pattern="?.txt")]), {f"sub/{name}.txt" for name in "abcdf"}),
            ("B-05", filters([rule("path", pattern="sub/"), rule("type", value="file"), rule("name", pattern="a.txt"), rule("name", pattern="e.log")]), {"sub/a.txt", "sub/e.log"}),
            ("B-06", filters(combined, excluded + [rule("name", pattern="b.txt")]), {"sub/a.txt"} | (set() if alternate_owner else {"sub/f.txt"})),
            ("B-07", filters([rule("path", pattern="sub/")], [rule("name", pattern="sub")]), set()),
            ("B-08", filters([rule("path", pattern="sub/"), rule("type", value="file"), rule("name", pattern="a?.txt")]), set()),
            ("B-09", filters([], [rule("type", value="symlink"), rule("type", value="fifo")]), ordinary | {"cache/a.txt"}),
        ]
        fixture = Fixture(root, agent, server)
        try:
            task = fixture.request("add_task", {
                "sources": [str(source)], "target_id": "local", "file_types": ["file", "symlink", "fifo"],
                "preserve_empty_dirs": True,
            })
            originals = []
            original_tree = tree(source)
            for label, rules, expected in cases:
                fixture.request("update_task_filters", {"id": task["id"], "filters": rules})
                scan = fixture.request("scan", {"task_id": task["id"]})
                assert scan["complete"], scan
                selected = {item["path"] for item in scan["sample"] if item["type"] != "directory"}
                assert selected == expected, (label, selected, expected)
                version = fixture.backup(task["id"])
                assert version["filters"] == rules and version["rules"]["filters"] == rules
                manifest_path = root / "repository/storage/versions" / version["id"] / "entries.jsonl"
                manifest = [json.loads(line) for line in manifest_path.read_text().splitlines()]
                assert {entry["path"] for entry in manifest if entry["type"] != "directory"} == expected
                destination = root / label
                fixture.restore(version["id"], destination)
                restored = tree(destination)
                assert {path for path, value in restored.items() if value != "directory"} == expected, (label, restored)
                for path in expected:
                    assert restored[path] == original_tree[path], (label, path)
                originals.append((version, manifest_path.read_bytes()))
                print(f"PASS: {label} 扫描、清单与还原一致：{sorted(expected)}", flush=True)

            # 入选空目录的结构父目录也应保留，但不能保存不入选的孤立目录。
            (source / "parent/keep-empty").mkdir(parents=True)
            (source / "reject-empty").mkdir()
            empty_rules = filters([rule("type", value="directory"),
                                   rule("name", pattern="keep-empty")])
            fixture.request("update_task_filters", {"id": task["id"], "filters": empty_rules})
            empty_version = fixture.backup(task["id"])
            fixture.restore(empty_version["id"], root / "empty-dirs")
            assert tree(root / "empty-dirs") == {"parent": "directory", "parent/keep-empty": "directory"}
            empty_directory = root / "repository/storage/versions" / empty_version["id"]
            empty_manifest = (empty_directory / "entries.jsonl").read_bytes()
            empty_summary = (empty_directory / "summary.json").read_bytes()
            damaged = [json.loads(line) for line in empty_manifest.splitlines()]
            damaged[-1]["path"] = "parent/not-matching"
            try:
                rewrite_manifest(empty_directory, damaged)
                fixture.restore(empty_version["id"], root / "bad-empty-dirs", success=False)
                assert not (root / "bad-empty-dirs").exists()
            finally:
                (empty_directory / "entries.jsonl").write_bytes(empty_manifest)
                (empty_directory / "summary.json").write_bytes(empty_summary)
            fixture.request("update_task_filters", {"id": task["id"], "filters": cases[-1][1]})
            print("PASS: 入选空目录及父目录可还原，孤立的不匹配目录被拒绝", flush=True)

            # 草稿预览不会保存；之后的普通扫描仍采用原规则。
            saved = fixture.request("config")
            draft = filters([rule("name", pattern="a.txt")])
            preview = fixture.request("start_filter_preview", {"task_id": task["id"], "filters": draft})
            preview_record = fixture.wait(preview["operation_id"])
            assert preview_record["action"] == "filter_preview"
            assert preview_record["filters"] == draft
            assert preview_record["result"]["files"] == 2
            assert fixture.request("config") == saved
            summaries = fixture.request("operations", {"group_scans": False})
            assert any(item.get("filters") == draft for item in summaries["operations"])

            invalid = [
                filters([rule("path", pattern="../escape")]), filters([rule("path", pattern="/absolute")]),
                filters([rule("size", min=-1)]), filters([rule("size", min=20, max=10)]),
                filters([rule("user", value=4294967296)]), filters([rule("mtime", min="NaN")]),
                filters([rule("name", pattern="x", extra="bad")]),
                {"include": [], "exclude": [], "unknown": []},
            ]
            config_path = root / "agent-state/config.json"
            before = config_path.read_bytes()
            fixture.request("import_tasks", {"tasks": [{
                "id": "legacy-filter-injection", "path": str(source),
                "createdAt": "2026-11-02", "filters": draft,
            }]}, ok=False)
            assert config_path.read_bytes() == before
            for bad in invalid:
                fixture.request("update_task_filters", {"id": task["id"], "filters": bad}, ok=False)
                fixture.request("start_filter_preview", {"task_id": task["id"], "filters": bad}, ok=False)
                fixture.request("add_task", {"path": str(source), "target_id": "local", "filters": bad}, ok=False)
                assert config_path.read_bytes() == before and fixture.request("config") == saved
            assert fixture.request("config")["format"] == 6
            backup_config = config_path.with_suffix(".saved")
            config_path.rename(backup_config)
            config_path.mkdir()
            try:
                fixture.request("update_task_filters", {"id": task["id"], "filters": draft}, ok=False)
                assert fixture.request("config") == saved
            finally:
                config_path.rmdir()
                backup_config.rename(config_path)
            print("PASS: 草稿隔离、非法规则及保存失败保留原配置", flush=True)

            # 显式选择的结构父目录也必须遵守排除规则。
            explicit = fixture.request("add_task", {
                "sources": [str(source / "sub/a.txt"), str(source / "cache/a.txt")],
                "target_id": "local", "file_types": ["file"], "preserve_empty_dirs": True,
                "filters": filters([], [rule("name", pattern="sub")]),
            })
            explicit_version = fixture.backup(explicit["id"])
            fixture.restore(explicit_version["id"], root / "explicit")
            assert set(tree(root / "explicit")) == {"cache", "cache/a.txt"}

            # 筛选先于硬链接代表路径选择，未选中的组成员不出现在版本中。
            (source / "hard-a").write_bytes(b"hard data")
            os.link(source / "hard-a", source / "hard-b")
            hard = filters([rule("type", value="hardlink"), rule("name", pattern="hard-b")])
            fixture.request("update_task_filters", {"id": task["id"], "filters": hard})
            hard_version = fixture.backup(task["id"])
            fixture.restore(hard_version["id"], root / "hard")
            assert set(tree(root / "hard")) == {"hard-b"}
            assert (root / "hard/hard-b").read_bytes() == b"hard data"

            # 启用压缩的配置不能因编辑筛选而降级，历史版本不随修改变化。
            fixture.request("save_target", {"id": "local", "name": "Compressed", "host": "127.0.0.1", "port": fixture.port, "compression": "lz77"})
            fixture.request("update_task_filters", {"id": task["id"], "filters": draft})
            compressed = fixture.backup(task["id"])
            assert compressed["compression"] == "lz77"
            fixture.restore(compressed["id"], root / "compressed")
            assert set(tree(root / "compressed")) == {"sub", "sub/a.txt", "cache", "cache/a.txt"}
            assert fixture.request("config")["format"] == 7
            stop(fixture.agent)
            fixture.agent = None
            fixture.start_agent()
            assert fixture.request("config")["format"] == 7
            assert next(item for item in fixture.request("config")["tasks"] if item["id"] == task["id"])["filters"] == draft
            for version, manifest in originals:
                path = root / "repository/storage/versions" / version["id"]
                assert (path / "entries.jsonl").read_bytes() == manifest
            fixture.restore(originals[0][0]["id"], root / "historical")
            print("PASS: 显式来源剪枝、硬链接、压缩组合、重启及历史快照", flush=True)

            # Server 独立拒绝违反规则的条目；历史规则被改坏时还原不能创建目录。
            token = json.loads((root / "repository/database/access.json").read_text())["token"]
            for rules, entry in [
                (filters([], [rule("name", pattern="blocked")]), {"path": "blocked", "type": "directory"}),
                (draft, {"path": "bad.txt", "type": "file", "size": "3"}),
            ]:
                with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as connection:
                    rpc(connection, "authenticate", {"token": token})
                    rpc(connection, "begin", {"source": str(source), "task_id": task["id"], "operation_id": str(uuid.uuid4()), "filters": rules})
                    entry.update({"uid": str(uid), "mtime_sec": str(m1)})
                    rpc(connection, "entry", entry, ok=False)
            with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as connection:
                rpc(connection, "authenticate", {"token": token})
                operation_id = str(uuid.uuid4())
                rpc(connection, "begin", {
                    "source": str(source), "task_id": task["id"], "operation_id": operation_id,
                    "filters": filters([rule("type", value="file")]),
                })
                rpc(connection, "entry", {
                    "path": "unexpected-empty", "type": "directory", "mode": 448,
                    "uid": str(uid), "gid": str(os.getgid()), "mtime_sec": str(m1), "mtime_nsec": "0",
                })
                failed_commit = rpc(connection, "commit", {"operation_id": operation_id}, ok=False)
                assert "Empty directory" in failed_commit["reason"]
            version = originals[0][0]
            summary_path = root / "repository/storage/versions" / version["id"] / "summary.json"
            original = summary_path.read_bytes()
            changed = json.loads(original)
            changed["filters"] = draft
            summary_path.write_text(json.dumps(changed))
            try:
                fixture.restore(version["id"], root / "tampered", success=False)
                assert not (root / "tampered").exists()
            finally:
                summary_path.write_bytes(original)
            print("PASS: Server 规则校验及损坏历史规则拒绝还原", flush=True)
        finally:
            fixture.close()


if __name__ == "__main__":
    run(*sys.argv[1:])
