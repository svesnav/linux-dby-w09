# Linux for the Huawei MatePad 11 (DBY-W09)

Mainline Linux 7.0 for the Huawei MatePad 11 (2021, DBY-W09, Qualcomm
SM8250 / Snapdragon 865). This repository holds the kernel side: the board
device tree, out-of-tree drivers, patch scripts for upstream drivers, and the
tools that build the boot images.

The matching Kubuntu system (image builder and userspace services) lives in
[kubuntu-dby-w09](https://github.com/svesnav/kubuntu-dby-w09). The HarmonyOS
app that installs both lives in
[dby-w09-installer](https://github.com/svesnav/dby-w09-installer).

## Hardware status

| Area | Status | Notes |
| --- | --- | --- |
| Display (2560×1600, 120 Hz) | works | DSI panel driver `panel/`, RT8555 backlight |
| GPU (Adreno 650) | works | freedreno; Huawei-signed zap shader from the tablet |
| Touchscreen | works | `touch/huawei-thp-lite.c` + Huawei's `aptouch_daemon` from the tablet |
| M-Pencil | works | position via the touch daemon, pressure over BLE |
| Wi-Fi, Bluetooth (QCA6390) | work | per-tablet MAC and Bluetooth address |
| Speakers (4× TFA9874) | work | primary TDM back end, q6afe/q6routing patches |
| Microphones (WCD938x) | work | SoundWire, MultiMedia2 capture |
| Sensors, auto-rotation | work | SLPI over QRTR, served by hexagonrpcd |
| Volume/power keys, cover | work | |
| USB-C host, DP Alt Mode, HDMI audio | work | `usb/README.md` |
| Battery, charging LED | work | `power/matepad-fg.c`, `power/README.md` |
| Flashlight (rear flash LED) | works | PM8150L flash block, `white:flash` (torch mode) |
| Suspend | works | Sleep button suspends; cover/power key blank the screen |
| Cameras | not supported | rear OV13B10 and front Hi-846 have mainline drivers; CAMSS wiring not done |
| RTC | not supported | time comes from NTP |

## Boot flow

Huawei's ABL boots a header v2 `boot` image and loads its ramdisk from the
separate `ramdisk` partition. Two bootloader rules shape the images:

- The DTBO entry ABL applies must fill **`/rootparam`** the way the stock
  board overlay does (`dby-w09-rootparam.dtso`); any other DTBO sends the
  tablet straight back to fastboot.
- `fastboot boot` (RAM boot) is refused, so tests flash the partitions.

The kernel is built with everything the early boot needs, and the
initramfs (`sd_root_init.sh`, static busybox) mounts the ext4 root labelled
`LINUX70ROOT` on the microSD card. HarmonyOS on the internal storage is never
touched. Without a card, or with `/var/lib/linux70/fastboot-next-boot` on it,
the initramfs reboots to fastboot.

## Blob-free builds

Nothing from Huawei is stored here or in a release:

- The device tree carries `local-bd-address = 00:00:00:00:00:00`;
  `set_bd_address.py` writes each tablet's own address into the DTB.
- The release bundle has no boot partition image. The installer builds it on
  the tablet from the stock `boot` partition (its header layout and ramdisk,
  plus this kernel, DTB and command line), lays the ramdisk payload over the
  stock `ramdisk` partition (keeping its AVB footer), and copies firmware and
  vendor files from the tablet itself.

## Building

Requirements: Ubuntu 24.04 (or WSL), `gcc-aarch64-linux-gnu libc6-dev-arm64-cross
bc bison flex libssl-dev device-tree-compiler cpio kmod python3` (`ccache`
optional: set `CCACHE_DIR` to use it).

```bash
git clone --depth 1 --branch "$(cat KERNEL_VERSION)" \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git ~/linux
export SOURCE_DIR=~/linux OUTPUT_DIR=~/linux-build MATEPAD_WORK_DIR=~/matepad-work
mkdir -p "$MATEPAD_WORK_DIR/build"
./configure-and-build.sh            # patches the tree, builds Image.gz, DTB, DTBO
./build_modules.sh                  # build/linux-70-modules.tar.gz
./fetch_busybox.sh "$MATEPAD_WORK_DIR/build/ubuntu-busybox-aarch64"
./build_release.sh                  # build/release/linux-boot-<release>.tar
```

`MATEPAD_WORK_DIR` is the work area for outputs (`build/`), and for the local
flashing tools also `backups/` (stock partition dumps), `tools/aosp-mkbootimg`
and `device/bt-mac`. It defaults to the folder that contains this repository.

The configuration is arm64 `defconfig` with Qualcomm as the only platform:
the other SoC families, PCIe graphics (nouveau, radeon, amdgpu), wired
Ethernet, SATA/SAS/NVMe, CAN, MTD, KVM/Xen and Wi-Fi chips other than
ath11k are off (about 800 modules instead of 1300). exFAT, NTFS3 and USB
Attached SCSI are added for drives on USB-C. Everything the early boot
needs is built in; `configure-and-build.sh` fails if any of it is missing.

### Release bundle

`linux-boot-<release>.tar`:

| File | Use |
| --- | --- |
| `Image.gz`, `sm8250-huawei-dby-w09.dtb` | kernel and device tree for the `boot` partition |
| `dtbo.img` | `dtbo` partition |
| `ramdisk-payload.img` | header v0 image with the SD-root initramfs, laid over the `ramdisk` partition |
| `cmdline.txt` | kernel command line |
| `linux-modules.tar.gz` | `/usr/lib/modules/<release>` |
| `linux-image-dby-w09_<version>_arm64.deb` | the same as a Debian package (also a separate release asset) |
| `SHA256SUMS`, `release.txt` | checksums, kernel release string |

## Updating a running Kubuntu

Every release also carries `linux-image-dby-w09_<version>_arm64.deb`:

```bash
sudo apt install ./linux-image-dby-w09_*_arm64.deb
sudo reboot
```

The package installs the modules and the boot files, then
`matepad-kernel-install` builds the `boot`, `ramdisk` and `dtbo` partitions
from the stock HarmonyOS images the installer saved
(`/usr/local/share/matepad/harmonyos-images`) and this tablet's Bluetooth
address, keeps the previous partitions in `/var/lib/matepad/boot-backup`,
writes each one and reads it back. `matepad-kernel-install --check` shows
what would change. In a chroot, a container or on another device the
package only installs its files.

## CI

`.github/workflows/build.yml` builds the kernel, modules and bundle on every
push and pull request (artifact `linux-boot`). A `v*` tag publishes the bundle
as a GitHub release, which the image build in `kubuntu-dby-w09` consumes.
With the repository variable `RUNNER` set, pushes build on a self-hosted
runner (kubuntu-dby-w09 `ci/runner/`, with ccache and a cached Linux git
copy); pull requests always use GitHub's runners.

## Repository layout

| Path | Contents |
| --- | --- |
| `sm8250-huawei-dby-w09.dts` | board device tree |
| `dby-w09-rootparam.dtso` | the DTBO entry ABL accepts |
| `panel/` | DSI panel and RT8555 backlight drivers, tree installer; `gen_panel_cmds.py` regenerates the init sequence from the tablet's own device tree |
| `touch/huawei-thp-lite.c` | touch/pen bridge for Huawei's userspace daemon |
| `power/` | fuel gauge driver, PMIC/charger device tree, `README.md` |
| `sound/` | patch scripts: primary TDM, TDM group/control, S32 COPPs, DP stream |
| `usb/` | Type-C/DP device tree, monitor mode patch, `README.md` |
| `configure-and-build.sh` | applies everything to a kernel tree, configures, builds |
| `build_modules.sh`, `build_release.sh`, `fetch_busybox.sh` | modules, blob-free bundle, initramfs busybox |
| `sd_root_init.sh` | initramfs that boots the microSD root |
| `set_bd_address.py` | writes a Bluetooth address into the DTB |
| `build_deb.sh`, `deb/matepad-kernel-install` | the Debian package and its boot-partition installer |
| `pack_dtbo.py` | packs the DTBO table |
| `tools/mkbootimg.py` | AOSP mkbootimg (Apache-2.0) |
| `build_dev_images.sh`, `repack_boot.py` | development: partition images for one tablet from its stock dumps (`backups/`) |
| `install_images.py` | writes boot images from a running Linux, with backup and readback check |
| `reboot_bootloader.c` | initramfs helper: reboot into fastboot |

## Recovery

Every partition this port writes (`boot`, `ramdisk`, `dtbo`) is backed up
before it is written. To return to HarmonyOS from fastboot, flash the three
stock images back:

```bash
fastboot flash boot boot.img
fastboot flash ramdisk ramdisk.img
fastboot flash dtbo dtbo.img
fastboot reboot
```

From a running Kubuntu, **Restart to HarmonyOS** in the menu does the same.
