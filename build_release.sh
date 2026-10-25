#!/usr/bin/env bash
set -euo pipefail

# After configure-and-build.sh and build_modules.sh: packs the blob-free
# boot bundle that CI publishes and the installer turns into partitions.
#
#   build/release/linux-boot-<release>.tar
#     Image.gz, sm8250-huawei-dby-w09.dtb   kernel; DTB Bluetooth address 00:00:...
#     dtbo.img                              DTBO partition (/rootparam overlay)
#     ramdisk-payload.img                   header v0 image, SD-root initramfs
#     cmdline.txt                           kernel command line
#     linux-modules.tar.gz                  /usr/lib/modules/<release>
#     linux-image-dby-w09_<version>_arm64.deb  the same as a Debian package (build_deb.sh)
#     SHA256SUMS
#
# No Huawei file goes in: on the tablet the installer builds the boot
# partition from the stock one (its header v2 layout and ramdisk, this kernel,
# DTB and command line), lays ramdisk-payload.img over the stock ramdisk
# partition (keeping its AVB footer, as build_dev_images.sh does) and writes
# the tablet's own Bluetooth address into the DTB (set_bd_address.py).
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="${MATEPAD_WORK_DIR:-$(cd "$repo_dir/.." && pwd)}"
kbuild="${OUTPUT_DIR:-$HOME/matepad-linux-7.0-build}"
busybox="${BUSYBOX:-$project_dir/build/ubuntu-busybox-aarch64}"
modules="$project_dir/build/linux-70-modules.tar.gz"
release=$(cat "$kbuild/include/config/kernel.release")
out="$project_dir/build/release"
dir="$out/linux-boot-$release"
stage="$(mktemp -d /tmp/dby-release.XXXXXXXX)"
trap 'rm -rf -- "$stage"' EXIT

test -x "$busybox" || { echo "no aarch64 busybox at $busybox (fetch_busybox.sh)" >&2; exit 1; }
tar -tzf "$modules" "$release/modules.dep" >/dev/null
rm -rf "$dir" && mkdir -p "$dir"

# SD-root initramfs: static busybox, sd_root_init.sh, the reboot helper.
root="$stage/root"
mkdir -p "$root/bin" "$root/dev" "$root/proc" "$root/sys" "$root/cache"
install -m 0755 "$busybox" "$root/bin/busybox"
install -m 0755 "$repo_dir/sd_root_init.sh" "$root/init"
aarch64-linux-gnu-gcc -static -no-pie -Os -s -Wall -Wextra \
    -o "$root/reboot-bootloader" "$repo_dir/reboot_bootloader.c"
(cd "$root" && find . -print0 | LC_ALL=C sort -z | cpio --null -o -H newc --owner=0:0 --quiet) \
    | gzip -n -9 > "$stage/initramfs.gz"
: > "$stage/empty-kernel"
python3 "$repo_dir/tools/mkbootimg.py" \
    --kernel "$stage/empty-kernel" --ramdisk "$stage/initramfs.gz" \
    --base 0x10000000 --kernel_offset 0x8000 --ramdisk_offset 0x2000000 \
    --tags_offset 0x100 --pagesize 2048 --os_version 12.0.0 \
    --os_patch_level 2022-08 --header_version 0 --output "$dir/ramdisk-payload.img"

cp "$kbuild/arch/arm64/boot/Image.gz" "$dir/Image.gz"
cp "$kbuild/arch/arm64/boot/dts/qcom/sm8250-huawei-dby-w09.dtb" "$dir/"
python3 "$repo_dir/pack_dtbo.py" "$kbuild/dby-w09-board.dtbo" "$dir/dtbo.img"
cp "$repo_dir/boot-cmdline.txt" "$dir/cmdline.txt"
cp "$modules" "$dir/linux-modules.tar.gz"
echo "$release" > "$dir/release.txt"
rm -f "$out"/linux-image-dby-w09_*.deb
bash "$repo_dir/build_deb.sh" "$dir" "$out"
cp "$out"/linux-image-dby-w09_*.deb "$dir/"    # the image build installs it
(cd "$dir" && sha256sum Image.gz sm8250-huawei-dby-w09.dtb dtbo.img ramdisk-payload.img \
    cmdline.txt linux-modules.tar.gz release.txt linux-image-dby-w09_*.deb > SHA256SUMS)
tar -C "$out" -cf "$out/linux-boot-$release.tar" "linux-boot-$release"
ls -l "$out/linux-boot-$release.tar"
cat "$dir/SHA256SUMS"
