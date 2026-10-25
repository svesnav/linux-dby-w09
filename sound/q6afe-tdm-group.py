#!/usr/bin/env python3
"""Configure and enable the AFE TDM group before a TDM port starts (idempotent).

Upstream q6afe starts a TDM port with only its port and slot-mapping config.
HarmonyOS's ADSP also wants the port's group device (PRIMARY TDM RX =
0x9100) configured (AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG) and enabled
(AFE_PARAM_ID_GROUP_DEVICE_ENABLE) first, as the downstream driver does in
msm_dai_q6_tdm_prepare / afe_port_group_enable; without it the port "starts"
but the ADM COPP open on it fails (ADM_CMD_DEVICE_OPEN_V5 error 1).  Group
values as downstream: bit width = slot width, channels = slots per frame.
Usage: q6afe-tdm-group.py KERNEL_SOURCE_DIR
"""
import sys
from pathlib import Path

f = Path(sys.argv[1]) / "sound/soc/qcom/qdsp6/q6afe.c"
t = f.read_text()
if "AFE_MODULE_GROUP_DEVICE" in t:
    print("already patched", f)
    sys.exit(0)


def rep(old, new):
    global t
    if old not in t:
        sys.exit(f"anchor not found in {f}: {old[:60]!r}")
    t = t.replace(old, new, 1)


rep("#define AFE_MODULE_TDM\t\t\t0x0001028A\n",
    "#define AFE_MODULE_TDM\t\t\t0x0001028A\n"
    "#define AFE_MODULE_GROUP_DEVICE\t\t0x00010254\n"
    "#define AFE_PARAM_ID_GROUP_DEVICE_ENABLE\t0x00010256\n"
    "#define AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG\t0x0001029E\n"
    "#define AFE_API_VERSION_GROUP_DEVICE_TDM_CONFIG\t0x1\n"
    "#define AFE_GROUP_DEVICE_NUM_PORTS\t8\n"
    "#define AFE_PORT_INVALID\t\t0xFFFF\n")

rep("struct afe_param_id_cdc_dma_cfg {\n",
    "struct afe_param_id_group_device_tdm_cfg {\n"
    "\tu32\tgroup_device_cfg_minor_version;\n"
    "\tu16\tgroup_id;\n"
    "\tu16\treserved;\n"
    "\tu16\tport_id[AFE_GROUP_DEVICE_NUM_PORTS];\n"
    "\tu32\tnum_channels;\n"
    "\tu32\tsample_rate;\n"
    "\tu32\tbit_width;\n"
    "\tu16\tnslots_per_frame;\n"
    "\tu16\tslot_width;\n"
    "\tu32\tslot_mask;\n"
    "} __packed;\n\n"
    "struct afe_group_device_enable {\n"
    "\tu16\tgroup_id;\n"
    "\tu16\tenable;\n"
    "} __packed;\n\n"
    "struct afe_param_id_cdc_dma_cfg {\n")

