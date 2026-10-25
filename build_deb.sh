#!/usr/bin/env bash
set -euo pipefail

# Called by build_release.sh: packs a bundle directory as the Debian package
# linux-image-dby-w09, for updating a running Kubuntu with apt:
#   /usr/lib/modules/<release>/          the modules
#   /usr/lib/matepad/linux-boot/         kernel, DTB, DTBO, ramdisk payload,
#                                        command line (what the installer reads)
#   /usr/sbin/matepad-kernel-install     writes them to the boot partitions;
#                                        the postinst runs it on the tablet
#
#   build_deb.sh BUNDLE_DIR OUTPUT_DIR     (DEB_VERSION overrides the version)
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bundle="${1:?usage: build_deb.sh BUNDLE_DIR OUTPUT_DIR}"
out="${2:?usage: build_deb.sh BUNDLE_DIR OUTPUT_DIR}"
release=$(cat "$bundle/release.txt")
kernel_version=$(sed 's/^v//' "$repo_dir/KERNEL_VERSION")
version="${DEB_VERSION:-$kernel_version~local$(date -u +%Y%m%d%H%M)}"
stage="$(mktemp -d /tmp/dby-deb.XXXXXXXX)"
trap 'rm -rf -- "$stage"' EXIT

root="$stage/root"
mkdir -p "$root/DEBIAN" "$root/usr/lib/modules" "$root/usr/lib/matepad/linux-boot" "$root/usr/sbin"
tar -C "$root/usr/lib/modules" -xzf "$bundle/linux-modules.tar.gz"
test -f "$root/usr/lib/modules/$release/modules.dep"
files=(Image.gz sm8250-huawei-dby-w09.dtb dtbo.img ramdisk-payload.img cmdline.txt release.txt)
for f in "${files[@]}"; do install -m 0644 "$bundle/$f" "$root/usr/lib/matepad/linux-boot/$f"; done
(cd "$root/usr/lib/matepad/linux-boot" && sha256sum "${files[@]}" > SHA256SUMS)
install -m 0755 "$repo_dir/deb/matepad-kernel-install" "$root/usr/sbin/matepad-kernel-install"

cat > "$root/DEBIAN/control" <<EOF
Package: linux-image-dby-w09
Version: $version
Architecture: arm64
Maintainer: linux-dby-w09 <svesnav@users.noreply.github.com>
Section: kernel
Priority: optional
Depends: python3, util-linux, kmod
Installed-Size: $(du -sk "$root/usr" | cut -f1)
Homepage: https://github.com/svesnav/linux-dby-w09
Description: Linux $release for the Huawei MatePad 11 (DBY-W09)
 Mainline Linux with the MatePad 11 device tree and drivers: kernel, modules,
 the microSD-root initramfs and the bootloader's DTBO entry. On the tablet the
 package writes them to the boot, ramdisk and dtbo partitions, built from the
 stock HarmonyOS images the installer saved (matepad-kernel-install); the new
 kernel starts with the next reboot.
EOF
cat > "$root/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
	/usr/sbin/matepad-kernel-install --auto
fi
EOF
chmod 0755 "$root/DEBIAN/postinst"

deb="$out/linux-image-dby-w09_${version}_arm64.deb"
dpkg-deb --root-owner-group -Zxz --build "$root" "$deb" >/dev/null
dpkg-deb --info "$deb" | sed -n '/Package:/,/Description:/p'
ls -l "$deb"
