#!/usr/bin/env python3
"""通过实际 TLS Server 验证远端认证、传输和实时任务自动补扫。"""

import json
from pathlib import Path
import select
import socket
import ssl
import subprocess
import sys
import tempfile
import time

from backup_integration import Fixture, rpc, stop

ACCOUNT = "backup-user"
PASSWORD = "remote-test-only-passphrase"
HOST = "127.0.0.2"


def wait_for(check, timeout=20):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = check()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError("timed out waiting for remote backup state")


class RemoteFixture(Fixture):
    def start_server(self):
        self.server = subprocess.Popen(
            [
                self.server_binary,
                "--data-dir",
                str(self.root / "repository"),
                "--port",
                str(getattr(self, "port", 0)),
                "--listen-address",
                "0.0.0.0",
                "--tls-cert",
                str(self.root / "cert.pem"),
                "--tls-key",
                str(self.root / "key.pem"),
                "--account-file",
                str(self.root / "account.json"),
                "--session-seconds",
                "8",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert select.select([self.server.stdout], [], [], 5)[
            0
        ], "remote listener did not respond"
        output = self.server.stdout.readline()
        assert output.startswith("LISTENING 0.0.0.0:"), (
            output,
            self.server.stderr.read(),
        )
        self.port = int(output.rsplit(":", 1)[1])

    def configure_target(self):
        return self.request(
            "save_target",
            {
                "id": "local",
                "name": "Remote target",
                "mode": "remote",
                "host": HOST,
                "port": self.port,
                "ca_certificate": str(self.root / "cert.pem"),
            },
        )

    def login(self, valid=True):
        return self.request(
            "login_remote",
            {
                "target_id": "local",
                "account": ACCOUNT,
                "password": PASSWORD if valid else "wrong password",
            },
            ok=valid,
        )

    def versions(self):
        return self.request("versions", {"target_id": "local"})["versions"]


def exercise(root, agent, server):
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-days",
            "1",
            "-keyout",
            str(root / "key.pem"),
            "-out",
            str(root / "cert.pem"),
            "-subj",
            f"/CN={HOST}",
            "-addext",
            f"subjectAltName=IP:{HOST}",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=True,
    )
    subprocess.run(
        [
            server,
            "--init-remote-account",
            ACCOUNT,
            "--account-file",
            str(root / "account.json"),
        ],
        input=PASSWORD + "\n",
        text=True,
        check=True,
    )
    assert PASSWORD.encode() not in (root / "account.json").read_bytes()
    assert (root / "account.json").stat().st_mode & 0o077 == 0
    fixture = RemoteFixture(root, agent, server)
    try:
        # 登录之前，TLS 请求也无法取得历史版本。
        context = ssl.create_default_context(cafile=str(root / "cert.pem"))
        with context.wrap_socket(
            socket.create_connection((HOST, fixture.port)), server_hostname=HOST
        ) as connection:
            assert (
                "AUTH_REQUIRED" in rpc(connection, "versions", {}, ok=False)["reason"]
            )
            assert "data_root" not in rpc(connection, "ping", {})
        assert (
            "AUTH_REQUIRED"
            in fixture.request("ping", {"target_id": "local"}, ok=False)["message"]
        )

        def add_target(id, host, with_certificate):
            fixture.request(
                "save_target",
                {
                    "id": id,
                    "name": id,
                    "mode": "remote",
                    "host": host,
                    "port": fixture.port,
                    "ca_certificate": (
                        str(root / "cert.pem") if with_certificate else ""
                    ),
                },
            )
            assert (
                "CERTIFICATE_FAILED"
                in fixture.request("ping", {"target_id": id}, ok=False)["message"]
            )

        add_target("wrong-host", "127.0.0.1", True)
        add_target("untrusted-ca", HOST, False)
        assert "Invalid remote account or password" in fixture.login(False)["message"]
        time.sleep(1.1)
        assert fixture.login()["expires_in"] == 8

        source = root / "source"
        source.mkdir()
        content = source / "proof.txt"
        content.write_text("original")
        task = fixture.request("add_task", {"path": str(source), "target_id": "local"})
        version = fixture.backup(task["id"])
        fixture.restore(version["id"], root / "restored")
        assert (root / "restored/proof.txt").read_text() == "original"
        fixture.request("set_task_realtime", {"id": task["id"], "enabled": True})

        def status():
            return fixture.request("config")["realtime_status"][task["id"]]["state"]

        wait_for(lambda: status() == "WATCHING")
        before = len(list((root / "repository/storage/versions").iterdir()))
        # 会话自然到期，即使文件未变，卡片也更新为待认证。
        wait_for(lambda: status() == "WAITING_AUTH")
        content.write_text("after session expiry")
        time.sleep(2)
        assert len(list((root / "repository/storage/versions").iterdir())) == before
        assert fixture.login()["expires_in"] == 8
        wait_for(lambda: status() == "WATCHING")
        latest = next(row for row in fixture.versions() if row["id"] != version["id"])
        fixture.restore(latest["id"], root / "after-login")
        assert (root / "after-login/proof.txt").read_text() == "after session expiry"

        # Server 重启会使尚未自然到期的旧令牌立即作废。
        stop(fixture.server)
        fixture.start_server()
        assert (
            "AUTH_REQUIRED"
            in fixture.request("ping", {"target_id": "local"}, ok=False)["message"]
        )
        assert status() == "WAITING_AUTH"
        assert fixture.login()["expires_in"] == 8
        wait_for(lambda: status() == "WATCHING")
        stop(fixture.agent)
        fixture.start_agent()
        assert status() == "WAITING_AUTH"
        assert (
            "AUTH_REQUIRED"
            in fixture.request("versions", {"target_id": "local"}, ok=False)["message"]
        )
        assert PASSWORD not in json.dumps(fixture.request("config"))
        for file in (root / "agent-state").rglob("*.json"):
            saved = file.read_bytes()
            assert (
                PASSWORD.encode() not in saved and b"session_token" not in saved
            ), file
        # 独立 TLS 对端保持同一 TCP 连接，检验 Server 也逐请求执行有效期检查。
        time.sleep(1.1)
        with context.wrap_socket(
            socket.create_connection((HOST, fixture.port)), server_hostname=HOST
        ) as connection:
            session = rpc(
                connection, "remote_login", {"account": ACCOUNT, "password": PASSWORD}
            )
            rpc(
                connection, "remote_resume", {"session_token": session["session_token"]}
            )
            assert rpc(connection, "versions", {})["versions"]
            time.sleep(session["expires_in"] + 0.2)
            assert (
                "AUTH_REQUIRED" in rpc(connection, "versions", {}, ok=False)["reason"]
            )
        print(
            "PASS: TLS host and CA verification, authentication, real upload/restore, expiry, relogin, server and Agent restart"
        )
    finally:
        fixture.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="backup-remote-") as temp:
        exercise(Path(temp), sys.argv[1], sys.argv[2])