rep("int q6afe_port_stop(struct q6afe_port *port)\n{\n",
    "static bool q6afe_is_tdm_port(int port_id)\n"
    "{\n"
    "\treturn port_id >= AFE_PORT_ID_PRIMARY_TDM_RX &&\n"
    "\t       port_id <= AFE_PORT_ID_QUINARY_TDM_TX_7;\n"
    "}\n\n"
    "/* RX_0..7 = base + 0, 2 .. 0xe and TX = base + 1 .. 0xf; group = port + 0x100 */\n"
    "static u16 q6afe_tdm_group_id(int port_id)\n"
    "{\n"
    "\treturn (port_id & ~0xe) + 0x100;\n"
    "}\n\n"
    "static int q6afe_tdm_group_enable(struct q6afe_port *port, bool enable)\n"
    "{\n"
    "\tstruct afe_group_device_enable en = {\n"
    "\t\t.group_id = q6afe_tdm_group_id(port->id),\n"
    "\t\t.enable = enable,\n"
    "\t};\n"
    "\tint ret, i;\n\n"
    "\tif (enable) {\n"
    "\t\tstruct afe_param_id_tdm_cfg *tdm = &port->port_cfg.tdm_cfg;\n"
    "\t\tstruct afe_param_id_group_device_tdm_cfg cfg = {\n"
    "\t\t\t.group_device_cfg_minor_version =\n"
    "\t\t\t\tAFE_API_VERSION_GROUP_DEVICE_TDM_CONFIG,\n"
    "\t\t\t.group_id = en.group_id,\n"
    "\t\t\t.num_channels = tdm->nslots_per_frame,\n"
    "\t\t\t.sample_rate = tdm->sample_rate,\n"
    "\t\t\t.bit_width = tdm->slot_width,\n"
    "\t\t\t.nslots_per_frame = tdm->nslots_per_frame,\n"
    "\t\t\t.slot_width = tdm->slot_width,\n"
    "\t\t\t.slot_mask = tdm->slot_mask,\n"
    "\t\t};\n\n"
    "\t\tcfg.port_id[0] = port->id;\n"
    "\t\tfor (i = 1; i < AFE_GROUP_DEVICE_NUM_PORTS; i++)\n"
    "\t\t\tcfg.port_id[i] = AFE_PORT_INVALID;\n"
    "\t\tret = q6afe_port_set_param(port, &cfg,\n"
    "\t\t\t\t\t   AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG,\n"
    "\t\t\t\t\t   AFE_MODULE_GROUP_DEVICE, sizeof(cfg));\n"
    "\t\tif (ret)\n"
    "\t\t\treturn ret;\n"
    "\t}\n\n"
    "\tret = q6afe_port_set_param(port, &en, AFE_PARAM_ID_GROUP_DEVICE_ENABLE,\n"
    "\t\t\t\t   AFE_MODULE_GROUP_DEVICE, sizeof(en));\n"
    "\tif (ret)\n"
    "\t\tdev_err(port->afe->dev, \"TDM group 0x%x %s failed %d\\n\",\n"
    "\t\t\ten.group_id, enable ? \"enable\" : \"disable\", ret);\n"
    "\treturn ret;\n"
    "}\n\n"
    "int q6afe_port_stop(struct q6afe_port *port)\n{\n")

rep("\tret = afe_apr_send_pkt(afe, pkt, port, AFE_PORT_CMD_DEVICE_STOP);\n"
    "\tif (ret)\n"
    "\t\tdev_err(afe->dev, \"AFE close failed %d\\n\", ret);\n",
    "\tret = afe_apr_send_pkt(afe, pkt, port, AFE_PORT_CMD_DEVICE_STOP);\n"
    "\tif (ret)\n"
    "\t\tdev_err(afe->dev, \"AFE close failed %d\\n\", ret);\n"
    "\telse if (q6afe_is_tdm_port(port_id))\n"
    "\t\tq6afe_tdm_group_enable(port, false);\n")

rep("\tret  = q6afe_port_set_param_v2(port, &port->port_cfg, param_id,\n"
    "\t\t\t\t       AFE_MODULE_AUDIO_DEV_INTERFACE,\n"
    "\t\t\t\t       sizeof(port->port_cfg));\n",
    "\tif (q6afe_is_tdm_port(port_id)) {\n"
    "\t\tret = q6afe_tdm_group_enable(port, true);\n"
    "\t\tif (ret)\n"
    "\t\t\treturn ret;\n"
    "\t}\n\n"
    "\tret  = q6afe_port_set_param_v2(port, &port->port_cfg, param_id,\n"
    "\t\t\t\t       AFE_MODULE_AUDIO_DEV_INTERFACE,\n"
    "\t\t\t\t       sizeof(port->port_cfg));\n")

f.write_bytes(t.encode())
print("patched", f)
