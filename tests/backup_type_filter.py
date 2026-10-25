#!/usr/bin/env python3
"""File type filters are enforced before task creation and during upload."""

import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import uuid

from backup_integration import Fixture, rpc, stop, tree


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-type-filter-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent, server)
        unix_socket = socket.socket(socket.AF_UNIX)
        nested_socket = socket.socket(socket.AF_UNIX)
        checks = 0

        def passed(label):
            nonlocal checks
            checks += 1
            print(f"PASS {checks:02d}: {label}", flush=True)

        def add(paths, types, preserve=None):
            payload = {
                "sources": [str(path) for path in paths],
                "target_id": "local", "file_types": types,
            }
            if preserve is not None:
                payload["preserve_empty_dirs"] = preserve
            return fixture.request("add_task", payload)

        try:
            source = root / "source"
            (source / "nested/empty").mkdir(parents=True)
            (source / "other-empty").mkdir()
            (source / "top.txt").write_text("top")
            (source / "nested/inside.txt").write_text("nested")
            (source / "nested/skip.txt").write_text("not selected by type")
            (source / "link").symlink_to("top.txt")
            os.mkfifo(source / "pipe")
            unix_socket.bind(str(source / "socket"))
            nested_socket.bind(str(source / "nested/empty/socket"))

            default = fixture.request("add_task", {
                "path": str(source), "target_id": "local",
            })
            unfiltered = fixture.backup(default["id"])
            assert any(item.get("type") == "socket" for item in unfiltered["warnings"])
            assert "file_types" not in default and "file_types" not in unfiltered
            passed("existing unfiltered tasks retain supported types and socket warnings")

            files = add([source], ["file"])
            assert files["file_types"] == ["file"]
            assert files["id"] != default["id"]
            assert fixture.request("config")["format"] == 3
            scan = fixture.request("scan", {"task_id": files["id"]})
            assert scan["complete"] and scan["files"] == 3
            assert scan["directories"] == 1 and scan["symlinks"] == 0
            assert [item["type"] for item in scan["warnings"]] == ["socket", "socket"]
            version = fixture.backup(files["id"])
            assert version["file_types"] == ["file"]
            assert version["rules"]["file_types"] == ["file"]
            assert [item["type"] for item in version["warnings"]] == ["socket", "socket"]
            fixture.restore(version["id"], root / "files-output")
            assert set(tree(root / "files-output")) == {
                "top.txt", "nested", "nested/inside.txt", "nested/skip.txt",
            }
            passed("file-only folder traversal excludes links, FIFO, socket and empty directories")

            directories = add([source], ["directory"])
            assert directories["id"] != files["id"]
            directory_scan = fixture.request("scan", {"task_id": directories["id"]})
            assert directory_scan["complete"] and directory_scan["files"] == 0
            assert directory_scan["directories"] == 3
            assert [item["type"] for item in directory_scan["warnings"]] == ["socket", "socket"]
            directory_version = fixture.backup(directories["id"])
            fixture.restore(directory_version["id"], root / "directories-output")
            assert tree(root / "directories-output") == {
                "nested": "directory", "nested/empty": "directory",
                "other-empty": "directory",
            }
            passed("directory-only backup retains empty folders without file contents")

            special = add([source], ["fifo", "symlink"])
            assert special["file_types"] == ["symlink", "fifo"]
            special_version = fixture.backup(special["id"])
            fixture.restore(special_version["id"], root / "special-output")
            assert tree(root / "special-output") == {
                "link": ("symlink", b"top.txt"), "pipe": "fifo",
            }
            assert [item["type"] for item in special_version["warnings"]] == ["socket", "socket"]
            passed("selected symlinks and FIFO retain their types without their file targets")

            for paths in ([source / "top.txt"], [source, source / "top.txt"],
                          [source / "top.txt", source / "link"]):
                payload = {"sources": [str(path) for path in paths],
                           "target_id": "local", "file_types": ["symlink"]}
                error = fixture.request("preview_sources", payload, ok=False)
                assert "does not match" in error["message"]
                fixture.request("add_task", payload, ok=False)
            assert add([source / "link"], ["symlink"])["file_types"] == ["symlink"]
            for invalid in ([], ["unknown"], ["file", "file"], ["file", 7]):
                fixture.request("preview_sources", {
                    "sources": [str(source)], "target_id": "local",
                    "file_types": invalid,
                }, ok=False)
            assert fixture.request("config")["format"] == 3
            passed("every explicit file is checked before merging; invalid rules preserve config")

            preserved = add([source], ["file"], True)
            compact = add([source], ["file"], False)
            assert preserved["id"] != compact["id"] != files["id"]
            assert fixture.request("config")["format"] == 4
            add([source], ["fifo"])
            assert fixture.request("config")["format"] == 4
            assert preserved["preserve_empty_dirs"] is True
            assert compact["preserve_empty_dirs"] is False
            preserved_scan = fixture.request("scan", {"task_id": preserved["id"]})
            compact_scan = fixture.request("scan", {"task_id": compact["id"]})
            assert preserved_scan["complete"] and compact_scan["complete"]
            assert preserved_scan["directories"] == 3
            assert compact_scan["directories"] == 1
            assert preserved_scan["files"] == compact_scan["files"] == 3
            preserved_version = fixture.backup(preserved["id"])
            compact_version = fixture.backup(compact["id"])
            assert preserved_version["rules"] == {
                "file_types": ["file"], "preserve_empty_dirs": True,
            }
            assert compact_version["rules"] == {
                "file_types": ["file"], "preserve_empty_dirs": False,
            }
            fixture.restore(preserved_version["id"], root / "preserved-output")
            fixture.restore(compact_version["id"], root / "compact-output")
            assert "other-empty" in tree(root / "preserved-output")
            assert "nested/empty" in tree(root / "preserved-output")
            assert "other-empty" not in tree(root / "compact-output")
            assert "nested/empty" not in tree(root / "compact-output")
            assert any(item.get("type") == "socket" for item in compact_version["warnings"])
            passed("new tasks always traverse subdirectories; empty directory rule is independent")

            for invalid_types, preserve in ((["directory"], False),
                                            ([], True), (["file"], "yes")):
                fixture.request("preview_sources", {
                    "sources": [str(source)], "target_id": "local",
                    "file_types": invalid_types, "preserve_empty_dirs": preserve,
                }, ok=False)
            fixture.request("preview_sources", {
                "sources": [str(source)], "target_id": "local",
                "preserve_empty_dirs": True,
            }, ok=False)
            fixture.request("add_task", {
                "sources": [str(source / "top.txt")], "target_id": "local",
                "file_types": ["symlink"], "preserve_empty_dirs": False,
            }, ok=False)
            assert fixture.request("config")["format"] == 4
            passed("new rules reject directory-only, missing content types and invalid booleans")

            empty_a = root / "empty-a"
            empty_b = root / "empty-b"
            empty_a.mkdir()
            empty_b.mkdir()
            (empty_a / "file").touch()
            (empty_b / "file").touch()
            empty_task = add([empty_a, empty_b], ["fifo"])
            empty_version = fixture.backup(empty_task["id"])
            assert int(empty_version["entries"]) == 0
            fixture.restore(empty_version["id"], root / "empty-output")
            assert tree(root / "empty-output") == {}
            passed("filtered multi-folder selection may publish an empty, restorable version")

            empty_root = root / "future-files"
            (empty_root / "subfolder").mkdir(parents=True)
            future = add([empty_root], ["file"], True)
            future_scan = fixture.request("scan", {"task_id": future["id"]})
            assert future_scan["complete"] and future_scan["files"] == 0
            assert future_scan["directories"] == 1
            future_version = fixture.backup(future["id"])
            fixture.restore(future_version["id"], root / "future-output")
            assert tree(root / "future-output") == {"subfolder": "directory"}
            passed("folder task with no current matching files remains valid")

            token = json.loads((root / "repository/database/access.json").read_text())["token"]
            with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as connection:
                rpc(connection, "authenticate", {"token": token})
                rpc(connection, "begin", {
                    "source": str(source), "task_id": "filter-check",
                    "operation_id": "excluded-selection",
                    "selection": [{"path": "top.txt", "type": "file"}],
                    "file_types": ["symlink"],
                }, ok=False)
                receipt = rpc(connection, "begin", {
                    "source": str(source), "task_id": "filter-check",
                    "operation_id": "excluded-file", "file_types": ["directory"],
                })
                assert receipt["file_types"] == ["directory"]
                rpc(connection, "entry", {
                    "path": "top.txt", "type": "file", "size": "3",
                }, ok=False)
            with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as connection:
                rpc(connection, "authenticate", {"token": token})
                rpc(connection, "begin", {
                    "source": str(source), "task_id": "filter-check",
                    "operation_id": "extra-empty-directory",
                    "file_types": ["file"], "preserve_empty_dirs": False,
                })
                rpc(connection, "entry", {"path": "other-empty", "type": "directory"})
                failed_commit = rpc(connection, "commit", {
                    "operation_id": "extra-empty-directory",
                }, ok=False)
                assert "Empty directory" in failed_commit["reason"]
            summary_path = root / "repository/storage/versions" / version["id"] / "summary.json"
            original = summary_path.read_bytes()
            tampered = json.loads(original)
            tampered["file_types"] = ["symlink"]
            summary_path.write_text(json.dumps(tampered))
            failed = fixture.restore(version["id"], root / "tampered-output", False)
            assert "selection" in failed["error"]
            assert not (root / "tampered-output").exists()
            summary_path.write_bytes(original)
            passed("Server rejects excluded types and restore rejects mismatched historical rules")

            new_summary_path = root / "repository/storage/versions" / preserved_version["id"] / "summary.json"
            original_new = new_summary_path.read_bytes()
            altered_new = json.loads(original_new)
            altered_new["preserve_empty_dirs"] = False
            new_summary_path.write_text(json.dumps(altered_new))
            fixture.restore(preserved_version["id"], root / "tampered-rule-output", False)
            assert not (root / "tampered-rule-output").exists()
            new_summary_path.write_bytes(original_new)
            passed("saved empty-directory rule is checked before restoration")

            for action in ("preview_sources", "add_task"):
                for paths, types in (([source], ["socket"]),
                                     ([source / "socket"], ["file"])):
                    error = fixture.request(action, {
                        "sources": [str(path) for path in paths],
                        "target_id": "local", "file_types": types,
                        "preserve_empty_dirs": False,
                    }, ok=False)
                    assert "Socket node" in error["message"]
            assert fixture.request("config")["format"] == 4
            passed("new tasks reject socket filters and directly selected socket nodes")

            stop(fixture.agent)
            fixture.start_agent()
            fixture.restore(directory_version["id"], root / "legacy-directory-after-restart")
            assert tree(root / "legacy-directory-after-restart") == {
                "nested": "directory", "nested/empty": "directory",
                "other-empty": "directory",
            }
            assert next(item for item in fixture.request("config")["tasks"]
                        if item["id"] == files["id"])["file_types"] == ["file"]
            assert next(item for item in fixture.request("config")["tasks"]
                        if item["id"] == compact["id"])["preserve_empty_dirs"] is False
            record = fixture.request("operations")["operations"]
            assert any(item.get("file_types") == ["file"] for item in record)
            fixture.request("remove_task", {"id": files["id"]})
            stop(fixture.server)
            fixture.start_server()
            fixture.restore(version["id"], root / "history-output")
            assert (root / "history-output/top.txt").read_text() == "top"
            history = fixture.request("versions", {"target_id": "local", "task_id": files["id"]})
            assert history["versions"][0]["file_types"] == ["file"]
            passed("saved filters survive Agent/Server restart and task deletion")

            stop(fixture.agent)
            config_path = root / "agent-state/config.json"
            config = json.loads(config_path.read_text())
            old_socket_task = dict(compact, id=str(uuid.uuid4()),
                                   file_types=["socket"])
            config["tasks"].append(old_socket_task)
            config["format"] = 5
            config_path.write_text(json.dumps(config))
            fixture.start_agent()
            assert any(item["id"] == old_socket_task["id"]
                       for item in fixture.request("config")["tasks"])
            old_scan = fixture.request("scan", {"task_id": old_socket_task["id"]})
            assert not old_scan["complete"]
            assert "Socket node backups" in old_scan["warnings"][0]["reason"]
            operation = fixture.request("start_backup", {
                "task_id": old_socket_task["id"],
            })["operation_id"]
            failed = fixture.wait(operation, False)
            assert "Socket node backups" in failed["error"]
            assert json.loads(config_path.read_text())["tasks"][-1]["id"] == old_socket_task["id"]
            passed("existing socket tasks remain readable but cannot produce new versions")
        finally:
            unix_socket.close()
            nested_socket.close()
            fixture.close()
        print(f"PASS: {checks} file type filter integration scenarios", flush=True)


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
