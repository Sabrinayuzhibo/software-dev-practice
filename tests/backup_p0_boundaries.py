#!/usr/bin/env python3
"""P0 health, scan, large-version, configuration and partial-restore boundaries."""

import hashlib
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading

from backup_faults import Proxy
from backup_integration import Fixture, stop, tree
from file_type_fixtures import create_socket


def pages(fixture, action, args, field):
    items = []
    offset = 0
    while offset is not None:
        page = fixture.request(action, {**args, "offset": offset})
        assert len(json.dumps(page, ensure_ascii=False).encode()) < 70 * 1024
        assert len(page[field]) <= 100
        items.extend(page[field])
        next_offset = page["next_offset"]
        assert next_offset is None or next_offset > offset
        offset = next_offset
    assert len(items) == page["total"]
    return items, page


def check_health_and_scan(fixture, root):
    assert fixture.request("ping", {"target_id": "local"})["storage_state"] == "available"
    staging = root / "repository/storage/staging"
    try:
        staging.chmod(0o500)
        health = fixture.request("ping", {"target_id": "local"})
        assert health["storage_state"] == "unavailable" and "staging" in health["storage_error"]
    finally:
        staging.chmod(0o700)
    assert fixture.request("ping", {"target_id": "local"})["storage_state"] == "available"
    versions = root / "repository/storage/versions"
    try:
        versions.chmod(0o300)
        fixture.request("versions", {"target_id": "local"}, ok=False)
    finally:
        versions.chmod(0o700)
    assert fixture.request("versions", {"target_id": "local"})["total"] == 0
    print("PASS: writable storage health, recovery, unreadable catalog is an error", flush=True)

    source = root / "scan"
    source.mkdir()
    (source / "readable").write_bytes(b"readable")
    (source / "empty-dir").mkdir()
    (source / "link").symlink_to("readable")
    blocked = [source / "denied-a", source / "denied-b", source / "denied-dir"]
    blocked[0].write_text("a")
    blocked[1].write_text("b")
    blocked[2].mkdir()
    task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
    try:
        for path in blocked:
            path.chmod(0)
        started = fixture.request("start_scan", {"task_id": task["id"]})
        scan = fixture.wait(started["operation_id"], False)
        assert not scan["complete"] and scan["error_count"] == 3, scan
        assert scan["files"] == 1 and scan["bytes"] == 8
        preview = {item["path"]: item for item in scan["result"]["sample"]}
        assert preview["empty-dir"]["type"] == "directory"
        assert preview["link"]["type"] == "symlink"
        assert preview["readable"]["sha256"] == hashlib.sha256(b"readable").hexdigest()
        warnings, _ = pages(fixture, "operation_warnings", {"id": scan["id"]}, "warnings")
        assert {item["path"] for item in warnings if item.get("severity") == "error"} == {
            path.name for path in blocked}
        assert fixture.request("versions", {"target_id": "local"})["total"] == 0
    finally:
        for path in blocked:
            path.chmod(0o700)
    source.rename(root / "scan-moved")
    missing = fixture.request("start_scan", {"task_id": task["id"]})
    result = fixture.wait(missing["operation_id"], False)
    assert not result["result"]["complete"] and result["error_count"] == 1
    print("PASS: multiple unreadable paths aggregated; types, hashes, invalid root and incomplete preview", flush=True)


