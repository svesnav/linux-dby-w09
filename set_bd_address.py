#!/usr/bin/env python3
"""Write a Bluetooth address into the DBY-W09 DTB (local-bd-address).

The DTS carries 00:00:00:00:00:00 so that no build holds one tablet's
address; the installer and build_dev_images.sh fill in the tablet's own
(HarmonyOS reports it; Linux keeps it in /etc/matepad/bt-mac).

    set_bd_address.py DTB AA:BB:CC:DD:EE:FF [--output OUT]
"""
import argparse
import re
import struct
from pathlib import Path

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def bd_address_offsets(dtb: bytes) -> list[int]:
    """Offsets of the 6-byte value of every local-bd-address property."""
    magic, _, off_struct, off_strings, _, _, _, _, size_strings, size_struct = \
        struct.unpack_from(">10I", dtb)
    if magic != FDT_MAGIC:
        raise SystemExit("not a flattened device tree")
    strings = dtb[off_strings:off_strings + size_strings]
    found, pos, end = [], off_struct, off_struct + size_struct
    while pos < end:
        (token,) = struct.unpack_from(">I", dtb, pos)
        pos += 4
        if token == FDT_BEGIN_NODE:
            pos = (dtb.index(b"\0", pos) + 4) & ~3
        elif token == FDT_PROP:
            length, name_off = struct.unpack_from(">II", dtb, pos)
            pos += 8
            name = strings[name_off:strings.index(b"\0", name_off)]
            if name == b"local-bd-address" and length == 6:
                found.append(pos)
            pos = (pos + length + 3) & ~3
        elif token in (FDT_END_NODE, FDT_NOP):
            continue
        elif token == FDT_END:
            break
        else:
            raise SystemExit(f"bad FDT token {token:#x} at {pos - 4:#x}")
    return found


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dtb", type=Path)
    parser.add_argument("address")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", args.address):
        raise SystemExit(f"not a Bluetooth address: {args.address}")
    value = bytes(reversed(bytes.fromhex(args.address.replace(":", ""))))
    dtb = bytearray(args.dtb.read_bytes())
    offsets = bd_address_offsets(dtb)
    if len(offsets) != 1:
        raise SystemExit(f"expected one local-bd-address, found {len(offsets)}")
    dtb[offsets[0]:offsets[0] + 6] = value
    (args.output or args.dtb).write_bytes(dtb)
    print(f"local-bd-address = {args.address.upper()}")


if __name__ == "__main__":
    main()
