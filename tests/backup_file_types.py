#!/usr/bin/env python3
"""FR-10 round trips, format compatibility and hostile link boundaries."""

import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import sys
import tempfile

from backup_faults import Proxy
from backup_integration import Fixture, rpc, stop, tree
from file_type_fixtures import create_socket


def manifest_path(fixture, version):
    return fixture.root / "repository/storage/versions" / version["id"]


def rewrite_manifest(directory, entries, version_format=3):
    """Keep checksum valid so tests exercise semantic validation, not SHA alone."""
    entries = [dict(item, index=str(index)) for index, item in enumerate(entries)]
    data = b"".join(json.dumps(item, sort_keys=True, separators=(",", ":"),
                              ensure_ascii=False).encode() + b"\n" for item in entries)
    (directory / "entries.jsonl").write_bytes(data)
    summary = json.loads((directory / "summary.json").read_text())
    summary.update({
        "format": version_format, "entries": str(len(entries)),
        "files": str(sum(item["type"] in ("file", "hardlink") for item in entries)),
        "directories": str(sum(item["type"] == "directory" for item in entries)),
        "bytes": str(sum(int(item.get("size", 0)) for item in entries
                         if item["type"] in ("file", "hardlink"))),
        "manifest_sha256": hashlib.sha256(data).hexdigest(),
    })
    for field, kind in [("symlinks", "symlink"), ("hardlinks", "hardlink"),
                        ("fifos", "fifo"), ("character_devices", "character_device"),
                        ("block_devices", "block_device"), ("sockets", "socket")]:
        summary[field] = str(sum(item["type"] == kind for item in entries))
    summary["stored_bytes"] = str(sum(int(item["size"]) for item in entries if item["type"] == "file"))
    if version_format < 3:
        for field in ("character_devices", "block_devices", "sockets"):
            summary.pop(field)
    if version_format == 1:
        for field in ("symlinks", "hardlinks", "fifos", "stored_bytes"):
            summary.pop(field)
    (directory / "summary.json").write_text(json.dumps(summary))
    return entries


def restore_pages(fixture, operation):
    entries = []
    offset = 0
    while offset is not None:
        page = fixture.request("restore_entries", {"id": operation, "offset": offset})
        entries.extend(page["entries"])
        offset = page["next_offset"]
    return entries


