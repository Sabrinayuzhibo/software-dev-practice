#!/usr/bin/env python3
"""Real device nodes and cross-device restore failures in a private namespace."""

import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

from backup_faults import Proxy
from backup_file_types import manifest_path, restore_pages, rewrite_manifest
from backup_integration import Fixture
from file_type_fixtures import create_socket


def check(agent, server):
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    with tempfile.TemporaryDirectory(prefix="backup-type-mounts-") as temporary:
        root = Path(temporary)
        source = root / "source"
        source.mkdir()
        mounted = []
        fixture = None
        proxy = None
        try:
            subprocess.run(["mount", "-t", "tmpfs", "-o", "size=8M,nodev", "tmpfs", str(source)], check=True)
            mounted.append(source)
            # User namespaces may forbid mknod. Read-only, nodev bind mounts
            # expose only existing node metadata and prohibit device I/O.
            block = next((path for path in Path("/dev").iterdir()
                          if stat.S_ISBLK(path.lstat().st_mode)), None)
            if block is None:
                print("SKIP: no block device node for metadata-only fixture")
                sys.exit(77)
            for name, device in [("character", Path("/dev/null")), ("block", block)]:
                path = source / name
                path.touch()
                subprocess.run(["mount", "--bind", str(device), str(path)], check=True)
                mounted.append(path)
                subprocess.run(["mount", "-o", "remount,bind,ro,nodev,nosuid,noexec", str(path)], check=True)
            create_socket(source / "socket")
            (source / "sub").mkdir()
            (source / "anchor").write_bytes(b"cross-device content")
            os.link(source / "anchor", source / "sub/alias")
            (source / "barrier").write_bytes(b"barrier")
            fixture = Fixture(root, agent, server)
            task = fixture.request("add_task", {"target_id": "local", "path": str(source)})
            scan = fixture.request("scan", {"task_id": task["id"]})
            assert scan["complete"]
            assert {item["type"] for item in scan["warnings"]} == {"character_device", "block_device", "socket"}
            version = fixture.backup(task["id"])
            assert len(version["warnings"]) == 3
            assert all(item["type"] in item["reason"] for item in version["warnings"])
            fixture.restore(version["id"], root / "ordinary-restore")
            for name in ("character", "block", "socket"):
                assert not os.path.lexists(root / "ordinary-restore" / name)
            print("PASS: actual character/block nodes and socket identified and skipped without opening device streams", flush=True)

            selected = fixture.request("add_task", {
                "target_id": "local", "sources": [str(source)],
                "file_types": ["character_device", "block_device", "socket"],
                "preserve_empty_dirs": False,
            })
            selected_scan = fixture.request("scan", {"task_id": selected["id"]})
            assert selected_scan["complete"] and not selected_scan["warnings"]
            assert all(selected_scan[field] == 1 for field in
                       ("character_devices", "block_devices", "sockets"))
            node_version = fixture.backup(selected["id"])
            assert node_version["format"] == 4 and not node_version["warnings"]
            node_directory = manifest_path(fixture, node_version)
            node_entries = {item["path"]: item for item in
                            (json.loads(line) for line in
                             (node_directory / "entries.jsonl").read_text().splitlines())}
            assert set(node_entries) == {"character", "block", "socket"}
            assert not list(node_directory.glob("*.data"))
            for name in ("character", "block"):
                source_info = (source / name).lstat()
                assert int(node_entries[name]["device_major"]) == os.major(source_info.st_rdev)
                assert int(node_entries[name]["device_minor"]) == os.minor(source_info.st_rdev)
            assert "device_major" not in node_entries["socket"]

            destination = root / "node-restore"
            destination.mkdir()
            subprocess.run(["mount", "-t", "tmpfs", "-o", "size=8M,nodev",
                            "tmpfs", str(destination)], check=True)
            mounted.append(destination)
            denied = []
            for name, mode in (("character", stat.S_IFCHR),
                               ("block", stat.S_IFBLK)):
                probe = destination / ("probe-" + name)
                try:
                    os.mknod(probe, mode | 0o600, (source / name).lstat().st_rdev)
                    probe.unlink()
                except PermissionError:
                    denied.append(name)
            can_restore_devices = not denied
            restored = fixture.restore(node_version["id"], destination,
                                       can_restore_devices)
            if can_restore_devices:
                assert restored["state"] == "SUCCEEDED"
                for name, mode in (("character", stat.S_ISCHR),
                                   ("block", stat.S_ISBLK),
                                   ("socket", stat.S_ISSOCK)):
                    assert mode((destination / name).lstat().st_mode)
                for name in ("character", "block"):
                    assert (destination / name).lstat().st_rdev == (source / name).lstat().st_rdev
                assert all(item["state"] == "written" for item in
                           restore_pages(fixture, restored["id"]))
            else:
                assert restored["state"] == "FAILED"
                assert any(name in restored["error"] for name in denied), restored
                assert "not permitted" in restored["error"].lower(), restored
                assert restore_pages(fixture, restored["id"])[-1]["state"] == "pending"
                assert all(not stat.S_ISREG(item.lstat().st_mode)
                           for item in destination.iterdir())
            print("PASS: opted-in device numbers and socket nodes saved without device payloads; restore respects node permissions", flush=True)

            directory = manifest_path(fixture, version)
            entries = [json.loads(line) for line in (directory / "entries.jsonl").read_text().splitlines()]
            original = next(item for item in entries if item["type"] == "file" and "link_group" in item)
            barrier = next(item for item in entries if item["path"] == "barrier")
            data = [(directory / f'{item["index"]}.data').read_bytes() for item in (original, barrier)]
            anchor = dict(original, path="anchor", link_group="anchor")
            alias = {"path": "sub/alias", "type": "hardlink", "link_to": "anchor",
                     "size": anchor["size"], "sha256": anchor["sha256"]}
            rewrite_manifest(directory, [anchor, {"path": "sub", "type": "directory"}, barrier, alias])
            (directory / "0.data").write_bytes(data[0])
            (directory / "2.data").write_bytes(data[1])
            destination = root / "cross-device-restore"

            def mount_destination(request, reply):
                if request["type"] == "download" and int(request["payload"]["index"]) == 2:
                    folder = destination / "sub"
                    if folder not in mounted:
                        subprocess.run(["mount", "-t", "tmpfs", "-o", "size=1M", "tmpfs", str(folder)], check=True)
                        mounted.append(folder)
                return False

            proxy = Proxy(fixture.port, mount_destination)
            fixture.request("save_target", {"id": "local", "name": "Mount proxy", "host": "127.0.0.1", "port": proxy.listen_port})
            failed = fixture.restore(version["id"], destination, False)
            assert destination.stat().st_dev != (destination / "sub").stat().st_dev
            assert "cross-device" in failed["error"].lower() and "sub/alias" in failed["error"], failed
            assert not (destination / "sub/alias").exists()
            assert (destination / "anchor").read_bytes() == data[0]
            assert restore_pages(fixture, failed["id"])[-1]["state"] == "pending"
            print("PASS: real EXDEV on a nested tmpfs explicitly fails hard-link restore without copying", flush=True)
        finally:
            if fixture:
                fixture.close()
            if proxy:
                proxy.close()
            for folder in reversed(mounted):
                subprocess.run(["umount", str(folder)], check=True)


if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[3] == "--inside":
        check(sys.argv[1], sys.argv[2])
    else:
        if not shutil.which("unshare") or not shutil.which("mount"):
            print("SKIP: private mount namespace tools unavailable")
            sys.exit(77)
        probe = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "true"],
                               capture_output=True, text=True)
        if probe.returncode:
            print("SKIP: private mount namespace unavailable: " + probe.stderr.strip())
            sys.exit(77)
        result = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "--fork",
                                 sys.executable, os.path.abspath(__file__), *sys.argv[1:3], "--inside"])
        sys.exit(result.returncode)
