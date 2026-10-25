#!/usr/bin/env bash
set -euo pipefail

# Run in Ubuntu WSL.  Builds Linux 7.0.y (the Ubuntu 24.04.5 HWE kernel
# line) for the DBY-W09 port; never contacts the tablet.
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Work area outside the repo: build/ (outputs), tools/aosp-mkbootimg, backups/.
project_dir="${MATEPAD_WORK_DIR:-$(cd "$repo_dir/.." && pwd)}"
source_dir="${SOURCE_DIR:-$HOME/matepad-linux-7.0}"
output_dir="${OUTPUT_DIR:-$HOME/matepad-linux-7.0-build}"
board_dts="$source_dir/arch/arm64/boot/dts/qcom/sm8250-huawei-dby-w09.dts"
make_args=(-C "$source_dir" O="$output_dir" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=)
# With CCACHE_DIR set (CI keeps it between builds), compile through ccache;
# both scripts must agree, or the second sees a changed compiler.
if [ -n "${CCACHE_DIR:-}" ] && command -v ccache >/dev/null; then
    make_args+=(CC="ccache aarch64-linux-gnu-gcc")
fi

test -f "$source_dir/Makefile"
mkdir -p "$output_dir"
cp "$repo_dir/sm8250-huawei-dby-w09.dts" "$board_dts"
cp "$repo_dir/usb/typec.dtsi" "$(dirname "$board_dts")/dby-w09-typec.dtsi"
cp "$repo_dir/power/power.dtsi" "$(dirname "$board_dts")/dby-w09-power.dtsi"
python3 "$repo_dir/power/install_into_tree.py" "$source_dir"
if ! grep -q 'sm8250-huawei-dby-w09.dtb' "$source_dir/arch/arm64/boot/dts/qcom/Makefile"; then
    sed -i '/sm8250-hdk.dtb/a dtb-$(CONFIG_ARCH_QCOM)\t+= sm8250-huawei-dby-w09.dtb' \
        "$source_dir/arch/arm64/boot/dts/qcom/Makefile"
fi

# Out-of-tree drivers for the panel and its backlight (panel/).
python3 "$repo_dir/panel/install_into_tree.py" "$source_dir"
# Speaker backend (PRIMARY TDM) for the sm8250 sound card (snd-soc-sm8250.ko).
python3 "$repo_dir/sound/sm8250-pri-tdm.py" "$source_dir"
python3 "$repo_dir/sound/q6afe-dp-stream.py" "$source_dir"
python3 "$repo_dir/usb/vg27a-native-modes.py" "$source_dir"
# TDM group device setup in q6afe (snd-q6afe.ko), as the downstream driver.
python3 "$repo_dir/sound/q6afe-tdm-group.py" "$source_dir"
# TDM invert-sync/data-delay/data-out from the DT actually reach the DSP.
python3 "$repo_dir/sound/q6afe-tdm-ctrl.py" "$source_dir"
# 32-bit ADM COPPs for the S32_LE speaker back end (q6routing.ko).
python3 "$repo_dir/sound/q6routing-s32.py" "$source_dir"

make "${make_args[@]}" defconfig

