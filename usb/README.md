# USB-C: host mode, DisplayPort and HDMI audio

USB-C works as a host (hubs, keyboards, storage, USB Ethernet) and as a
peripheral (the initramfs's serial gadget), and drives external monitors
through DisplayPort Alt Mode, including HDMI/DP audio.

## Hardware

`typec.dtsi` describes the PM8150B TCPM/PD controller, DWC3 dual-role USB,
the QMP USB3/DP combo PHY, the NB7VPQ904M redriver (I2C15, 0x1c) and the
FSA4480 AUX switch (I2C15, 0x43); addresses and PD supply follow the stock
device tree. The board adds PM8150 L18 for the PHY and enables DP. The
USB/Type-C drivers are built in so the initramfs can offer its serial gadget
when the tablet is connected to a PC.

## Display

Two DP lanes at HBR2. Docks with HDMI converters work at 2560×1440 60/72 Hz.

Initial or repeated detection sometimes leaves DP connected without an EDID
(generic name, 640×480 fallback modes). Re-entering the partner's DP Alt Mode
restores it: `matepad-dp-recover` (kubuntu-dby-w09 `usb/`) does this after
five empty-EDID checks, at most three times per attachment, with a
30-second cooldown. It never substitutes a saved EDID or forces a mode. The
detection race itself still needs a kernel fix.

`vg27a-native-modes.py` adds an MSM DP mode filter for the ASUS VG27A
(product AUS 0x2723 / name `VG27A`): its EDID, as passed through HDMI
bridges, carries duplicate and inconsistent 4K entries, so modes larger than
its native 2560×1440 are rejected. Other monitors, the internal panel and
the EDID audio data are unaffected.

## Audio

| Front end | PCM | Use |
| --- | --- | --- |
| MultiMedia1 | 0 | speakers |
| MultiMedia2 | 1 | microphones |
| MultiMedia3 | 2 | DisplayPort / HDMI |

Keep the `&q6asmdai` children in numeric order: ASoC indexes the child array,
so listing `dai@2` first swaps the mappings and breaks UCM.

The DP back end links `DISPLAY_PORT_RX` to the MSM DP HDMI codec.
`sound/q6afe-dp-stream.py` adds what the DSP needs, as the downstream driver
does:

- DSP display stream and controller selection (DP0/SST0, parameters
  0x102b5/0x102b6, version 1);
- DP codec preparation before the AFE port starts (it provides the clock);
- AFE stop in `hw_free`, before codec shutdown removes that clock.

Without all three, repeated starts end in AFE timeouts (`-110`) and DSP
error 9, and PipeWire blocks.

`matepad-hdmi-audio` (kubuntu-dby-w09 `usb/`) creates a separate stereo
S16LE/48 kHz PipeWire sink when a monitor with EDID audio is connected; it
stays outside UCM so a missing monitor never removes the speakers or
microphones.

## Known issues

- DP detection can miss the EDID (worked around in userspace, above).
- Occasionally the ADSP starts without its APR audio service after a warm
  reboot, leaving no sound card; restarting the ADSP remote processor
  recovers it. There is no automatic workaround yet.
- Ethernet throughput and storage write speed through docks are untested.