def check_round_trip(fixture):
    root = fixture.root
    source = root / "types"
    (source / "sub").mkdir(parents=True)
    (source / "empty directory").mkdir()
    payload = b"shared\x00content\n" * 8192
    (source / "original").write_bytes(payload)
    os.link(source / "original", source / "sub/hard one")
    os.link(source / "original", source / "third")
    os.link(source / "original", root / "outside-member")
    (source / "empty").touch()
    os.link(source / "empty", source / "sub/empty-alias")
    (source / "same-content").write_bytes(payload)
    (source / "single-in-scope").write_bytes(b"external group")
    os.link(source / "single-in-scope", root / "other-external-member")
    (source / ".hidden").write_bytes(b"hidden")
    (root / "outside").write_text("outside sentinel")
    targets = {
        "relative": b"sub/hard one", "absolute": os.fsencode(root / "outside"),
        "dangling": b"not-here", "directory-link": b"sub", "external": b"../outside",
        "self": b"self", "cycle-a": b"cycle-b", "cycle-b": b"cycle-a",
        "raw-target": b"../non-utf8-\xff", "long-target": b"x" * 3000,
    }
    for path, target in targets.items():
        os.symlink(target, os.fsencode(source / path))
    os.mkfifo(source / "pipe")
    os.mkfifo(source / "sub/.hidden-pipe")
    create_socket(source / "socket")
    expected = tree(source)
    task = fixture.request("add_task", {"target_id": "local", "path": str(source)})
    scan = fixture.request("scan", {"task_id": task["id"]})
    assert scan["complete"] and scan["files"] == 8 and scan["directories"] == 2, scan
    assert scan["hardlinks"] == 3 and scan["symlinks"] == 10 and scan["fifos"] == 2
    assert scan["bytes"] == 4 * len(payload) + 20
    assert scan["stored_bytes"] == 2 * len(payload) + 20
    sample = {item["path"]: item for item in scan["sample"]}
    assert sample["relative"]["link_target"] == "sub/hard one"
    assert sample["pipe"]["type"] == "fifo" and sample["socket"]["type"] == "socket"
    assert len([item for item in sample.values() if item["type"] == "hardlink"]) == 3
    assert fixture.request("versions", {"target_id": "local"})["total"] == 0

    version = fixture.backup(task["id"])
    assert version["format"] == 3 and len(version["warnings"]) == 1
    assert version["warnings"][0]["type"] == "socket"
    directory = manifest_path(fixture, version)
    entries = [json.loads(line) for line in (directory / "entries.jsonl").read_text().splitlines()]
    assert len(list(directory.glob("*.data"))) == 5
    assert sum(item.stat().st_size for item in directory.glob("*.data")) == scan["stored_bytes"]
    assert all(int(version[key]) == scan[key]
               for key in ("files", "directories", "bytes", "stored_bytes", "symlinks", "hardlinks", "fifos"))
    assert all(item["path"] != "socket" for item in entries)
    saved_links = {item["path"]: base64.b64decode(item["target_base64"])
                   for item in entries if item["type"] == "symlink"}
    assert saved_links == targets
    # Change source and restart both processes: restore must use only the version.
    (source / "original").write_bytes(b"changed after backup")
    (source / "relative").unlink()
    (source / "relative").symlink_to("different")
    stop(fixture.server)
    fixture.start_server()
    stop(fixture.agent)
    fixture.start_agent()
    fixture.request("remove_task", {"id": task["id"]})
    destination = root / "types-restored"
    restored = fixture.restore(version["id"], destination)
    assert restored["state"] == "SUCCEEDED_WITH_WARNINGS"
    assert tree(destination) == expected and not (destination / "socket").exists()
    assert all(os.readlink(os.fsencode(destination / name)) == target for name, target in targets.items())
    assert stat.S_ISFIFO((destination / "pipe").lstat().st_mode)
    identity = lambda path: (path.stat().st_dev, path.stat().st_ino)
    assert len({identity(destination / name) for name in ("original", "sub/hard one", "third")}) == 1
    assert identity(destination / "empty") == identity(destination / "sub/empty-alias")
    assert identity(destination / "same-content") != identity(destination / "original")
    assert identity(destination / "original") != identity(root / "outside-member")
    assert identity(destination / "single-in-scope") != identity(root / "other-external-member")
    assert (root / "outside").read_text() == "outside sentinel"
    written = restore_pages(fixture, restored["id"])
    assert len(written) == 22 and all(item["state"] == "written" for item in written)
    assert {item["type"] for item in written} == {"directory", "file", "hardlink", "symlink", "fifo"}
    stop(fixture.agent)
    fixture.start_agent()
    assert restore_pages(fixture, restored["id"]) == written
    print("PASS: exact link bytes, inode groups, one stored payload per group, FIFO, socket, restart and journals", flush=True)


