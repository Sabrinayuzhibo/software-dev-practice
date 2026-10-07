#!/usr/bin/env python3
"""Exercise server TCP framing and the real C++ Agent command bridge."""

import hashlib
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import sys
import tempfile
import time


def receive_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise AssertionError("server closed before the complete response")
        result.extend(chunk)
    return bytes(result)


def agent_request(process: subprocess.Popen, action: str, payload: dict, request_id: str) -> dict:
    process.stdin.write(json.dumps({"id": request_id, "action": action, "payload": payload}) + "\n")
    process.stdin.flush()
    ready, _, _ = select.select([process.stdout], [], [], 5)
    assert ready, f"agent timed out: {action}"
    line = process.stdout.readline()
    assert line, f"agent exited: {process.poll()}"
    reply = json.loads(line)
    assert reply["id"] == request_id, reply
    return reply


def main(agent: str, server: str) -> None:
    with tempfile.TemporaryDirectory(prefix="backup-system-test-") as data_root:
        with tempfile.TemporaryDirectory(prefix="backup-source-test-") as source_root:
            source = Path(source_root)
            (source / "sub").mkdir()
            contents = b"course backup test\n"
            (source / "sub" / "example.txt").write_bytes(contents)
            environment = os.environ.copy()
            server_process = subprocess.Popen(
                [server, "--data-dir", data_root, "--port", "0"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            agent_process = subprocess.Popen(
                [agent, "--state-dir", str(Path(data_root) / "agent-state")],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=environment,
            )
            try:
                line = server_process.stdout.readline().strip()
                match = re.fullmatch(r"LISTENING 127\.0\.0\.1:(\d+)", line)
                assert match, f"server did not start: {line!r}"
                port = int(match.group(1))

                root = Path(data_root)
                assert (root / "database").is_dir()
                assert (root / "storage").is_dir()
                assert not (root / "database" / "metadata.db").exists()
                assert not any((root / "storage" / "versions").iterdir())

                request = {"type": "ping", "request_id": "integration-1", "payload": {}}
                body = json.dumps(request).encode("utf-8")
                frame = len(body).to_bytes(4, "big") + body
                with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
                    connection.sendall(frame[:2])
                    time.sleep(0.02)
                    connection.sendall(frame[2:7])
                    time.sleep(0.02)
                    connection.sendall(frame[7:])
                    response_size = int.from_bytes(receive_exact(connection, 4), "big")
                    response = json.loads(receive_exact(connection, response_size))
                    assert response["type"] == "pong"
                    assert response["request_id"] == "integration-1"
                    assert response["payload"]["server"] == "Backup Server"

                ping = agent_request(agent_process, "ping", {"port": port}, "agent-ping")
                assert ping["ok"] and ping["result"]["server"] == "Backup Server", ping

                scan = agent_request(agent_process, "scan", {"path": source_root}, "agent-scan")
                assert scan["ok"], scan
                result = scan["result"]
                assert result["files"] == 1 and result["directories"] == 1, result
                assert result["bytes"] == len(contents) and result["hashed_files"] == 1, result
                preview = {item["path"]: item for item in result["sample"]}
                assert preview["sub"]["type"] == "directory"
                assert preview["sub/example.txt"]["sha256"] == hashlib.sha256(contents).hexdigest()
                assert preview["sub/example.txt"]["type"] == "file"
                assert not any((root / "storage" / "versions").iterdir())

                occupied = subprocess.run(
                    [server, "--data-dir", str(root / "occupied"), "--port", str(port)],
                    capture_output=True, text=True, timeout=5,
                )
                assert occupied.returncode == 3, occupied.stderr
                assert "LISTEN_ERROR" in occupied.stderr

                server_process.terminate()
                server_process.wait(timeout=3)
                stopped = agent_request(agent_process, "ping", {"port": port}, "agent-stopped")
                assert not stopped["ok"] and stopped["error"]["message"], stopped
            finally:
                if server_process.poll() is None:
                    server_process.terminate()
                    server_process.wait(timeout=3)
                agent_process.stdin.close()
                try:
                    agent_process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    agent_process.kill()
                    agent_process.wait(timeout=3)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
