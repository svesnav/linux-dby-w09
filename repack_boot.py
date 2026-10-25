#!/usr/bin/env python3
"""Repack the DBY-W09 boot payload with a replacement Image.gz.

Run under WSL Python. This prepares an image only; it never contacts the tablet.
"""

import argparse
import hashlib
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


# Work area (backups/, tools/) is the folder that holds this repo; MATEPAD_WORK_DIR overrides it.
ROOT = Path(os.environ.get("MATEPAD_WORK_DIR") or Path(__file__).resolve().parent.parent)
STOCK_BOOT = ROOT / "backups/boot.img"
TOOLS = ROOT / "tools/aosp-mkbootimg"
EXPECTED_STOCK_SHA256 = "6fb82f6ca823c8acffaece13b161eca6c685a6e26f2c216b50293c3fd5da539d"
STOCK_PAYLOAD_SIZE = 24412160


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, help="Replacement compressed ARM64 kernel")
    parser.add_argument("--dtb", type=Path, help="Replacement ARM64 device tree blob")
    parser.add_argument("--cmdline", help="Replacement kernel command line")
    parser.add_argument("--full-partition", action="store_true",
                        help="Preserve the stock 96 MiB partition layout and AVB footer")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if sha256(STOCK_BOOT) != EXPECTED_STOCK_SHA256:
        raise SystemExit("Stock boot backup hash mismatch")
    if STOCK_BOOT.stat().st_size != 96 * 1024 * 1024:
        raise SystemExit("Unexpected stock boot partition size")
    if args.kernel and not args.kernel.is_file():
        raise SystemExit(f"Kernel missing: {args.kernel}")
    if args.dtb and not args.dtb.is_file():
        raise SystemExit(f"Device tree missing: {args.dtb}")
    if args.full_partition and not args.kernel:
        raise SystemExit("--full-partition requires --kernel")

    with tempfile.TemporaryDirectory(prefix="dby-boot-") as temp:
        extracted = Path(temp) / "unpacked"
        command = subprocess.check_output(
            [sys.executable, str(TOOLS / "unpack_bootimg.py"),
             "--boot_img", str(STOCK_BOOT), "--out", str(extracted),
             "--format=mkbootimg"], text=True
        ).strip()
        flags = shlex.split(command)
        if flags[:2] != ["--header_version", "2"]:
            raise SystemExit(f"Unexpected boot image header: {flags[:2]}")
        if args.kernel:
            flags[flags.index("--kernel") + 1] = str(args.kernel.resolve())
        if args.dtb:
            flags[flags.index("--dtb") + 1] = str(args.dtb.resolve())
        if args.cmdline is not None:
            flags[flags.index("--cmdline") + 1] = args.cmdline
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [sys.executable, str(TOOLS / "mkbootimg-android10.py"), *flags,
             "--output", str(output)], check=True
        )

    if args.kernel is None:
        with STOCK_BOOT.open("rb") as source, output.open("rb") as rebuilt:
            if rebuilt.read() != source.read(STOCK_PAYLOAD_SIZE):
                raise SystemExit("Rebuilt stock boot payload differs from backup")
        print("Stock boot payload reproduced byte for byte")
    if args.full_partition:
        payload = output.read_bytes()
        if len(payload) > STOCK_PAYLOAD_SIZE:
            raise SystemExit("Replacement boot payload exceeds stock payload size")
        partition = bytearray(STOCK_BOOT.read_bytes())
        partition[:len(payload)] = payload
        partition[len(payload):STOCK_PAYLOAD_SIZE] = bytes(STOCK_PAYLOAD_SIZE - len(payload))
        output.write_bytes(partition)
    print(f"Prepared {output} ({output.stat().st_size} bytes; SHA-256 {sha256(output)})")


if __name__ == "__main__":
    main()