def check_manifests(fixture):
    root = fixture.root
    source = root / "manifest-source"
    source.mkdir()
    (source / "file").write_bytes(b"payload")
    task = fixture.request("add_task", {"target_id": "local", "path": str(source)})
    version = fixture.backup(task["id"])
    directory = manifest_path(fixture, version)
    ordinary = json.loads((directory / "entries.jsonl").read_text())
    # A real format-1 summary has no new counters and may have inline warnings.
    rewrite_manifest(directory, [ordinary], 1)
    summary_path = directory / "summary.json"
    summary = json.loads(summary_path.read_text())
    summary["warnings"] = []
    del summary["warning_count"], summary["warnings_sha256"]
    summary_path.write_text(json.dumps(summary))
    fixture.restore(version["id"], root / "legacy-restored")
    assert tree(root / "legacy-restored") == tree(source)
    anchor = dict(ordinary, link_group="file")
    alias = {"path": "alias", "type": "hardlink", "link_to": "file",
             "size": ordinary["size"], "sha256": ordinary["sha256"]}
    symlink = {"path": "link", "type": "symlink", "target_base64": base64.b64encode(b"../outside").decode()}
    cases = [
        [symlink, dict(ordinary, path="link/escape")],
        [dict(symlink, path="/absolute")],
        [dict(symlink, path="../escape")],
        [dict(symlink, target_base64="AA==")],
        [dict(symlink, target_base64="")],
        [dict(symlink, target_base64="invalid base64")],
        [dict(symlink, target_base64=base64.b64encode(b"x" * 4097).decode())],
        [anchor, dict(alias, link_to="../outside")],
        [anchor, dict(alias, link_to="/etc/passwd")],
        [anchor, dict(alias, link_to="missing")],
        [alias, anchor],
        [dict(alias, path="a", link_to="b"), dict(alias, path="b", link_to="a")],
        [anchor, alias, dict(alias, path="chain", link_to="alias")],
        [ordinary, alias],
        [anchor, dict(alias, size="8")],
        [anchor, dict(alias, sha256="0" * 64)],
        [anchor, dict(alias, link_to=version["id"] + "/file")],
        [dict(anchor, link_group="other")],
        [symlink, dict(alias, link_to="link")],
        [{"path": "pipe", "type": "fifo"}, dict(alias, link_to="pipe")],
        [{"path": "dir", "type": "directory"}, dict(alias, link_to="dir")],
        [anchor, dict(alias, path="file")],
        [{"path": "node", "type": "character_device"}],
        [{"path": "node", "type": "block_device", "device_major": "1"}],
        [{"path": "node", "type": "socket", "device_major": "1"}],
        [{"path": "node", "type": "socket", "size": "0"}],
        [{"path": "node", "type": "character_device", "device_major": "-1", "device_minor": "1"}],
        [{"path": "node", "type": "block_device", "device_major": "4294967296", "device_minor": "0"}],
        [dict(ordinary, device_major="1", device_minor="3")],
    ]
    for index, entries in enumerate(cases):
        rewrite_manifest(directory, entries)
        destination = root / f"invalid-{index}"
        result = fixture.restore(version["id"], destination, False)
        assert result["state"] == "FAILED" and not destination.exists(), result
    rewrite_manifest(directory, [symlink], 1)
    fixture.restore(version["id"], root / "format1-special", False)
    assert not (root / "format1-special").exists()
    rewrite_manifest(directory, [ordinary], 2)
    fixture.restore(version["id"], root / "format2-restored")
    assert tree(root / "format2-restored") == tree(source)
    rewrite_manifest(directory, [{"path": "node", "type": "socket"}], 2)
    fixture.restore(version["id"], root / "format2-node", False)
    assert not (root / "format2-node").exists()
    rewrite_manifest(directory, [anchor, alias, {"path": "pipe", "type": "fifo"}, symlink])
    valid = root / "validated"
    fixture.restore(version["id"], valid)
    assert (valid / "file").stat().st_ino == (valid / "alias").stat().st_ino
    assert (root / "outside").read_text() == "outside sentinel"

    token = json.loads((root / "repository/database/access.json").read_text())["token"]
    for index, bad in enumerate([alias, dict(symlink, target_base64="AA=="),
                                 {"path": "device", "type": "socket", "device_major": "1"}]):
        with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as connection:
            rpc(connection, "authenticate", {"token": token})
            rpc(connection, "begin", {"operation_id": f"invalid-link-{index}",
                                      "source": str(source), "task_id": task["id"]})
            rpc(connection, "entry", bad, ok=False)
    print("PASS: formats 1 and 2 remain readable; malformed special nodes fail before writes; Server rejects invalid entries", flush=True)