# Everything the boot needs before the root is mounted is built in: the initramfs
# carries no modules.
builtin=(
    BATTERY_MATEPAD_FG LEDS_CLASS_MULTICOLOR LEDS_QCOM_LPG QCOM_PBS
    SCSI_UFS_QCOM PHY_QCOM_QMP PHY_QCOM_QMP_UFS PHY_QCOM_USB_SNPS_FEMTO_V2
    USB_DWC3 USB_DWC3_QCOM USB_GADGET USB_LIBCOMPOSITE USB_CONFIGFS
    USB_CONFIGFS_ACM USB_CONFIGFS_NCM USB_CONFIGFS_ECM
USB_DWC3_DUAL_ROLE USB_XHCI_HCD USB_XHCI_PLATFORM USB_ROLE_SWITCH
PHY_QCOM_QMP_COMBO TYPEC TYPEC_TCPM TYPEC_QCOM_PMIC
TYPEC_MUX_FSA4480 TYPEC_MUX_NB7VPQ904M TYPEC_DP_ALTMODE
REGULATOR_QCOM_USB_VBUS DRM_MSM_DP
    DRM DRM_SIMPLEDRM DRM_FBDEV_EMULATION FRAMEBUFFER_CONSOLE
    FRAMEBUFFER_CONSOLE_ROTATION FONTS FONT_TER16x32
    REGULATOR_QCOM_RPMH QCOM_RPMH QCOM_RPMHPD QCOM_COMMAND_DB
    SM_GCC_8250 PINCTRL_SM8250 INTERCONNECT_QCOM_SM8250 INTERCONNECT_QCOM_OSM_L3
    SPMI_MSM_PMIC_ARB MFD_SPMI_PMIC PINCTRL_QCOM_SPMI_PMIC
    INPUT_PM8941_PWRKEY KEYBOARD_GPIO POWER_RESET_QCOM_PON REBOOT_MODE
    QCOM_WDT EXT4_FS DEVTMPFS DEVTMPFS_MOUNT BLK_DEV_INITRD
    QCOM_GENI_SE I2C_QCOM_GENI REGULATOR_FIXED_VOLTAGE REGULATOR_TPS65132
    BACKLIGHT_CLASS_DEVICE BACKLIGHT_RT8555 SM_DISPCC_8250
    QCOM_AOSS_QMP QCOM_OCMEM QCOM_LLCC REGULATOR_QCOM_REFGEN
    DRM_MSM DRM_MSM_DSI DRM_MSM_DSI_7NM_PHY DRM_PANEL_HUAWEI_DBY_W09
    F2FS_FS F2FS_FS_XATTR F2FS_FS_POSIX_ACL F2FS_FS_COMPRESSION UNICODE
    FS_ENCRYPTION BLK_DEV_LOOP OVERLAY_FS TMPFS
    MMC MMC_BLOCK MMC_SDHCI MMC_SDHCI_PLTFM MMC_SDHCI_MSM
    POWER_SEQUENCING PCIE_QCOM SERIAL_QCOM_GENI SERIAL_DEV_BUS
    SPI SPI_QCOM_GENI TOUCHSCREEN_HUAWEI_THP_LITE
    ANDROID_BINDER_IPC ANDROID_BINDERFS UHID HIDRAW INPUT_UINPUT
    FW_LOADER_COMPRESS FW_LOADER_COMPRESS_ZSTD FW_LOADER_COMPRESS_XZ
)
# Ubuntu's linux-firmware is zstd-compressed (GPU, Wi-Fi, Bluetooth): the
# FW_LOADER_COMPRESS* entries above let the kernel read it.
# Wi-Fi requests its firmware at probe time, before the microSD root is
# mounted, and Bluetooth/PCI power control can only be modules here: all of
# the QCA6390 stack is installed on the card by build_modules.sh.
modules=(MHI_BUS QRTR QRTR_MHI ATH11K ATH11K_PCI POWER_SEQUENCING_QCOM_WCN
    PCI_PWRCTRL_PWRSEQ BT BT_HCIUART BT_QCA
    # the speaker link's codec stub (linux,spdif-dit); defconfig only had it
    # through other SoCs' sound drivers
    SND_SOC_SPDIF)