def check_large_versions(fixture, root):
    source = root / "repeated"
    source.mkdir()
    (source / "data").write_text("original")
    nested = source
    for index in range(5):
        nested /= str(index) + "x" * 180
        nested.mkdir()
    for index in range(1899):
        create_socket(nested / f"link-{index:04}")
    task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
    ids = []
    refs = set()
    for _ in range(10):
        operation = fixture.request("start_backup", {"task_id": task["id"]})["operation_id"]
        result = fixture.wait(operation)
        assert result["warning_count"] == 1899
        assert result["result"]["version"]["warning_count"] == 1899
        stored = json.loads((root / "agent-state/operations" / f"{operation}.json").read_text())
        refs.add(stored["warning_ref"])
        ids.append(operation)
    assert len(refs) == 1
    directory = root / "repository/storage/versions"
    warning_file = directory / ids[0] / "warnings.jsonl"
    assert warning_file.stat().st_size > 1024 * 1024
    warnings, _ = pages(fixture, "version_warnings", {
        "target_id": "local", "version_id": ids[0]}, "warnings")
    assert len({item["path"] for item in warnings}) == 1899
    legacy_path = directory / ids[0] / "summary.json"
    legacy = json.loads(legacy_path.read_text())
    # Original versions could only store up to 1 MiB of JSON per summary.
    legacy_warnings = warnings[:400]
    legacy["warnings"] = legacy_warnings
    del legacy["warning_count"], legacy["warnings_sha256"]
    legacy_path.write_text(json.dumps(legacy))
    listed, _ = pages(fixture, "versions", {"target_id": "local"}, "versions")
    assert len(listed) == 10 and all("warnings" not in item for item in listed)
    assert all(item["warning_count"] == (400 if item["id"] == ids[0] else 1899)
               and item["rules"] == {} for item in listed)
    assert pages(fixture, "version_warnings", {
        "target_id": "local", "version_id": ids[0]}, "warnings")[0] == legacy_warnings
    fixture.restore(ids[0], root / "legacy-restore")
    assert tree(root / "legacy-restore") == tree(source)
    stop(fixture.server)
    fixture.start_server()
    stop(fixture.agent)
    fixture.start_agent()
    assert fixture.request("versions", {"target_id": "local"})["total"] == 10
    (source / "data").write_text("changed")
    changed = fixture.backup(task["id"])
    fixture.restore(changed["id"], root / "new-restore")
    assert (root / "legacy-restore/data").read_text() == "original"
    assert (root / "new-restore/data").read_text() == "changed"
    corrupt = directory / ids[1] / "warnings.jsonl"
    content = corrupt.read_bytes()
    try:
        corrupt.write_bytes(content[:-1])
        fixture.request("version_warnings", {"target_id": "local", "version_id": ids[1]}, ok=False)
        assert fixture.request("versions", {"target_id": "local"})["total"] == 11
    finally:
        corrupt.write_bytes(content)
    print("PASS: ten backups with 1899 warnings; bounded pages, legacy versions, restart and changed content", flush=True)


