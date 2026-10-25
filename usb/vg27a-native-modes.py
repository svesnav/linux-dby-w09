#!/usr/bin/env python3
"""Keep the ASUS VG27A at native-or-lower resolutions on MSM DP.

Through HDMI bridges its EDID advertises duplicate 4K timings for this 1440p
panel. This is a local
mode-list policy, not a replacement EDID or a claim that 4K input is impossible.
Audio capability data and other monitor models remain untouched.
"""

import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "drivers/gpu/drm/msm/dp/dp_display.c"
text = path.read_text()
marker = "/* MatePad local policy: native-or-lower modes for ASUS VG27A. */"
anchor = "\tlink_info = &msm_dp_display->panel->link_info;\n"
replacement = anchor + """
	/* MatePad local policy: native-or-lower modes for ASUS VG27A. */
	{
		static const struct drm_edid_ident vg27a =
			DRM_EDID_IDENT_INIT('A', 'U', 'S', 0x2723, "VG27A");

		if (msm_dp_display->panel->drm_edid &&
		    drm_edid_match(msm_dp_display->panel->drm_edid, &vg27a) &&
		    (mode->hdisplay > 2560 || mode->vdisplay > 1440))
			return MODE_BAD;
	}
"""
if marker not in text:
    if text.count(anchor) != 1:
        raise SystemExit("Unexpected dp_display.c layout; no changes written")
    path.write_text(text.replace(anchor, replacement))
