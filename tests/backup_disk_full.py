#!/usr/bin/env python3
"""Real ENOSPC on bounded tmpfs mounts in a private user/mount namespace."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from backup_integration import Fixture, tree


def check(agent, server):
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    with tempfile.TemporaryDirectory(prefix="backup-disk-full-") as temporary:
        root = Path(temporary)
        repository = root / "repository"
        destination = root / "limited-output"
        repository.mkdir()
        destination.mkdir()
        mounted = []
        fixture = None
        try:
            for folder, size in [(repository, "8M"), (destination, "2M")]:
                subprocess.run(["mount", "-t", "tmpfs", "-o", f"size={size}",
                                "tmpfs", str(folder)], check=True)
                mounted.append(folder)
            fixture = Fixture(root, agent, server)
            source = root / "source"
            source.mkdir()
            (source / "small").write_bytes(b"stable" * 1024)
            task = fixture.request("add_task", {"target_id": "local", "path": str(source)})
            first = fixture.backup(task["id"])
            original = tree(source)
            (source / "large").write_bytes(b"x" * (12 * 1024 * 1024))
            operation = fixture.request("start_backup", {"task_id": task["id"]})["operation_id"]
            failed = fixture.wait(operation, False)
            assert "No space left" in failed["error"], failed
            assert fixture.request("versions", {"target_id": "local"})["total"] == 1
            assert fixture.request("ping", {"target_id": "local"})["storage_state"] == "unavailable"
            fixture.restore(first["id"], root / "old-after-full")
            assert tree(root / "old-after-full") == original
            print("PASS: real repository ENOSPC fails backup; old version remains restorable", flush=True)

            # Remove only this failed fixture's staging files to make room for
            # a completed version larger than the separate restore filesystem.
            shutil.rmtree(repository / "storage/staging" / operation)
            (source / "large").write_bytes(b"x" * (5 * 1024 * 1024))
            second = fixture.backup(task["id"])
            failed = fixture.restore(second["id"], destination, False)
            assert "No space left" in failed["error"], failed
            assert failed["restore_journal"]
            page = fixture.request("restore_entries", {"id": failed["id"]})
            assert page["uncertain"] == 1 and page["total"] >= 1
            assert not (destination / "large").exists()
            assert not list(destination.glob(".backup-*"))
            fixture.restore(second["id"], root / "restored-after-full")
            assert tree(root / "restored-after-full") == tree(source)
            assert fixture.request("versions", {"target_id": "local"})["total"] == 2
            print("PASS: real restore ENOSPC reports partial journal; temporary file cleaned; version unchanged", flush=True)
        finally:
            if fixture:
                fixture.close()
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
                                 sys.executable, os.path.abspath(__file__),
                                 *sys.argv[1:3], "--inside"])
        sys.exit(result.returncode)
