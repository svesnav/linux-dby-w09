#!/usr/bin/env python3
"""Copy the DBY-W09 panel and RT8555 backlight drivers into a kernel tree and
register them in Kconfig/Makefile (idempotent).  Usage: install_into_tree.py SRC"""
import shutil
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
src = Path(sys.argv[1])
panel = src / "drivers/gpu/drm/panel"
backlight = src / "drivers/video/backlight"

shutil.copy(here / "panel-huawei-dby-w09.c", panel)
shutil.copy(here / "panel-huawei-dby-w09-cmds.h", panel)
shutil.copy(here / "rt8555-backlight.c", backlight / "rt8555_bl.c")


def register(kconfig: Path, before: str, entry: str, makefile: Path, obj: str) -> None:
    text = kconfig.read_text()
    if entry.split("\n")[0] not in text:
        anchor = f"config {before}\n"
        if anchor not in text:
            raise SystemExit(f"{anchor.strip()} not found in {kconfig}")
        kconfig.write_text(text.replace(anchor, entry + "\n" + anchor, 1))
    make = makefile.read_text()
    if obj not in make:
        makefile.write_text(make + obj + "\n")


register(panel / "Kconfig", "DRM_PANEL_ILITEK_IL9322",
         "config DRM_PANEL_HUAWEI_DBY_W09\n"
         "\ttristate \"Huawei MatePad 11 DBY-W09 dual-DSI panel\"\n"
         "\tdepends on OF && DRM_MIPI_DSI && BACKLIGHT_CLASS_DEVICE\n",
         panel / "Makefile",
         "obj-$(CONFIG_DRM_PANEL_HUAWEI_DBY_W09) += panel-huawei-dby-w09.o")
register(backlight / "Kconfig", "BACKLIGHT_SAHARA",
         "config BACKLIGHT_RT8555\n"
         "\ttristate \"Richtek RT8555 backlight\"\n"
         "\tdepends on I2C\n"
         "\tselect REGMAP_I2C\n",
         backlight / "Makefile",
         "obj-$(CONFIG_BACKLIGHT_RT8555) += rt8555_bl.o")
touch = src / "drivers/input/touchscreen"
shutil.copy(here.parent / "touch/huawei-thp-lite.c", touch)
register(touch / "Kconfig", "TOUCHSCREEN_HYCON_HY46XX",
         "config TOUCHSCREEN_HUAWEI_THP_LITE\n"
         "\ttristate \"Huawei THP host interface (MatePad 11 aptouch_daemon)\"\n"
         "\tdepends on SPI && OF\n",
         touch / "Makefile",
         "obj-$(CONFIG_TOUCHSCREEN_HUAWEI_THP_LITE) += huawei-thp-lite.o")
print("drivers installed in", src)
