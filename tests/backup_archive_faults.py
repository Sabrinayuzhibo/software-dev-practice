#!/usr/bin/env python3
"""Archive publication, snapshot, transfer failures and real ENOSPC."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading

from backup_archive import run_operation
from backup_faults import Proxy
from backup_integration import Fixture, stop


def network(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-archive-fault-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent, server)
        proxy = None
        release = threading.Event()
        try:
            source = root / "source"
            source.mkdir()
            data = b"archive data" * 100000
            (source / "file").write_bytes(data)
            task = fixture.request(
                "add_task", {"path": str(source), "target_id": "local"}
            )
            version = fixture.backup(task["id"])
            archive = root / "version.bsar"
            run_operation(
                fixture,
                "export_archive",
                {"version_id": version["id"], "archive_path": str(archive)},
            )

            def attach(hook):
                nonlocal proxy
                fixture.configure_target()
                if proxy:
                    proxy.close()
                proxy = Proxy(fixture.port, hook)
                fixture.request(
                    "save_target",
                    {
                        "id": "local",
                        "name": "Archive fault proxy",
                        "host": "127.0.0.1",
                        "port": proxy.listen_port,
                    },
                )

            attach(lambda request, reply: request["type"] == "download")
            failed_export = root / "failed.bsar"
            run_operation(
                fixture,
                "export_archive",
                {"version_id": version["id"], "archive_path": str(failed_export)},
                False,
            )
            assert not failed_export.exists() and not list(
                root.glob(".backup-archive-*")
            )

            changed = root / "changing.bsar"
            shutil.copyfile(archive, changed)
            changed_once = False

            def mutate_original(request, reply):
                nonlocal changed_once
                if request["type"] == "begin_import" and not changed_once:
                    changed_once = True
                    changed.write_bytes(b"changed after full verification")
                return False

            attach(mutate_original)
            imported = run_operation(
                fixture, "import_archive", {"archive_path": str(changed)}
            )
            assert changed_once
            fixture.configure_target()
            fixture.restore(
                imported["result"]["version"]["id"], root / "snapshot-output"
            )
            assert (root / "snapshot-output/file").read_bytes() == data

            dropped = False

            def lose_receipt(request, reply):
                nonlocal dropped
                if request["type"] == "commit" and not dropped:
                    dropped = True
                    return True
                return False

            attach(lose_receipt)
            before = fixture.request("versions", {"target_id": "local"})["total"]
            run_operation(fixture, "import_archive", {"archive_path": str(archive)})
            assert (
                dropped
                and fixture.request("versions", {"target_id": "local"})["total"]
                == before + 1
            )

            committed = threading.Event()
            allow = threading.Event()

            def block_confirmation(request, reply):
                if request["type"] == "commit":
                    committed.set()
                return committed.is_set() and not allow.is_set()

            attach(block_confirmation)
            result = fixture.request(
                "start_import_archive",
                {"target_id": "local", "archive_path": str(archive)},
            )
            waiting = fixture.wait(result["operation_id"], False)
            assert (
                waiting["state"] == "WAITING" and "COMMIT_UNCERTAIN" in waiting["error"]
            )
            allow.set()
            confirmed = fixture.request(
                "confirm", {"target_id": "local", "id": waiting["id"]}
            )
            assert (
                confirmed["state"].startswith("SUCCEEDED")
                and confirmed["result"]["version"]["id"] == waiting["id"]
            )

            arrived = threading.Event()

            def hold_chunk(request, reply):
                if request["type"] == "chunk":
                    arrived.set()
                    release.wait(10)
                    return True
                return False

            attach(hold_chunk)
            before = fixture.request("versions", {"target_id": "local"})["total"]
            started = fixture.request(
                "start_import_archive",
                {"target_id": "local", "archive_path": str(archive)},
            )
            assert arrived.wait(10), "import did not reach content upload"
            fixture.agent.kill()
            fixture.agent.wait(timeout=5)
            stop(fixture.agent)
            fixture.agent = None
            release.set()
            proxy.close()
            proxy = None
            fixture.start_agent()
            record = fixture.request("operation", {"id": started["operation_id"]})
            assert record["state"] == "INTERRUPTED", record
            assert (
                fixture.request("versions", {"target_id": "local"})["total"] == before
            )
            fixture.restore(version["id"], root / "old-after-interruption")
            assert (root / "old-after-interruption/file").read_bytes() == data
            print(
                "PASS: failed export cleanup, immutable import snapshot, lost receipt confirmation, WAITING recovery and interrupted import preserves old versions",
                flush=True,
            )
        finally:
            release.set()
            fixture.close()
            if proxy:
                proxy.close()


def disk(agent, server):
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    with tempfile.TemporaryDirectory(prefix="backup-archive-enospc-") as temporary:
        root = Path(temporary)
        output = root / "limited-output"
        output.mkdir()
        repository = root / "target/repository"
        repository.mkdir(parents=True)
        mounted = []
        original = target = None
        try:
            for folder, size in ((output, "1M"), (repository, "2M")):
                subprocess.run(
                    [
                        "mount",
                        "-t",
                        "tmpfs",
                        "-o",
                        f"size={size}",
                        "tmpfs",
                        str(folder),
                    ],
                    check=True,
                )
                mounted.append(folder)
            original = Fixture(root / "original", agent, server)
            target = Fixture(root / "target", agent, server)
            source = root / "source"
            source.mkdir()
            (source / "small").write_bytes(b"stable")
            task = original.request(
                "add_task", {"path": str(source), "target_id": "local"}
            )
            old = original.backup(task["id"])
            small = root / "small.bsar"
            run_operation(
                original,
                "export_archive",
                {"version_id": old["id"], "archive_path": str(small)},
            )
            kept = run_operation(
                target, "import_archive", {"archive_path": str(small)}
            )["result"]["version"]
            (source / "large").write_bytes(b"x" * (3 * 1024 * 1024))
            large = original.backup(task["id"])
            failed = run_operation(
                original,
                "export_archive",
                {"version_id": large["id"], "archive_path": str(output / "full.bsar")},
                False,
            )
            assert "space" in failed["error"].lower(), failed
            assert not (output / "full.bsar").exists() and not list(
                output.glob(".backup-archive-*")
            )
            complete = root / "large.bsar"
            run_operation(
                original,
                "export_archive",
                {"version_id": large["id"], "archive_path": str(complete)},
            )
            failed = run_operation(
                target, "import_archive", {"archive_path": str(complete)}, False
            )
            assert "space" in failed["error"].lower(), failed
            assert target.request("versions", {"target_id": "local"})["total"] == 1
            target.restore(kept["id"], root / "old-after-enospc")
            assert (root / "old-after-enospc/small").read_bytes() == b"stable"
            print(
                "PASS: real export/import ENOSPC leaves no published partial archive/version and old version remains restorable",
                flush=True,
            )
        finally:
            if target:
                target.close()
            if original:
                original.close()
            for folder in reversed(mounted):
                subprocess.run(["umount", str(folder)], check=True)


if __name__ == "__main__":
    agent, server = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
    if "--disk" in sys.argv:
        disk(agent, server)
    elif "--disk-probe" in sys.argv:
        if not shutil.which("unshare") or not shutil.which("mount"):
            print("SKIP: private mount namespace tools unavailable")
            sys.exit(77)
        probe = subprocess.run(
            ["unshare", "--user", "--map-root-user", "--mount", "true"],
            capture_output=True,
        )
        if probe.returncode:
            print("SKIP: private mount namespace unavailable")
            sys.exit(77)
        sys.exit(
            subprocess.run(
                [
                    "unshare",
                    "--user",
                    "--map-root-user",
                    "--mount",
                    "--fork",
                    sys.executable,
                    str(Path(__file__).resolve()),
                    agent,
                    server,
                    "--disk",
                ]
            ).returncode
        )
    else:
        network(agent, server)
