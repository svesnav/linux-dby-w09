#!/usr/bin/env python3
"""Venus decoder: end of stream with HarmonyOS's firmware.

V4L2_DEC_CMD_STOP queues an empty input buffer flagged EOS. On SM8250 (IRIS2)
the driver sends it with a NULL address only for firmware up to 1.0.87 and
0xdeadb000 for newer. The tablet's firmware, VIDEO.VPU.1.0-00096-PROD-2,
still wants NULL: with 0xdeadb000 it raises "Exception ... FA = 0x0
cause = 0x6" at the end of every stream, the driver's recovery fails and the
tablet resets. Extend the NULL case to 1.0.96.
"""

import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "drivers/media/platform/qcom/venus/vdec.c"
text = path.read_text()
old = "IS_IRIS2(inst->core) && is_fw_rev_or_older(inst->core, 1, 0, 87)"
new = "IS_IRIS2(inst->core) && is_fw_rev_or_older(inst->core, 1, 0, 96)"

if new not in text:
    if text.count(old) != 1:
        sys.exit(f"{path}: anchor not found once: {old!r}")
    path.write_text(text.replace(old, new))
