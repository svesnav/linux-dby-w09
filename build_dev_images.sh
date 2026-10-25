#!/usr/bin/env bash
set -euo pipefail

# After configure-and-build.sh: packs boot, ramdisk and dtbo partition images
# for one tablet into build/linux-70-*, from that tablet's stock partition
# dumps (work area backups/) and Bluetooth address (device/bt-mac). For
# development; install_images.py writes them from a running Linux, the
# installer app does the same job for everyone else. Never contacts the tablet.
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Work area outside the repo: build/ (outputs), tools/aosp-mkbootimg, backups/.
project_dir="${MATEPAD_WORK_DIR:-$(cd "$repo_dir/.." && pwd)}"
kbuild="${OUTPUT_DIR:-$HOME/matepad-linux-7.0-build}"
out="$project_dir/build"
tools="$project_dir/tools/aosp-mkbootimg"
stage="$(mktemp -d /tmp/dby-linux70.XXXXXXXX)"
trap 'rm -rf -- "$stage"' EXIT

cmdline="$(cat "$repo_dir/boot-cmdline.txt")"

# Initramfs: static busybox, sd_root_init.sh and the reboot helper.
root="$stage/root"
mkdir -p "$root/bin" "$root/dev" "$root/proc" "$root/sys" "$root/cache"
install -m 0755 "$project_dir/build/ubuntu-busybox-aarch64" "$root/bin/busybox"
install -m 0755 "$repo_dir/sd_root_init.sh" "$root/init"
aarch64-linux-gnu-gcc -static -no-pie -Os -s -Wall -Wextra \
    -o "$root/reboot-bootloader" "$repo_dir/reboot_bootloader.c"
(cd "$root" && find . -print0 | cpio --null -o -H newc --owner=0:0 --quiet) \
    | gzip -n -9 > "$out/linux-70-initramfs.gz"

# Ramdisk partition: header v0 image with an empty kernel, laid over the
# stock 2 MiB partition so its AVB footer stays in place.
: > "$stage/empty-kernel"
python3 "$tools/mkbootimg-android10.py" \
    --kernel "$stage/empty-kernel" --ramdisk "$out/linux-70-initramfs.gz" \
    --base 0x10000000 --kernel_offset 0x8000 --ramdisk_offset 0x2000000 \
    --tags_offset 0x100 --pagesize 2048 --os_version 12.0.0 \
    --os_patch_level 2022-08 --header_version 0 --output "$stage/ramdisk-payload.img"
python3 - "$project_dir/backups/ramdisk.img" "$stage/ramdisk-payload.img" \
    "$out/linux-70-ramdisk.img" <<'PY'
import struct, sys
from pathlib import Path
stock = bytearray(Path(sys.argv[1]).read_bytes())
payload = Path(sys.argv[2]).read_bytes()
if len(stock) != 2 * 1024 * 1024 or stock[-64:-60] != b"AVBf":
    raise SystemExit("Unexpected stock ramdisk partition layout")
size = struct.unpack_from(">Q", stock, len(stock) - 52)[0]
if not 0 < len(payload) <= size:
    raise SystemExit(f"Ramdisk payload {len(payload)} exceeds {size}")
stock[:len(payload)] = payload
stock[len(payload):size] = bytes(size - len(payload))
Path(sys.argv[3]).write_bytes(stock)
PY

# Boot partition: stock header v2 layout with the 7.0 kernel and DTB, the
# DTB with this tablet's Bluetooth address (work area device/bt-mac).
dtb="$stage/sm8250-huawei-dby-w09.dtb"
cp "$kbuild/arch/arm64/boot/dts/qcom/sm8250-huawei-dby-w09.dtb" "$dtb"
if [ -s "$project_dir/device/bt-mac" ]; then
    python3 "$repo_dir/set_bd_address.py" "$dtb" "$(cat "$project_dir/device/bt-mac")"
else
    echo "warning: no $project_dir/device/bt-mac, Bluetooth stays unconfigured" >&2
fi
python3 "$repo_dir/repack_boot.py" \
    --kernel "$kbuild/arch/arm64/boot/Image.gz" \
    --dtb "$dtb" \
    --cmdline "$cmdline" --output "$out/linux-70-boot.img"

# DTBO partition: one overlay for board 0x206a that fills /rootparam.
python3 "$repo_dir/pack_dtbo.py" \
    "$kbuild/dby-w09-board.dtbo" "$out/linux-70-dtbo.img"

cp "$kbuild/.config" "$out/linux-70.config"
cp "$kbuild/arch/arm64/boot/Image.gz" "$out/linux-70-Image.gz"
cp "$dtb" "$out/linux-70-dby-w09.dtb"
cd "$out"
ls -l linux-70-*.img
sha256sum linux-70-boot.img linux-70-dtbo.img linux-70-ramdisk.img | tee linux-70-images.sha256
