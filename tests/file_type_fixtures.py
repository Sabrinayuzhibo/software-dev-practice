"""Unsupported socket nodes for warning tests; use short bind names on Linux."""

import os
from pathlib import Path
import socket
import sys


def create_socket(path):
    # AF_UNIX limits the bind string, even when its containing path is valid.
    # Fixture setup is single-threaded; always restore cwd before returning.
    previous = os.open(".", os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.chdir(path.parent)
        with socket.socket(socket.AF_UNIX) as endpoint:
            endpoint.bind(path.name)
    finally:
        os.fchdir(previous)
        os.close(previous)


if __name__ == "__main__":
    for index in range(int(sys.argv[2])):
        create_socket(Path(sys.argv[1]) / f"skip-{index:04}.socket")
