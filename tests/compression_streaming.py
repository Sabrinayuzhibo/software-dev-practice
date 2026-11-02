#!/usr/bin/env python3
"""比较两种算法处理 64 MiB 和 1 GiB 稀疏源文件时的实际内存占用。"""

import json
from pathlib import Path
import shutil
import sys
import tempfile
import time

from backup_compression import policy
from backup_integration import Fixture
from backup_streaming import digest, peak_rss


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-codec-streaming-") as temporary:
        root = Path(temporary)
        for algorithm in ("huffman", "lz77"):
            measurements = []
            for size_mib in (64, 1024):
                sample = root / f"{algorithm}-{size_mib}"
                sample.mkdir()
                fixture = Fixture(sample, agent, server)
                try:
                    source = sample / "source"
                    source.mkdir()
                    data = source / "large.bin"
                    with data.open("wb") as output:
                        output.truncate(size_mib * 1024 * 1024)
                    policy(fixture, algorithm)
                    task = fixture.request("add_task", {"target_id": "local", "path": str(source)})
                    started = time.monotonic()
                    backup = fixture.request("start_backup", {"task_id": task["id"]})
                    version = fixture.wait(backup["operation_id"], timeout=240)["result"]["version"]
                    restore = fixture.request("start_restore", {
                        "target_id": "local", "version_id": version["id"],
                        "destination": str(sample / "restored"),
                    })
                    fixture.wait(restore["operation_id"], timeout=240)
                    assert digest(data) == digest(sample / "restored/large.bin")
                    assert int(version["stored_bytes"]) < data.stat().st_size / 4
                    measurements.append({
                        "algorithm": algorithm, "size_mib": size_mib,
                        "stored_bytes": version["stored_bytes"],
                        "seconds": round(time.monotonic() - started, 2),
                        "agent_peak_kib": peak_rss(fixture.agent),
                        "server_peak_kib": peak_rss(fixture.server),
                    })
                    print(json.dumps(measurements[-1]), flush=True)
                finally:
                    fixture.close()
                # 大文件输出仅用于本次验证，检查后立即清理。
                shutil.rmtree(sample)
            for field in ("agent_peak_kib", "server_peak_kib"):
                assert measurements[1][field] < measurements[0][field] + 32 * 1024, measurements
                assert measurements[1][field] < 128 * 1024, measurements
        print("PASS: both codecs restore 64 MiB/1 GiB exactly with bounded process memory", flush=True)


if __name__ == "__main__":
    run(*sys.argv[1:])
