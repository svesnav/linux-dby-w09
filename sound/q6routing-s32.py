#!/usr/bin/env python3
"""Let q6routing open 32-bit ADM COPPs for S32_LE back ends (idempotent).

q6routing takes the COPP bit width from the back end's hw_params but only
knows S16_LE and S24_LE; an S32_LE back end (the DBY-W09 speaker TDM port,
4 x S32_LE as on HarmonyOS) left it at 16, and the DSP refused the COPP
(ADM_CMD_DEVICE_OPEN_V5 error 1).  Usage: q6routing-s32.py KERNEL_SOURCE_DIR
"""
import sys
from pathlib import Path

f = Path(sys.argv[1]) / "sound/soc/qcom/qdsp6/q6routing.c"
t = f.read_text()
if "session->bits_per_sample = 32;" in t:
    print("already patched", f)
    sys.exit(0)
old = ("\tcase SNDRV_PCM_FORMAT_S24_LE:\n"
       "\t\t\tsession->bits_per_sample = 24;\n"
       "\t\tbreak;\n")
if old not in t:
    sys.exit(f"anchor not found in {f}")
t = t.replace(old, old +
              "\tcase SNDRV_PCM_FORMAT_S32_LE:\n"
              "\t\t\tsession->bits_per_sample = 32;\n"
              "\t\tbreak;\n", 1)
f.write_bytes(t.encode())
print("patched", f)
