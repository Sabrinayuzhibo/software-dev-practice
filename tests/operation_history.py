#!/usr/bin/env python3
"""Execution summaries, shared warnings and legacy migration on real Agents."""

import json
from pathlib import Path
import sys
import tempfile

from backup_integration import Fixture, stop


def warning_pages(fixture, operation_id):
    warnings = []
    offset = 0
    sizes = []
    while offset is not None:
        page = fixture.request("operation_warnings", {"id": operation_id, "offset": offset})
        sizes.append(len(json.dumps(page, ensure_ascii=False).encode()))
        assert len(page["warnings"]) <= 100 and sizes[-1] < 70 * 1024
        warnings.extend(page["warnings"])
        assert page["next_offset"] is None or page["next_offset"] > offset
        offset = page["next_offset"]
    assert len(warnings) == page["total"]
    return warnings, sizes


def run(agent_binary, server_binary):
    with tempfile.TemporaryDirectory(prefix="backup-history-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent_binary, server_binary)
        try:
            stop(fixture.agent)
            state = root / "agent-state"
            records = state / "operations"
            warnings = [{"path": f"project/dependencies/{index:04}/" + "x" * 64,
                         "reason": "Special file skipped in basic backup"}
                        for index in range(1899)]
            originals = {}
            for index in range(10):
                # Reverse traversal order in alternate records: contents are equal.
                entries = warnings if index % 2 else warnings[::-1]
                record = {
                    "id": f"legacy-{index}", "action": "scan" if index < 9 else "backup",
                    "state": "SUCCEEDED_WITH_WARNINGS", "stage": "scan",
                    "task_id": "old-task", "source": "/old/source", "files": index,
                    "bytes": index * 10, "target": {"id": "local"},
                    "started_at": f"2026-10-01T00:00:{index:02}.000Z",
                    "finished_at": f"2026-10-01T00:01:{index:02}.000Z",
                    "warnings": entries, "result": {"warnings": entries, "sample": []},
                }
                path = records / f"legacy-{index}.json"
                path.write_text(json.dumps(record), encoding="utf8")
                assert path.stat().st_size < 1024 * 1024
                originals[path.name] = path.read_bytes()

            # Force compaction to fail before any reference is published.
            (state / "warnings").write_text("blocked")
            fixture.start_agent()
            summary = fixture.request("operations")
            assert summary["total"] == 10 and len(json.dumps(summary)) < 16000
            assert all("warnings" not in item and "result" not in item
                       and item["warning_count"] == 1899 for item in summary["operations"])
            for name, content in originals.items():
                assert (records / name).read_bytes() == content
            assert len(warning_pages(fixture, "legacy-0")[0]) == 1899
            print("PASS: legacy summaries remain small and readable when compaction fails", flush=True)

            stop(fixture.agent)
            (state / "warnings").unlink()
            fixture.start_agent()
            blobs = list((state / "warnings").glob("*.jsonl"))
            assert len(blobs) == 1
            compact = [json.loads((records / name).read_text()) for name in originals]
            assert len({record["warning_ref"] for record in compact}) == 1
            assert all("warnings" not in record and "warnings" not in record["result"]
                       for record in compact)
            assert all(record["files"] == index for index, record in enumerate(compact))
            assert sum((records / name).stat().st_size for name in originals) < 16000
            assert sorted(warning_pages(fixture, "legacy-0")[0], key=lambda item: item["path"]) == warnings

            # Simulate interruption after shared data was written, before the
            # legacy record was replaced. Restart must reuse the verified object.
            stop(fixture.agent)
            (records / "legacy-0.json").write_bytes(originals["legacy-0.json"])
            fixture.start_agent()
            assert len(list((state / "warnings").glob("*.jsonl"))) == 1
            assert "warnings" not in json.loads((records / "legacy-0.json").read_text())
            print("PASS: ten equal warning sets share one object; restart finishes interrupted compaction", flush=True)

            original_blob = blobs[0].read_bytes()
            blobs[0].write_bytes(original_blob.replace(b"Special", b"Altered", 1))
            failure = fixture.request("operation_warnings", {"id": "legacy-0"}, ok=False)
            assert "integrity" in failure["message"]
            assert fixture.request("operations")["total"] == 10
            assert fixture.agent.poll() is None
            blobs[0].write_bytes(original_blob)
            fixture.request("operation_warnings", {"id": "legacy-0", "offset": -1}, ok=False)
            print("PASS: corrupt warnings and invalid offsets fail only their query", flush=True)

            stop(fixture.agent)
            for index in range(107):
                record = {
                    "id": f"extra-{index:03}", "action": "scan", "task_id": "other-task",
                    "source": "/another/source", "target": {"id": "local"},
                    "state": "FAILED" if index == 0 else "INTERRUPTED" if index == 1 else "SUCCEEDED",
                    "files": 0, "bytes": 0, "warning_count": 0,
                    "started_at": f"2026-10-02T00:{index // 60:02}:{index % 60:02}.000Z",
                }
                (records / f"extra-{index:03}.json").write_text(json.dumps(record))
            fixture.start_agent()
            groups = fixture.request("operations", {"group_scans": True})
            assert groups["total"] == 3 and groups["execution_total"] == 117
            first = groups["operations"][0]
            assert first["scan_count"] == 107 and first["failure_count"] == 2
            assert groups["operations"][1]["action"] == "backup"
            assert groups["operations"][2]["scan_count"] == 9
            assert groups["operations"][2]["warning_runs"] == 9
            history = []
            offset = 0
            while offset is not None:
                page = fixture.request("operations", {"scan_group": first["scan_group"], "offset": offset})
                history.extend(page["operations"])
                offset = page["next_offset"]
            assert len(history) == 107 and len({item["id"] for item in history}) == 107
            print("PASS: scan grouping spans all pages and preserves failures, warning counts and backups", flush=True)

            source = root / "large-warnings"
            source.mkdir()
            (source / "data.txt").write_text("one")
            nested = source
            for index in range(15):
                nested /= f"{index:02}-" + "x" * 175
                nested.mkdir()
            for index in range(420):
                (nested / f"link-{index:03}").symlink_to("missing")
            task = fixture.request("add_task", {"path": str(source), "target_id": "local"})

            def scan():
                operation_id = fixture.request("start_scan", {"task_id": task["id"]})["operation_id"]
                record = fixture.wait(operation_id)
                stored = json.loads((records / f"{operation_id}.json").read_text())
                assert "warnings" not in record and "warnings" not in record["result"]
                assert len(json.dumps(fixture.request("operation", {"id": operation_id, "summary": True}))) < 6000
                return record, stored

            first, first_stored = scan()
            assert first["warning_count"] == 420
            fetched, sizes = warning_pages(fixture, first["id"])
            assert len(fetched) == 420 and len(sizes) > 5
            assert (state / "warnings" / (first_stored["warning_ref"] + ".jsonl")).stat().st_size > 1024 * 1024
            (source / "data.txt").write_text("changed file contents")
            second, second_stored = scan()
            assert first_stored["warning_ref"] == second_stored["warning_ref"]
            assert first["bytes"] != second["bytes"]
            def content_hash(record):
                return next(item["sha256"] for item in record["result"]["sample"]
                            if item["path"] == "data.txt")

            assert content_hash(first) != content_hash(second)
            (nested / "new-link").symlink_to("missing")
            third, third_stored = scan()
            assert third["warning_count"] == 421
            assert third_stored["warning_ref"] != first_stored["warning_ref"]
            stop(fixture.agent)
            fixture.start_agent()
            assert len(warning_pages(fixture, first["id"])[0]) == 420
            assert len(warning_pages(fixture, third["id"])[0]) == 421
            assert fixture.request("ping", {"target_id": "local"})
            print("PASS: warnings over 1 MiB paginate by bytes; repeated scans share details but preserve changed file results", flush=True)
            for index in range(400):
                (nested / f"extra-{index:03}").symlink_to("missing")
            agent_pid = fixture.agent.pid
            failure = fixture.request("scan", {"task_id": task["id"]}, ok=False)
            assert "Response too large" in failure["message"]
            assert fixture.agent.pid == agent_pid and fixture.agent.poll() is None
            assert fixture.request("operations")["operations"][0]["warning_count"] == 821
            assert fixture.request("ping", {"target_id": "local"})
            print("PASS: Agent emits a bounded error for an oversized legacy reply and accepts the next request", flush=True)
        finally:
            fixture.close()


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