def check_link_races(fixture):
    root = fixture.root
    source = root / "races"
    source.mkdir()
    for name in ("a", "b", "c"):
        if name == "a":
            (source / name).write_bytes(b"original" * 100000)
        else:
            os.link(source / "a", source / name)
    (source / "barrier").write_bytes(b"barrier")
    task = fixture.request("add_task", {"target_id": "local", "path": str(source)})
    version = fixture.backup(task["id"])
    original_count = fixture.request("versions", {"target_id": "local"})["total"]
    changed = False

    def change_group(request, reply):
        nonlocal changed
        if request["type"] == "entry" and request["payload"].get("type") == "hardlink" and not changed:
            changed = True
            (source / "a").write_bytes(b"changed after alias was accepted")
        return False

    proxy = Proxy(fixture.port, change_group)
    try:
        fixture.request("save_target", {"id": "local", "name": "Race proxy", "host": "127.0.0.1", "port": proxy.listen_port})
        operation = fixture.request("start_backup", {"task_id": task["id"]})["operation_id"]
        failed = fixture.wait(operation, False)
        assert changed and "changed" in failed["error"].lower(), failed
        assert fixture.request("versions", {"target_id": "local"})["total"] == original_count
    finally:
        fixture.configure_target()
        proxy.close()

    directory = manifest_path(fixture, version)
    original_entries = [json.loads(line) for line in (directory / "entries.jsonl").read_text().splitlines()]
    anchor = next(item for item in original_entries if item["type"] == "file" and "link_group" in item)
    alias = next(item for item in original_entries if item["type"] == "hardlink")
    barrier = next(item for item in original_entries if item["path"] == "barrier")
    payloads = [(directory / f'{item["index"]}.data').read_bytes() for item in (anchor, barrier)]
    rewrite_manifest(directory, [anchor, barrier, alias])
    for index, data in enumerate(payloads):
        (directory / f"{index}.data").write_bytes(data)
    for mutation in ("replace", "modify"):
        destination = root / ("race-restore-" + mutation)
        attacked = False

        def replace_anchor(request, reply):
            nonlocal attacked
            if request["type"] == "download" and int(request["payload"]["index"]) == 1 and not attacked:
                attacked = True
                target = destination / anchor["path"]
                if mutation == "modify":
                    target.write_bytes(b"bad content")
                else:
                    target.unlink()
                    target.symlink_to(root / "outside")
            return False

        # Both mutations occur after verified publication, before alias creation.
        proxy = Proxy(fixture.port, replace_anchor)
        try:
            fixture.request("save_target", {"id": "local", "name": "Race proxy", "host": "127.0.0.1", "port": proxy.listen_port})
            failed = fixture.restore(version["id"], destination, False)
            assert attacked and "hard link" in failed["error"].lower(), failed
            assert not os.path.lexists(destination / alias["path"])
            assert (root / "outside").read_text() == "outside sentinel"
            journal = restore_pages(fixture, failed["id"])
            assert any(item["type"] == "hardlink" and item["state"] == "pending" for item in journal)
        finally:
            fixture.configure_target()
            proxy.close()
    print("PASS: changing an uploaded hard-link group fails; restored anchor substitution/content changes fail without outside writes", flush=True)


def check_restore_failures(fixture, shim):
    root = fixture.root
    source = root / "creation-failures"
    source.mkdir()
    (source / "file").write_bytes(b"unchanged version")
    task = fixture.request("add_task", {
        "target_id": "local", "sources": [str(source)],
        "file_types": ["file", "symlink", "fifo", "socket"],
        "preserve_empty_dirs": False,
    })
    version = fixture.backup(task["id"])
    directory = manifest_path(fixture, version)
    anchor = dict(json.loads((directory / "entries.jsonl").read_text()), link_group="file")
    special = [
        {"path": "blocked-alias", "type": "hardlink", "link_to": "file",
         "size": anchor["size"], "sha256": anchor["sha256"]},
        {"path": "blocked-link", "type": "symlink", "target_base64": "ZmlsZQ=="},
        {"path": "blocked-pipe", "type": "fifo"},
        {"path": "blocked-socket", "type": "socket"},
    ]
    for entry in special:
        rewrite_manifest(directory, [anchor, entry])
        stop(fixture.agent)
        environment = {"LD_PRELOAD": shim, "BACKUP_TEST_FAIL_TYPE": entry["type"],
                       "BACKUP_TEST_FAIL_LEAF": entry["path"]}
        previous = {key: os.environ.get(key) for key in environment}
        try:
            os.environ.update(environment)
            fixture.start_agent()
        finally:
            for key, value in previous.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        destination = root / ("unsupported-" + entry["type"])
        failed = fixture.restore(version["id"], destination, False)
        assert entry["path"] in failed["error"] and "not supported" in failed["error"].lower(), failed
        assert not os.path.lexists(destination / entry["path"])
        assert (destination / "file").read_bytes() == b"unchanged version"
        assert restore_pages(fixture, failed["id"])[-1] == {
            "path": entry["path"], "type": entry["type"], "state": "pending"}
        stop(fixture.agent)
        fixture.start_agent()
        fixture.restore(version["id"], root / ("supported-" + entry["type"]))
    print("PASS: injected EOPNOTSUPP for hardlink/symlink/FIFO/socket yields explicit failures, partial journals and no copy fallback", flush=True)


