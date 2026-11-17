#!/usr/bin/env python3
"""64 MiB / 1 GiB archive transfers with process peak-memory bounds."""

import hashlib
from pathlib import Path
import sys
import tempfile

from backup_archive import run_operation
from backup_integration import Fixture


def peak(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmHWM:"):
            return int(line.split()[1])
    raise AssertionError("Missing process memory metric")


def run(agent, server):
    profiles = []
    for mib in (64, 1024):
        with tempfile.TemporaryDirectory(prefix="backup-archive-stream-") as temporary:
            root = Path(temporary)
            fixture = Fixture(root, agent, server)
            try:
                source = root / "source"
                source.mkdir()
                contents = source / "large"
                digest = hashlib.sha256()
                block = bytes(range(256)) * 4096
                with contents.open("wb") as output:
                    for _ in range(mib):
                        output.write(block)
                        digest.update(block)
                    output.write(b"tail")
                    digest.update(b"tail")
                task = fixture.request(
                    "add_task", {"path": str(source), "target_id": "local"}
                )
                version = fixture.backup(task["id"])
                contents.unlink()
                archive = root / "large.bsar"
                run_operation(
                    fixture,
                    "export_archive",
                    {"version_id": version["id"], "archive_path": str(archive)},
                )
                imported = run_operation(
                    fixture, "import_archive", {"archive_path": str(archive)}
                )["result"]["version"]
                destination = root / "restored"
                fixture.restore(imported["id"], destination)
                actual = hashlib.sha256()
                with (destination / "large").open("rb") as restored:
                    for chunk in iter(lambda: restored.read(1024 * 1024), b""):
                        actual.update(chunk)
                assert actual.digest() == digest.digest()
                usage = (peak(fixture.agent.pid), peak(fixture.server.pid))
                assert max(usage) < 256 * 1024, usage
                profiles.append(usage)
                print(
                    f"PASS: {mib} MiB archive export/import/restore; Agent/Server peak KiB={usage}",
                    flush=True,
                )
            finally:
                fixture.close()
    for small, large in zip(profiles[0], profiles[1]):
        assert large < small + 64 * 1024, profiles
    print(
        "PASS: archive file-size growth does not cause linear process-memory growth",
        flush=True,
    )


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()))
