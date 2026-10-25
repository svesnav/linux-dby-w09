#!/usr/bin/env python3
"""Match downstream afe_set_display_stream() for SM8250 SST DP audio.

Huawei's DSP requires HDMI_DP_MST_VID_IDX_CFG and HDMI_DPTX_IDX_CFG before
AFE_DEVICE_START. Both version-1 payloads select index 0 (DP0, SST0).
Use the existing AFE v2 parameter helper, as for the audio interface config.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
path = root / "sound/soc/qcom/qdsp6/q6afe.c"
text = path.read_text()
anchor = "\tint ret, param_id = port->cfg_type;\n\tstruct apr_pkt *pkt;"
new = anchor + """

\t/* SM8250 DP0/SST0: downstream afe_set_display_stream(). */
\tif (port_id == AFE_PORT_ID_HDMI_OVER_DP_RX) {
\t\tstruct { u32 minor_version; u32 index; } __packed cfg = { 1, 0 };

\t\tret = q6afe_port_set_param_v2(port, &cfg, 0x000102b5,
\t\t\t\tAFE_MODULE_AUDIO_DEV_INTERFACE, sizeof(cfg));
\t\tif (ret)
\t\t\treturn ret;
\t\tret = q6afe_port_set_param_v2(port, &cfg, 0x000102b6,
\t\t\t\tAFE_MODULE_AUDIO_DEV_INTERFACE, sizeof(cfg));
\t\tif (ret)
\t\t\treturn ret;
\t}
"""
if new not in text:
    assert text.count(anchor) == 1, "q6afe_port_start anchor changed"
    path.write_text(text.replace(anchor, new))

# The display clock must also be enabled before the DSP starts the port.
path = root / "sound/soc/qcom/sm8250.c"
text = path.read_text()
start = text.find("\t/* DP must supply its audio clock before AFE_DEVICE_START. */")
if start >= 0:
    end = text.index("\treturn qcom_snd_sdw_prepare", start)
    text = text[:start] + text[end:]
    text = text.replace("#include <linux/delay.h>\n", "")
anchor = "\treturn qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);"
new = """\t/* Prepare DP clocks before AFE_DEVICE_START (CPU DAI prepare). */
\tif (cpu_dai->id == DISPLAY_PORT_RX)
\t\treturn snd_soc_dai_prepare(snd_soc_rtd_to_codec(rtd, 0), substream);

""" + anchor
if new not in text:
    assert text.count(anchor) == 1
    text = text.replace(anchor, new)
path.write_text(text)

path = root / "sound/soc/qcom/qdsp6/q6afe-dai.c"
text = path.read_text()
anchor = "static const struct snd_soc_dai_ops q6hdmi_ops = {"
helper = """/* Stop the DSP while the HDMI codec still supplies its audio clock.
 * ASoC calls codec shutdown before CPU shutdown; hw_free runs earlier.
 */
static int q6hdmi_hw_free(struct snd_pcm_substream *substream,
                         struct snd_soc_dai *dai)
{
\tq6afe_dai_shutdown(substream, dai);
\treturn 0;
}

"""
if helper not in text:
    assert text.count(anchor) == 1
    text = text.replace(anchor, helper + anchor + "\n\t.hw_free = q6hdmi_hw_free,")
    path.write_text(text)