# Bool options inside those modules.
module_options=(BT_HCIUART_SERDEV BT_HCIUART_QCA)
# Useful on USB-C host that defconfig leaves out: exFAT/NTFS cards and drives,
# USB Attached SCSI (fast USB SSDs and card readers).
extra_modules=(EXFAT_FS NTFS3_FS USB_UAS)
# Rear camera flash LED (PM8150L), used as the flashlight.
extra_modules+=(LEDS_QCOM_FLASH)
# defconfig targets every arm64 board. Only Qualcomm stays: switching off the
# other SoC platforms drops their drivers too; the rest are PCIe, server and
# network drivers this tablet has no hardware for (about 1300 -> 800 modules).
other_platforms=(ACTIONS AIROHA SUNXI ALPINE APPLE ARTPEC AXIADO BCM BCM2835
    BCM_IPROC BCMBCA BRCMSTB BERLIN BITMAIN BLAIZE BST CIX EXYNOS K3 LG1K HISI
    KEEMBAY MEDIATEK MESON MICROCHIP LAN969X SPARX5 MMP MVEBU NXP LAYERSCAPE
    MXC S32 MA35 NPCM PENSANDO REALTEK RENESAS ROCKCHIP SEATTLE INTEL_SOCFPGA
    SOPHGO STM32 SYNQUACER TEGRA TESLA_FSD SPRD THUNDER THUNDER2 UNIPHIER
    VEXPRESS VISCONTI XGENE ZYNQMP)
unused=(DRM_NOUVEAU DRM_RADEON DRM_AMDGPU DRM_PANFROST DRM_PANTHOR DRM_LIMA
    DRM_ETNAVIV DRM_POWERVR DRM_AST DRM_HISI_HIBMC
    ETHERNET ATA SCSI_LOWLEVEL SCSI_HISI_SAS BLK_DEV_NVME NVME_TARGET CAN MTD
    INFINIBAND XEN KVM
    WLAN_VENDOR_ADMTEK WLAN_VENDOR_ATMEL WLAN_VENDOR_BROADCOM WLAN_VENDOR_INTEL
    WLAN_VENDOR_INTERSIL WLAN_VENDOR_MARVELL WLAN_VENDOR_MEDIATEK
    WLAN_VENDOR_MICROCHIP WLAN_VENDOR_PURELIFI WLAN_VENDOR_RALINK
    WLAN_VENDOR_REALTEK WLAN_VENDOR_RSI WLAN_VENDOR_SILABS WLAN_VENDOR_ST
    WLAN_VENDOR_TI WLAN_VENDOR_ZYDAS WLAN_VENDOR_QUANTENNA
    ATH5K ATH9K ATH6KL ATH10K WCN36XX CARL9170 ATH12K)
args=()
for symbol in "${other_platforms[@]}"; do args+=(-d "ARCH_$symbol"); done
for symbol in "${unused[@]}"; do args+=(-d "$symbol"); done
for symbol in "${builtin[@]}"; do args+=(-e "$symbol"); done
for symbol in "${modules[@]}" "${extra_modules[@]}"; do args+=(-m "$symbol"); done
for symbol in "${module_options[@]}"; do args+=(-e "$symbol"); done
"$source_dir/scripts/config" --file "$output_dir/.config" --keep-case "${args[@]}" \
    --set-str LOCALVERSION "-dby-w09" -d LOCALVERSION_AUTO
make "${make_args[@]}" olddefconfig

missing=0
for symbol in "${builtin[@]}"; do
    if ! grep -qx "CONFIG_${symbol}=y" "$output_dir/.config"; then
        echo "Not built in: CONFIG_${symbol} ($(grep "CONFIG_${symbol}[= ]" "$output_dir/.config" || echo absent))" >&2
        missing=1
    fi
done
for symbol in "${modules[@]}" "${extra_modules[@]}"; do
    if ! grep -qE "^CONFIG_${symbol}=[ym]$" "$output_dir/.config"; then
        echo "Not enabled: CONFIG_${symbol}" >&2
        missing=1
    fi
done
[ "$missing" = 0 ] || exit 1

make "${make_args[@]}" DTC_FLAGS=-@ -j"${JOBS:-28}" \
    Image.gz qcom/sm8250-huawei-dby-w09.dtb
dtc -@ -I dts -O dtb -o "$output_dir/dby-w09-board.dtbo" \
    "$repo_dir/dby-w09-rootparam.dtso"

sha256sum "$output_dir/arch/arm64/boot/Image.gz" \
    "$output_dir/arch/arm64/boot/dts/qcom/sm8250-huawei-dby-w09.dtb" \
    "$output_dir/dby-w09-board.dtbo"
