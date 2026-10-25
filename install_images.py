#!/usr/bin/python3
"""Write boot/ramdisk/dtbo images from the running microSD Linux, verified.

Usage (root, on the tablet under 7.0):
    install_images.py PART=IMAGE:SHA256 [PART=IMAGE:SHA256 ...]
PART is boot, ramdisk or dtbo. For each partition the current contents are
saved to /root/partition-backups/<part>-<UTC time>.img first, then the image
is written, fsynced and read back; the SHA-256 must match both before the
write (the file) and after it (the partition). Does not reboot.
"""
import hashlib
import os
import pathlib
import subprocess
import sys
import time

ALLOWED = {"boot", "ramdisk", "dtbo"}
BACKUPS = pathlib.Path("/root/partition-backups")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main(args):
    if not args:
        sys.exit(__doc__)
    assert "dby-w09" in os.uname().release, "not the DBY-W09 7.0 kernel"
    root = subprocess.check_output(["findmnt", "-n", "-o", "SOURCE", "/"], text=True).strip()
    assert root.startswith("/dev/mmcblk"), f"root is {root}, not the microSD card"
    jobs = []
    for arg in args:
        part, _, rest = arg.partition("=")
        image_path, _, expected = rest.rpartition(":")
        assert part in ALLOWED, f"{part}: only {sorted(ALLOWED)}"
        image = pathlib.Path(image_path).read_bytes()
        assert sha256(image) == expected.lower(), f"{image_path}: SHA-256 mismatch"
        target = pathlib.Path(f"/dev/disk/by-partlabel/{part}").resolve()
        size = int(subprocess.check_output(["blockdev", "--getsize64", str(target)]))
        assert 0 < len(image) <= size, f"{image_path} ({len(image)}) larger than {part} ({size})"
        jobs.append((part, target, image, expected.lower(), size))

    BACKUPS.mkdir(mode=0o700, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    for part, target, image, expected, size in jobs:
        with target.open("rb") as device:
            old = device.read(size)
        backup = BACKUPS / f"{part}-{stamp}.img"
        backup.write_bytes(old)
        print(f"{part}: {target} backed up to {backup}")
        with target.open("r+b", buffering=0) as device:
            device.write(image)
            os.fsync(device.fileno())
            device.seek(0)
            assert sha256(device.read(len(image))) == expected, f"{part}: readback mismatch"
        print(f"{part}: written, readback SHA-256 verified")
    os.sync()


if __name__ == "__main__":
    main(sys.argv[1:])
