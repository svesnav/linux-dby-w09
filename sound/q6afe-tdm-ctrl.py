#!/usr/bin/env python3
"""Pass the TDM sync/data controls from the device tree to the DSP (idempotent).

q6afe-dai reads qcom,tdm-data-out, qcom,tdm-invert-sync and qcom,tdm-data-delay
but never copies them into the AFE port config, so every TDM port runs with
a non-inverted sync pulse and no data delay.  The DBY-W09 speaker amps need
HarmonyOS's invert-sync 1 / data-delay 1 (misframed, they played very
quietly).  Adds the three fields to struct q6afe_tdm_cfg (q6afe.h, so
q6afe.ko and q6afe-dai.ko must be rebuilt together) and copies them through.
Usage: q6afe-tdm-ctrl.py KERNEL_SOURCE_DIR
"""
import sys
from pathlib import Path

d = Path(sys.argv[1]) / "sound/soc/qcom/qdsp6"


def patch(name, pairs):
    f = d / name
    t = f.read_text()
    if "ctrl_invert_sync" in t and name != "q6afe.c":
        print("already patched", f)
        return
    if name == "q6afe.c" and "cfg->ctrl_invert_sync" in t:
        print("already patched", f)
        return
    for old, new in pairs:
        if old not in t:
            sys.exit(f"anchor not found in {f}: {old[:60]!r}")
        t = t.replace(old, new, 1)
    f.write_bytes(t.encode())
    print("patched", f)


patch("q6afe.h", [(
    "\tu32\tdata_align_type;\n"
    "\tu16\tch_mapping[AFE_MAX_CHAN_COUNT];\n"
    "};\n",
    "\tu32\tdata_align_type;\n"
    "\tu16\tch_mapping[AFE_MAX_CHAN_COUNT];\n"
    "\tu16\tctrl_data_out_enable;\n"
    "\tu16\tctrl_invert_sync;\n"
    "\tu16\tctrl_data_delay;\n"
    "};\n")])

patch("q6afe-dai.c", [(
    "\ttdm->sync_mode = dai_data->priv[dai->id].sync_mode;\n",
    "\ttdm->sync_mode = dai_data->priv[dai->id].sync_mode;\n"
    "\ttdm->ctrl_data_out_enable = dai_data->priv[dai->id].data_out_enable;\n"
    "\ttdm->ctrl_invert_sync = dai_data->priv[dai->id].invert_sync;\n"
    "\ttdm->ctrl_data_delay = dai_data->priv[dai->id].data_delay;\n")])

patch("q6afe.c", [(
    "\tpcfg->tdm_cfg.nslots_per_frame = cfg->nslots_per_frame;\n",
    "\tpcfg->tdm_cfg.nslots_per_frame = cfg->nslots_per_frame;\n"
    "\tpcfg->tdm_cfg.ctrl_data_out_enable = cfg->ctrl_data_out_enable;\n"
    "\tpcfg->tdm_cfg.ctrl_invert_sync_pulse = cfg->ctrl_invert_sync;\n"
    "\tpcfg->tdm_cfg.ctrl_sync_data_delay = cfg->ctrl_data_delay;\n")])
