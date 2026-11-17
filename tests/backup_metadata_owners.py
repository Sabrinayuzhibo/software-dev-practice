#!/usr/bin/env python3
"""私有命名空间内验证真实属主权限失败，不修改宿主文件属主。"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from backup_integration import Fixture, tree
from backup_metadata import attributes, warnings


def check(agent, server):
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    with tempfile.TemporaryDirectory(prefix="backup-metadata-owners-") as temporary:
        root = Path(temporary)
        source = root / "source"
        source.mkdir()
        (source / "local").write_text("local data")
        foreign = source / "foreign"
        foreign.touch()
        subprocess.run(["mount", "--bind", "/etc/hostname", str(foreign)], check=True)
        fixture = None
        try:
            subprocess.run(
                ["mount", "-o", "remount,bind,ro,nodev,nosuid,noexec", str(foreign)],
                check=True,
            )
            before = attributes(foreign)
            assert before[1] != os.getuid()
            fixture = Fixture(root, agent, server)
            task = fixture.request(
                "add_task", {"path": str(source), "target_id": "local"}
            )
            version = fixture.backup(task["id"])
            destination = root / "restored"
            record = fixture.restore(version["id"], destination)
            assert record["state"] == "SUCCEEDED_WITH_WARNINGS", record
            assert record["result"]["metadata_warning_count"] == 1
            items = warnings(fixture, record)
            assert len(items) == 1 and items[0]["path"] == "foreign"
            assert items[0]["field"] == "owner/group"
            assert tree(destination) == tree(source)
            assert attributes(foreign) == before
            assert attributes(destination / "local") == attributes(source / "local")
            restored = attributes(destination / "foreign")
            assert restored[0] == before[0] and restored[3] == before[3]
            print(
                "PASS: 实际不可映射 UID/GID 恢复产生明确属主警告，内容/权限/mtime 仍恢复，宿主只读源不变",
                flush=True,
            )
        finally:
            if fixture:
                fixture.close()
            subprocess.run(["umount", str(foreign)], check=True)


if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[3] == "--inside":
        check(*sys.argv[1:3])
    else:
        if not shutil.which("unshare") or not shutil.which("mount"):
            print("SKIP: 缺少私有命名空间工具")
            sys.exit(77)
        probe = subprocess.run(
            ["unshare", "--user", "--map-root-user", "--mount", "true"],
            capture_output=True,
            text=True,
        )
        if probe.returncode:
            print("SKIP: 无法创建私有命名空间：" + probe.stderr.strip())
            sys.exit(77)
        result = subprocess.run(
            [
                "unshare",
                "--user",
                "--map-root-user",
                "--mount",
                "--fork",
                sys.executable,
                os.path.abspath(__file__),
                *sys.argv[1:3],
                "--inside",
            ]
        )
        sys.exit(result.returncode)
