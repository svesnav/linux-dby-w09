#!/usr/bin/env python3
"""Package one DTBO into an Android DT table, without flashing it."""
import argparse
import struct
from pathlib import Path

MAGIC = 0xD7B7AB1E
HEADER_SIZE = 32
ENTRY_SIZE = 32


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("overlay", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    blob = args.overlay.read_bytes()
    if len(blob) < 40 or blob[:4] != b"\xd0\x0d\xfe\xed":
        raise SystemExit("Input is not a flattened device tree")
    dt_offset = HEADER_SIZE + ENTRY_SIZE
    header = struct.pack(">8I", MAGIC, dt_offset + len(blob), HEADER_SIZE,
                         ENTRY_SIZE, 1, HEADER_SIZE, 4096, 0)
    entry = struct.pack(">8I", len(blob), dt_offset, 0, 0, 0, 0, 0, 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + entry + blob)
    print(f"Packed {args.output}: one overlay, {len(blob)} DTB bytes")


if __name__ == "__main__":
    main()