def check_parent_replacement(fixture):
    root = fixture.root
    source = root / "parent-race"
    source.mkdir()
    (source / "anchor").write_bytes(b"verified content")
    (source / "barrier").write_bytes(b"pause here")
    task = fixture.request("add_task", {
        "target_id": "local", "sources": [str(source)],
        "file_types": ["file", "symlink", "fifo", "socket"],
        "preserve_empty_dirs": False,
    })
    version = fixture.backup(task["id"])
    directory = manifest_path(fixture, version)
    entries = {item["path"]: item for item in
               (json.loads(line) for line in (directory / "entries.jsonl").read_text().splitlines())}
    anchor = dict(entries["anchor"], link_group="anchor")
    barrier = entries["barrier"]
    data = [(directory / f'{item["index"]}.data').read_bytes() for item in (anchor, barrier)]
    external = root / "outside-directory"
    external.mkdir()
    (external / "sentinel").write_text("safe")
    for kind in ("file", "symlink", "fifo", "hardlink", "socket"):
        leaf = {"path": "sub/leaf", "type": kind}
        if kind == "symlink":
            leaf["target_base64"] = "YW5jaG9y"
        elif kind in ("file", "hardlink"):
            leaf.update(size=anchor["size"], sha256=anchor["sha256"])
            if kind == "hardlink":
                leaf["link_to"] = "anchor"
        rewrite_manifest(directory, [anchor, {"path": "sub", "type": "directory"}, barrier, leaf])
        (directory / "0.data").write_bytes(data[0])
        (directory / "2.data").write_bytes(data[1])
        (directory / "3.data").write_bytes(data[0])
        destination = root / ("parent-replaced-" + kind)
        attacked = False

        def replace_parent(request, reply):
            nonlocal attacked
            if request["type"] == "download" and int(request["payload"]["index"]) == 2 and not attacked:
                attacked = True
                (destination / "sub").rmdir()
                (destination / "sub").symlink_to(external)
            return False

        proxy = Proxy(fixture.port, replace_parent)
        try:
            fixture.request("save_target", {"id": "local", "name": "Parent proxy", "host": "127.0.0.1", "port": proxy.listen_port})
            failed = fixture.restore(version["id"], destination, False)
            assert attacked and "sub/leaf" in failed["error"], failed
            assert list(external.iterdir()) == [external / "sentinel"]
            assert (external / "sentinel").read_text() == "safe"
        finally:
            fixture.configure_target()
            proxy.close()
    print("PASS: replacing a restore parent with an external symlink blocks all five leaf types without outside writes", flush=True)


def run(agent, server, shim):
    with tempfile.TemporaryDirectory(prefix="backup-types-") as temporary:
        fixture = Fixture(Path(temporary), agent, server)
        try:
            check_round_trip(fixture)
            check_manifests(fixture)
            check_link_races(fixture)
            check_restore_failures(fixture, shim)
            check_parent_replacement(fixture)
        finally:
            fixture.close()


if __name__ == "__main__":
    run(*sys.argv[1:4])
