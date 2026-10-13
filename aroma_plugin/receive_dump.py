#!/usr/bin/env python3
"""Receive one TCP transfer from the Wii U plugin and save its bytes."""

import argparse
import socket
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="interface address to listen on (default: all interfaces)",
    )
    parser.add_argument("--port", type=int, default=4406)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("network-test.bin"),
        help="file to write received bytes to (default: network-test.bin)",
    )
    args = parser.parse_args()

    total_bytes = 0
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.port))
        listener.listen(1)
        print(f"Listening on {args.host}:{args.port}")
        connection, address = listener.accept()

        with connection, args.output.open("wb") as output:
            print(f"Connected by {address[0]}:{address[1]}")
            while True:
                chunk = connection.recv(64 * 1024)
                if not chunk:
                    break
                output.write(chunk)
                total_bytes += len(chunk)

    print(f"Saved {total_bytes} bytes to {args.output}")


if __name__ == "__main__":
    main()
