#!/usr/bin/env python3
"""Selected sources stay bounded across tasks, uploads, history and restore."""

import json
import os
from pathlib import Path
import socket
import sys
import tempfile

from backup_faults import Proxy
from backup_integration import Fixture, rpc, stop, tree


def run(agent, server):
    with tempfile.TemporaryDirectory(prefix="backup-sources-") as temporary:
        root = Path(temporary)
        fixture = Fixture(root, agent, server)
        proxy = None
        checks = 0

        def passed(label):
            nonlocal checks
            checks += 1
            print(f"PASS {checks:02d}: {label}", flush=True)

        def task(paths, name=""):
            return fixture.request("add_task", {
                "sources": [str(path) for path in paths],
                "target_id": "local", "name": name,
            })

        def version_entries(version):
            path = root / "repository/storage/versions" / version["id"] / "entries.jsonl"
            return [json.loads(line) for line in path.read_text().splitlines()]

        try:
            legacy = root / "legacy"
            legacy.mkdir()
            (legacy / "empty").mkdir()
            (legacy / ".hidden").touch()
            old = fixture.request("add_task", {"path": str(legacy), "target_id": "local"})
            assert fixture.request("config")["format"] == 1
            assert task([legacy, legacy / ".hidden", legacy])["id"] == old["id"]
            whole = fixture.backup(old["id"])
            fixture.restore(whole["id"], root / "legacy-output")
            assert tree(root / "legacy-output") == tree(legacy)
            passed("legacy folder tasks merge covered children and restore without extra nesting")

            source = root / "sources"
            source.mkdir()
            one = source / "one file.txt"
            one.write_text("chosen content")
            ignored = source / "unreadable-sibling"
            ignored.write_text("private")
            ignored.chmod(0)
            (source / "unselected-empty").mkdir()
            single = task([one], "One file")
            assert single["path"] == str(source)
            assert single["selection"] == [{"path": one.name, "type": "file"}]
            assert fixture.request("config")["format"] == 2
            fixture.request("import_tasks", {"tasks": [{
                "id": "invalid-legacy-selection", "path": str(source),
                "createdAt": "2026-10-08", "selection": single["selection"],
            }]}, ok=False)
            assert not fixture.request("config").get("legacy_imported", False)
            scan = fixture.request("scan", {"task_id": single["id"]})
            assert scan["complete"] and scan["files"] == 1 and scan["directories"] == 0
            first = fixture.backup(single["id"])
            fixture.restore(first["id"], source / "restored-sibling")
            assert (source / "restored-sibling" / one.name).read_text() == "chosen content"
            assert len(tree(source / "restored-sibling")) == 1
            assert first["selection"] == single["selection"] and first["source_name"] == "One file"
            ignored.chmod(0o600)
            passed("single file ignores unreadable siblings and restores into a sibling directory")

            for name in ("left", "right"):
                (source / name).mkdir()
                (source / name / "same.txt").write_text(name)
                (source / name / "ignored.txt").write_text("outside selection")
            chosen = [source / "left/same.txt", source / "right/same.txt", one]
            multiple = task(chosen)
            assert task(list(reversed(chosen)) + [one])["id"] == multiple["id"]
            assert task([source / "left/ignored.txt"])["id"] != multiple["id"]
            version = fixture.backup(multiple["id"])
            output = root / "multi-output"
            fixture.restore(version["id"], output)
            assert set(tree(output)) == {"left", "right", "left/same.txt", "right/same.txt", one.name}
            assert (output / "left/same.txt").read_text() == "left"
            assert (output / "right/same.txt").read_text() == "right"
            passed("multiple files preserve same-name paths, deduplicate order and exclude siblings")

            selected_folder = source / "selected-folder"
            (selected_folder / "empty").mkdir(parents=True)
            (selected_folder / ".hidden").write_text("folder content")
            mixed = task([selected_folder, one, selected_folder / ".hidden"])
            assert len(mixed["selection"]) == 2
            plan = fixture.request("preview_sources", {
                "sources": [str(one), str(selected_folder), str(selected_folder / ".hidden")],
                "target_id": "local",
            })
            assert plan["merged_count"] == 1 and plan["scope"]["selection"] == mixed["selection"]
            mixed_version = fixture.backup(mixed["id"])
            fixture.restore(mixed_version["id"], root / "mixed-output")
            assert set(tree(root / "mixed-output")) == {
                one.name, "selected-folder", "selected-folder/empty", "selected-folder/.hidden",
            }
            folders = task([source / "left", source / "right"])
            folder_version = fixture.backup(folders["id"])
            fixture.restore(folder_version["id"], root / "folders-output")
            assert (root / "folders-output/left/ignored.txt").exists()
            passed("mixed sources and multiple folders include their full trees and empty directories")

            specials = root / "specials"
            specials.mkdir()
            payload = specials / "payload"
            payload.write_text("shared payload")
            alias = specials / "alias"
            os.link(payload, alias)
            symbolic = specials / "dangling"
            symbolic.symlink_to("../missing")
            pipe = specials / "pipe"
            os.mkfifo(pipe)
            special_task = task([alias, payload, symbolic, pipe])
            special_version = fixture.backup(special_task["id"])
            assert int(special_version["hardlinks"]) == 1 and int(special_version["symlinks"]) == 1
            entries = version_entries(special_version)
            assert sum(item["type"] == "file" for item in entries) == 1
            fixture.restore(special_version["id"], root / "special-output")
            assert (root / "special-output/alias").stat().st_ino == (root / "special-output/payload").stat().st_ino
            assert os.readlink(root / "special-output/dangling") == "../missing"
            for chosen_special in (symbolic, pipe):
                result = fixture.backup(task([chosen_special])["id"])
                assert int(result["entries"]) == 1 and int(result["files"]) == 0
            passed("explicit symlink, FIFO and selected hardlink groups retain FR-10 semantics")

            changing = root / "changing"
            changing.mkdir()
            mutable = changing / "file"
            mutable.touch()
            changing_task = task([mutable])
            mutable.unlink()
            missing = fixture.request("scan", {"task_id": changing_task["id"]})
            assert not missing["complete"] and missing["error_count"] >= 1
            mutable.mkdir()
            (mutable / "unexpected").write_text("must not expand scope")
            mismatch = fixture.request("scan", {"task_id": changing_task["id"]})
            assert not mismatch["complete"] and mismatch["files"] == 0
            failed = fixture.request("start_backup", {"task_id": changing_task["id"]})
            assert "type changed" in fixture.wait(failed["operation_id"], False)["error"]
            fixture.request("remove_task", {"id": changing_task["id"]})
            passed("missing roots fail and replaced file types never expand a saved selection")

            selected_root = root / "root-race"
            selected_root.mkdir()
            (selected_root / "file").write_text("original")
            root_task = task([selected_root / "file"])
            selected_root.rename(root / "root-moved")
            selected_root.symlink_to(source, target_is_directory=True)
            invalid_root = fixture.request("scan", {"task_id": root_task["id"]})
            assert not invalid_root["complete"] and invalid_root["files"] == 0
            fixture.request("remove_task", {"id": root_task["id"]})
            selected_root.unlink()
            passed("a task root replaced by a symlink is rejected without reading its target")

            def mutate_sibling(request, _reply):
                if request["type"] == "chunk":
                    (source / "new-unselected-file").write_text("unrelated change")
                return False

            proxy = Proxy(fixture.port, mutate_sibling)
            fixture.request("save_target", {
                "id": "local", "name": "Selection proxy", "host": "127.0.0.1", "port": proxy.listen_port,
            })
            unaffected = fixture.backup(single["id"])
            assert int(unaffected["files"]) == 1
            proxy.close()
            proxy = None

            changed = False

            def mutate_selected(request, _reply):
                nonlocal changed
                if request["type"] == "chunk" and not changed:
                    changed = True
                    one.write_text("changed during upload")
                return False

            proxy = Proxy(fixture.port, mutate_selected)
            fixture.request("save_target", {
                "id": "local", "name": "Selection proxy", "host": "127.0.0.1", "port": proxy.listen_port,
            })
            failed = fixture.request("start_backup", {"task_id": single["id"]})
            assert "changed" in fixture.wait(failed["operation_id"], False)["error"]
            proxy.close()
            proxy = None
            fixture.configure_target()
            assert all(item["id"] != failed["operation_id"] for item in fixture.request("versions", {"target_id": "local"})["versions"])
            passed("unselected sibling changes are ignored; selected file changes abort publication")

            # The source root may contain application data, but selected paths may not.
            peer = root / "beside-repository.txt"
            peer.write_text("safe sibling")
            fixture.backup(task([peer])["id"])
            fixture.request("ping", {"target_id": "local"})
            for prohibited in (root, root / "repository", root / "agent-state/config.json"):
                error = fixture.request("add_task", {
                    "sources": [str(prohibited)], "target_id": "local",
                }, ok=False)
                assert "overlaps" in error["message"]
            for invalid in ([], [str(peer)] * 101, [str(root / "missing")]):
                fixture.request("preview_sources", {"sources": invalid, "target_id": "local"}, ok=False)
            fixture.restore(first["id"], selected_folder / "restore", False)
            passed("precise scope isolation protects source, repository and Agent data, with bounded inputs")

            token = json.loads((root / "repository/database/access.json").read_text())["token"]
            with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as connection:
                rpc(connection, "authenticate", {"token": token})
                rpc(connection, "begin", {"source": str(source), "task_id": "selected", "operation_id": "outside-scope", "selection": single["selection"]})
                rpc(connection, "entry", {"path": "unselected", "type": "directory"}, ok=False)
            with socket.create_connection(("127.0.0.1", fixture.port), timeout=5) as connection:
                rpc(connection, "authenticate", {"token": token})
                rpc(connection, "begin", {"source": str(source), "task_id": "selected", "operation_id": "missing-selection", "selection": single["selection"]})
                rpc(connection, "commit", {"operation_id": "missing-selection"}, ok=False)
            summary_path = root / "repository/storage/versions" / first["id"] / "summary.json"
            original = summary_path.read_bytes()
            poisoned = json.loads(original)
            poisoned["selection"] = [{"path": "elsewhere", "type": "file"}]
            summary_path.write_text(json.dumps(poisoned))
            invalid = fixture.restore(first["id"], root / "tampered-restore", False)
            assert "selection" in invalid["error"] and not (root / "tampered-restore").exists()
            summary_path.write_bytes(original)
            passed("Server rejects out-of-scope or incomplete uploads; restore checks the saved scope")

            restricted = root / "search-only-parent"
            (restricted / "nested").mkdir(parents=True)
            (restricted / "chosen").write_text("root file")
            (restricted / "nested/chosen").write_text("nested file")
            restricted.chmod(0o111)
            (restricted / "nested").chmod(0o111)
            try:
                readable = task([restricted / "chosen", restricted / "nested/chosen"])
                readable_version = fixture.backup(readable["id"])
                fixture.restore(readable_version["id"], root / "search-only-output")
                assert (root / "search-only-output/nested/chosen").read_text() == "nested file"
            finally:
                restricted.chmod(0o700)
                (restricted / "nested").chmod(0o700)
            passed("selected readable files require search permission, not listing access to their parents")

            stop(fixture.agent)
            fixture.start_agent()
            config = fixture.request("config")
            assert next(item for item in config["tasks"] if item["id"] == single["id"]) == single
            assert next(item for item in config["tasks"] if item["id"] == old["id"]) == old
            record = fixture.request("operation", {"id": first["id"]})
            assert record["selection"] == single["selection"] and record["source_name"] == "One file"
            fixture.request("remove_task", {"id": single["id"]})
            stop(fixture.server)
            fixture.start_server()
            fixture.restore(first["id"], root / "history-output")
            assert (root / "history-output" / one.name).read_text() == "chosen content"
            versions = fixture.request("versions", {"target_id": "local", "task_id": single["id"]})["versions"]
            assert all(item["selection"] == single["selection"] for item in versions)
            passed("configuration upgrade preserves IDs; selections survive restart, edits and task deletion")
        finally:
            if proxy:
                proxy.close()
            fixture.close()
        print(f"PASS: {checks} selected-source integration scenarios", flush=True)


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
