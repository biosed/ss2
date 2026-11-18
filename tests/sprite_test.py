"""Construct tiny FZFS/PCK files in memory and exercise the native CLI."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def words(*values):
    return struct.pack("<" + "H" * len(values), *values)


def descriptor(data, count, width):
    packed = zlib.compress(data)
    return struct.pack("<3I", len(packed), count, width) + packed


def archive(sprite, storage=8, declared_size=None):
    chunk = 7
    offsets = []
    data = bytearray()
    if storage == 8:
        for start in range(0, len(sprite), chunk):
            offsets.append(8 + len(data))
            data.extend(zlib.compress(sprite[start:start + chunk]))
        offsets.append(8 + len(data))
    else:
        data.extend(sprite)
    record = struct.pack("<5I", 0, storage, 0 if storage == 8 else 8,
                         len(sprite) if declared_size is None else declared_size, chunk)
    names = b"fixture.pck\0"
    index = (descriptor(record, 1, 20) + descriptor(names, 1, len(names)) +
             descriptor(struct.pack("<" + "I" * len(offsets), *offsets), len(offsets), 4))
    return b"FZFS" + struct.pack("<I", len(data)) + data + index


def frame(run, width=3, tag=0xa5):
    return struct.pack("<4hBH", -2, 4, width, 1, tag, len(run)) + run


def pck(*frames):
    block = bytearray()
    offsets = []
    for image in frames:
        offsets.append(len(frames) * 4 + len(block))
        block.extend(image)
    return struct.pack("<" + "I" * len(frames), *offsets) + block


def main():
    first = frame(b"\x82" + words(0x1234) + b"\x01" + words(0xabcd))
    second = frame(b"\x03" + words(1, 2, 3))
    sprite = pck(first, second)
    with tempfile.TemporaryDirectory() as root:
        path = Path(root) / "fixture.sue"

        def run(data, index=0, name="fixture.pck", error=None):
            path.write_bytes(data)
            result = subprocess.run([sys.argv[1], os.fspath(path), name, str(index)],
                                    capture_output=True, timeout=5)
            if error is not None:
                if result.returncode != 1 or error.encode() not in result.stderr or result.stdout:
                    raise RuntimeError((result.returncode, result.stderr, error))
            elif result.returncode:
                raise RuntimeError(result.stderr)
            return result

        for kind in (2, 8):
            first_result = run(archive(sprite, kind))
            assert first_result.stdout == words(0x1234, 0x1234, 0xabcd)
            assert b"origin=-2,4 size=3x1" in first_result.stderr
            assert run(archive(sprite, kind), index=1).stdout == words(1, 2, 3)
        run(archive(sprite), index=2, error="frame table or index")
        run(archive(sprite), name="absent", error="entry not found")
        run(archive(sprite, storage=1), error="storage type")
        run(archive(sprite, declared_size=0xffffffff), error="reader limit")
        run(archive(sprite)[:-2], error="Truncated")
        run(archive(pck(frame(b"\x84" + words(0)))), error="run width")
        run(archive(pck(frame(b"\x01" + words(1)))), error="Incomplete PCK row")
        run(archive(pck(frame(b"\x03" + words(1)))), error="Truncated")
        run(archive(pck(frame(b"", tag=0xa2))), error="frame tag")
        damaged = bytearray(archive(sprite))
        damaged[8] ^= 1
        run(damaged, error="zlib stream")


if __name__ == "__main__":
    main()
