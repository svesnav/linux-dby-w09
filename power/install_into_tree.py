#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys

target = Path(sys.argv[1]) / "drivers/power/supply"
shutil.copy(Path(__file__).with_name("matepad-fg.c"), target)
kconfig = target / "Kconfig"
text = kconfig.read_text()
if "config BATTERY_MATEPAD_FG\n" not in text:
    anchor = "config BATTERY_QCOM_BATTMGR\n"
    assert text.count(anchor) == 1
    text = text.replace(anchor, 'config BATTERY_MATEPAD_FG\n'
                        '\ttristate "Huawei MatePad 11 read-only PM8150B fuel gauge"\n'
                        '\tdepends on OF && MFD_SPMI_PMIC\n\n' + anchor)
    kconfig.write_text(text)
makefile = target / "Makefile"
text = makefile.read_text()
entry = "obj-$(CONFIG_BATTERY_MATEPAD_FG) += matepad-fg.o\n"
if entry not in text:
    makefile.write_text(text + entry)
