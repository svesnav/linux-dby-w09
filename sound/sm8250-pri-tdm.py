#!/usr/bin/env python3
"""Teach sound/soc/qcom/sm8250.c the DBY-W09 speaker backend (idempotent).

The four TFA9874 amps sit on PRIMARY TDM (GPIO 138-141), as on HarmonyOS:
LPASS is the clock master at 6.144 MHz = 48 kHz x 4 slots x 32 bit.  The
backend has 4 channels, one per slot, as the HarmonyOS mixer path; each amp
picks the slot it plays in its own register 0x26.  Also: the mic back end
(TX_CODEC_DMA_TX_3, WCD938x ADC1-4) gets 4 channels instead of 2.
Usage: sm8250-pri-tdm.py KERNEL_SOURCE_DIR
"""
import sys
from pathlib import Path

f = Path(sys.argv[1]) / "sound/soc/qcom/sm8250.c"
t = f.read_text()
if "PRI_TDM_BCLK_RATE" in t:
    print("already patched", f)
    sys.exit(0)


def rep(old, new):
    global t
    if old not in t:
        sys.exit(f"anchor not found in {f}: {old[:60]!r}")
    t = t.replace(old, new, 1)


rep("#define MI2S_BCLK_RATE\t\t1536000\n",
    "#define MI2S_BCLK_RATE\t\t1536000\n"
    "#define PRI_TDM_BCLK_RATE\t6144000\n"
    "#define PRI_TDM_SLOTS\t\t4\n"
    "#define PRI_TDM_SLOT_WIDTH\t32\n\n"
    "/* Byte offset of each channel in the TDM frame: slots 0-3. */\n"
    "static const unsigned int pri_tdm_slot_offset[] = { 0, 4, 8, 12 };\n")

# Backend format: 4 x S32_LE on the TDM port as on HarmonyOS (needs
# q6routing-s32.py; with 16/24-bit samples in the 32-bit slots the speakers
# were very quiet), 2 x S16_LE elsewhere (as before).
rep("\tstruct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);\n\n"
    "\trate->min = rate->max = 48000;\n"
    "\tchannels->min = channels->max = 2;\n"
    "\tsnd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);\n",
    "\tstruct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);\n"
    "\tstruct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);\n\n"
    "\trate->min = rate->max = 48000;\n"
    "\tchannels->min = channels->max = 2;\n"
    "\tif (cpu_dai->id == PRIMARY_TDM_RX_0) {\n"
    "\t\tchannels->min = channels->max = PRI_TDM_SLOTS;\n"
    "\t\tsnd_mask_none(fmt);\n"
    "\t\tsnd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S32_LE);\n"
    "\t} else {\n"
    "\t\t/* The four analog mics (WCD938x ADC1-4) on one codec DMA port. */\n"
    "\t\tif (cpu_dai->id == TX_CODEC_DMA_TX_3)\n"
    "\t\t\tchannels->min = channels->max = 4;\n"
    "\t\tsnd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);\n"
    "\t}\n")

rep("\tcase QUINARY_MI2S_RX:\n"
    "\t\tcodec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;\n"
    "\t\tsnd_soc_dai_set_sysclk(cpu_dai,\n"
    "\t\t\tQ6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT,\n"
    "\t\t\tMI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);\n"
    "\t\tsnd_soc_dai_set_fmt(cpu_dai, fmt);\n"
    "\t\tsnd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);\n"
    "\t\tbreak;\n",
    "\tcase QUINARY_MI2S_RX:\n"
    "\t\tcodec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;\n"
    "\t\tsnd_soc_dai_set_sysclk(cpu_dai,\n"
    "\t\t\tQ6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT,\n"
    "\t\t\tMI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);\n"
    "\t\tsnd_soc_dai_set_fmt(cpu_dai, fmt);\n"
    "\t\tsnd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);\n"
    "\t\tbreak;\n"
    "\tcase PRIMARY_TDM_RX_0: {\n"
    "\t\tint ret;\n\n"
    "\t\tret = snd_soc_dai_set_sysclk(cpu_dai,\n"
    "\t\t\tQ6AFE_LPASS_CLK_ID_PRI_TDM_IBIT,\n"
    "\t\t\tPRI_TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);\n"
    "\t\tif (!ret)\n"
    "\t\t\tret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0xf,\n"
    "\t\t\t\tPRI_TDM_SLOTS, PRI_TDM_SLOT_WIDTH);\n"
    "\t\tif (!ret)\n"
    "\t\t\tret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,\n"
    "\t\t\t\tARRAY_SIZE(pri_tdm_slot_offset),\n"
    "\t\t\t\tpri_tdm_slot_offset);\n"
    "\t\tif (ret) {\n"
    "\t\t\tdev_err(rtd->dev, \"PRI TDM setup failed: %d\\n\", ret);\n"
    "\t\t\treturn ret;\n"
    "\t\t}\n"
    "\t\tbreak;\n"
    "\t}\n")

rep("static const struct snd_soc_ops sm8250_be_ops = {\n"
    "\t.startup = sm8250_snd_startup,\n"
    "\t.shutdown = qcom_snd_sdw_shutdown,\n",
    "static void sm8250_snd_shutdown(struct snd_pcm_substream *substream)\n"
    "{\n"
    "\tstruct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);\n"
    "\tstruct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);\n\n"
    "\tif (cpu_dai->id == PRIMARY_TDM_RX_0)\n"
    "\t\tsnd_soc_dai_set_sysclk(cpu_dai, Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT,\n"
    "\t\t\t\t       0, SNDRV_PCM_STREAM_PLAYBACK);\n\n"
    "\tqcom_snd_sdw_shutdown(substream);\n"
    "}\n\n"
    "static const struct snd_soc_ops sm8250_be_ops = {\n"
    "\t.startup = sm8250_snd_startup,\n"
    "\t.shutdown = sm8250_snd_shutdown,\n")

f.write_bytes(t.encode())
print("patched", f)