def check_snapshot_and_restore(fixture, root):
    source = root / "journal-source"
    source.mkdir()
    for index in range(125):
        (source / f"file-{index:03}").write_bytes(b"x" * 32)
    task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
    blocked, release = threading.Event(), threading.Event()

    def pause_upload(request, _reply):
        if request["type"] == "chunk" and not blocked.is_set():
            blocked.set()
            assert release.wait(10)
        return False

    proxy = Proxy(fixture.port, pause_upload)
    target = {"id": "local", "name": "Snapshot", "host": "127.0.0.1", "port": proxy.listen_port}
    try:
        fixture.request("save_target", target)
        operation = fixture.request("start_backup", {"task_id": task["id"]})["operation_id"]
        assert blocked.wait(5)
        # Keep the unused port reserved so no unrelated service can claim it.
        with socket.socket() as unavailable:
            unavailable.bind(("127.0.0.1", 0))
            fixture.request("save_target", {**target, "port": unavailable.getsockname()[1]})
            release.set()
            record = fixture.wait(operation)
            assert record["target"]["port"] == proxy.listen_port
            failed = fixture.request("start_backup", {"task_id": task["id"]})["operation_id"]
            assert "connect" in fixture.wait(failed, False)["error"].lower()
    finally:
        release.set()
        proxy.close()
    target["port"] = fixture.port
    fixture.request("save_target", {**target, "repository_path": str(source)}, ok=False)
    fixture.request("save_target", {**target, "repository_path": str(root / "repository")})
    fixture.request("add_task", {"target_id": "local", "path": str(root / "repository")}, ok=False)
    fixture.request("save_target", {**target, "id": "alias"})
    fixture.request("add_task", {"target_id": "alias", "path": str(source)}, ok=False)
    with socket.socket() as unused:
        unused.bind(("127.0.0.1", 0))
        fixture.request("save_target", {**target, "id": "alias", "port": unused.getsockname()[1]})
        other_task = fixture.request("add_task", {"target_id": "alias", "path": str(source)})
        fixture.request("save_target", {**target, "id": "alias"}, ok=False)
        fixture.request("remove_task", {"id": other_task["id"]})
    fixture.request("save_target", {**target, "repository_path": str(root / "different-repository")})
    assert "match" in fixture.request("ping", {"target_id": "local"}, ok=False)["message"]
    fixture.request("save_target", target)
    print("PASS: saved configuration affects the next run; active target snapshot and repository checks", flush=True)

    version_id = operation
    other_repository = root / "other-repository"
    other_repository.mkdir()
    fixture.request("save_target", {**target, "id": "other",
                                    "repository_path": str(other_repository)})
    fixture.request("add_task", {"target_id": "local", "path": str(other_repository)}, ok=False)
    denied = fixture.restore(version_id, other_repository, False)
    assert "configured repository" in denied["error"] and not list(other_repository.iterdir())
    manifest = [json.loads(line) for line in (
        root / "repository/storage/versions" / version_id / "entries.jsonl").read_text().splitlines()]
    bad = root / "repository/storage/versions" / version_id / (manifest[110]["index"] + ".data")
    content = bad.read_bytes()
    try:
        bad.write_bytes(b"broken")
        partial = fixture.restore(version_id, root / "partial-restore", False)
        assert partial["restore_journal"]
        entries, page = pages(fixture, "restore_entries", {"id": partial["id"]}, "entries")
        assert page["written"] == 110 and page["uncertain"] == 1
        assert {item["path"] for item in entries if item["state"] == "written"} == set(tree(root / "partial-restore"))
        assert entries[-1]["path"] == manifest[110]["path"]
    finally:
        bad.write_bytes(content)
    complete = fixture.restore(version_id, root / "complete-restore")
    _, page = pages(fixture, "restore_entries", {"id": complete["id"]}, "entries")
    assert page["written"] == 125 and page["uncertain"] == 0
    assert tree(root / "complete-restore") == tree(source)

    killed = threading.Event()

    def kill_restore(request, _reply):
        if request["type"] == "download" and int(request["payload"]["index"]) == 2:
            fixture.agent.kill()
            fixture.agent.wait(timeout=5)
            killed.set()
            return True
        return False

    proxy = Proxy(fixture.port, kill_restore)
    try:
        fixture.request("save_target", {**target, "port": proxy.listen_port})
        operation = fixture.request("start_restore", {"target_id": "local", "version_id": version_id,
                                                      "destination": str(root / "crash-restore")})["operation_id"]
        assert killed.wait(5)
        stop(fixture.agent)
        # Model a torn last journal event. The preceding durable intent survives.
        journal = root / "agent-state/restores" / f"{operation}.jsonl"
        with journal.open("ab") as output:
            output.write(b'{"path":')
        fixture.start_agent()
        record = fixture.request("operation", {"id": operation})
        assert record["state"] == "INTERRUPTED" and record["restore_journal"]
        entries, page = pages(fixture, "restore_entries", {"id": operation}, "entries")
        assert page["written"] == 2 and page["uncertain"] == 1 and page["incomplete"]
        assert (root / "crash-restore" / entries[-1]["temporary_path"]).exists()
    finally:
        proxy.close()
    fixture.request("save_target", target)
    print("PASS: paged restore journal matches partial writes; crash, temporary file and torn-event recovery", flush=True)


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-p0-boundaries-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent, server)
        try:
            check_health_and_scan(fixture, root)
            check_large_versions(fixture, root)
            check_snapshot_and_restore(fixture, root)
        finally:
            fixture.close()


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
