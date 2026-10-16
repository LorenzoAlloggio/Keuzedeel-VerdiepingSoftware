import argparse
import hashlib
import socket
import struct
import sys
import zlib
from pathlib import Path

MAX_CHUNK = 16 * 1024 * 1024

EXPECTED_SIZES = {
    "otp.bin": 0x400, 
    "slc.bin": 0x40000 * 0x800, 
}

SECRET_FILES = {"otp.bin", "seeprom.bin"}


def recv_exact(connection, size):
    """recv() may return fewer bytes, so keep reading until we have `size`."""
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise ConnectionError(
                f"connection closed after {len(data)} of {size} bytes")
        data += part
    return bytes(data)


def start_file(out_dir, name_raw, size, chunk_size):
    name = Path(name_raw.split(bytes(1))[0].decode("ascii", "replace")).name
    if not name:
        raise ValueError("FILE header without a name: out of sync")
    path = out_dir / name
    print(f"{name}: receiving {size} bytes (chunks up to {chunk_size} bytes)")
    return {
        "name": name, "size": size, "received": 0, "bad_chunks": 0,
        "crc": 0, "sha": hashlib.sha256(), "path": path,
        "file": path.open("wb"),
    }


def finish_file(current, file_crc, read_errors, unverified):
    """Called at FEND. Returns True when the file is complete and correct."""
    current["file"].close()
    name = current["name"]
    problems = []

    if current["received"] != current["size"]:
        problems.append(f"got {current['received']} of {current['size']} bytes")
    expected = EXPECTED_SIZES.get(name)
    if expected is not None and current["size"] != expected:
        problems.append(f"size is {current['size']}, expected {expected}")
    if current["bad_chunks"]:
        problems.append(f"{current['bad_chunks']} chunks failed the CRC32 check")
    if current["crc"] != file_crc:
        problems.append(f"file CRC32 differs (console {file_crc:08x}, "
                        f"PC {current['crc']:08x})")

    if name in SECRET_FILES:
        current["path"].chmod(0o600)
        data = current["path"].read_bytes()
        if data and data.count(0) == len(data):
            problems.append("all bytes are 00, the console sent an empty buffer")
        if data and data.count(0xFF) == len(data):
            problems.append("all bytes are FF, the console sent an empty buffer")

    ok = not problems
    print(f"{name}: {'OK' if ok else 'NOT OK'} "
          f"({current['received']} bytes, CRC32 {current['crc']:08x})")
    for problem in problems:
        print(f"  problem: {problem}")
    if read_errors or unverified:
        print(f"  console reported {read_errors} read errors, "
              f"{unverified} unverified sectors")
    print(f"  SHA-256 {current['sha'].hexdigest()}")
    if name in SECRET_FILES:
        print(f"  {name} holds console-unique keys: never commit or share it")
    return ok


def receive_dumps(connection, out_dir):
    """Read blocks until DONE. Returns a dict: file name -> True (OK) / False."""
    current = None
    results = {}
    while True:
        magic = recv_exact(connection, 4)

        if magic == b"FILE":
            if current is not None:
                raise ValueError(f"FILE before FEND of {current['name']}: out of sync")
            name_raw, size, chunk_size = struct.unpack(">16sQI", recv_exact(connection, 28))
            current = start_file(out_dir, name_raw, size, chunk_size)

        elif magic == b"CHNK":
            offset, length, crc = struct.unpack(">QII", recv_exact(connection, 16))
            if current is None or length > MAX_CHUNK:
                raise ValueError("CHNK without FILE, or silly length: out of sync")
            if offset != current["received"]:
                raise ValueError(f"expected offset {current['received']:#x}, got {offset:#x}")
            data = recv_exact(connection, length)
            if zlib.crc32(data) != crc:
                current["bad_chunks"] += 1
                print(f"  CRC32 mismatch in chunk at offset {offset:#x}")
            current["file"].write(data)
            current["crc"] = zlib.crc32(data, current["crc"])
            current["sha"].update(data)
            before = current["received"]
            current["received"] += length
            if before >> 26 != current["received"] >> 26: 
                print(f"  {current['received'] >> 20} / {current['size'] >> 20} MiB")

        elif magic == b"FEND":
            file_crc, read_errors, unverified = struct.unpack(">III", recv_exact(connection, 12))
            if current is None:
                raise ValueError("FEND without FILE: out of sync")
            results[current["name"]] = finish_file(current, file_crc, read_errors, unverified)
            current = None

        elif magic == b"DONE":
            if current is not None:
                raise ValueError(f"DONE before FEND of {current['name']}: out of sync")
            return results

        else:
            raise ValueError(f"unknown block {magic!r}: out of sync "
                             "(is the plugin an older build without the protocol?)")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="0.0.0.0",
                        help="interface address to listen on (default: all interfaces)")
    parser.add_argument("--port", type=int, default=4406)
    parser.add_argument("--output-dir", type=Path, default=Path("dumps"),
                        help="folder for the received files (default: dumps)")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.port))
        listener.listen(1)
        print(f"Listening on {args.host}:{args.port}, saving to {args.output_dir}/")
        connection, address = listener.accept()
        with connection:
            print(f"Connected by {address[0]}:{address[1]}")
            try:
                results = receive_dumps(connection, args.output_dir)
            except (ConnectionError, ValueError) as error:
                print(f"INCOMPLETE: {error}")
                print("The dump did not finish (no DONE). Check the UDP log on port 4405.")
                return 1

    print("DONE received.")
    if not results:
        print("No files were sent. Check the UDP log on port 4405 for the reason.")
        return 1
    for name, ok in results.items():
        print(f"  {name}: {'OK' if ok else 'NOT OK'}")
    if all(results.values()):
        print("All files OK.")
        return 0
    print("Some files are NOT OK, see the problems above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
