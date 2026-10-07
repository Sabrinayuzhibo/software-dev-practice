#!/usr/bin/env python3
"""Compare 64 MiB and 1 GiB transfers and record real process peak RSS."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import time

from backup_integration import Fixture


def peak_rss(process):
    fields = Path(f"/proc/{process.pid}/status").read_text().splitlines()
    return int(next(line for line in fields if line.startswith("VmHWM:")).split()[1])


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-streaming-") as temporary:
        root = Path(temporary)
        samples = []
        for size_mib in (64, 1024):
            sample_root = root / str(size_mib)
            sample_root.mkdir()
            fixture = Fixture(sample_root, agent, server)
            try:
                source = sample_root / "source"
                source.mkdir()
                large = source / "large.bin"
                with large.open("wb") as stream:
                    stream.truncate(size_mib * 1024 * 1024)
                # More than one manifest page, including a directory-only subtree.
                for index in range(120):
                    (source / f"empty-{index}").mkdir()
                task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
                started = time.monotonic()
                version = fixture.backup(task["id"])
                backup_seconds = time.monotonic() - started
                started = time.monotonic()
                fixture.restore(version["id"], sample_root / "restored")
                restore_seconds = time.monotonic() - started
                assert digest(large) == digest(sample_root / "restored/large.bin")
                assert len(list((sample_root / "restored").iterdir())) == 121
                samples.append({
                    "size_mib": size_mib,
                    "backup_seconds": round(backup_seconds, 2),
                    "restore_seconds": round(restore_seconds, 2),
                    "agent_peak_kib": peak_rss(fixture.agent),
                    "server_peak_kib": peak_rss(fixture.server),
                })
                print(json.dumps(samples[-1]), flush=True)
            finally:
                fixture.close()
        for key in ("agent_peak_kib", "server_peak_kib"):
            assert samples[1][key] < samples[0][key] + 32 * 1024, samples
            assert samples[1][key] < 128 * 1024, samples
        print("PASS: 64 MiB/1 GiB exact roundtrip, paged directory manifest, bounded RSS (AC-28)")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
