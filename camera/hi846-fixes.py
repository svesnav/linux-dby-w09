#!/usr/bin/env python3
"""Hi-846 (front camera) driver fixes.

1. Streaming never started: hi846_set_ctrl() keeps the 1 from
   pm_runtime_get_if_in_use() in ret, and the exposure/vblank cases only
   overwrite ret on errors, so __v4l2_ctrl_handler_setup() in
   hi846_start_streaming() returned 1. set_stream() then powered the sensor
   off again while CAMSS (which only checks for negative values) went on,
   and no frame ever arrived. Reset ret after the runtime-PM check.

2. Mode changes on a 4-lane link: hi846_set_format() checked whether the *current* mode has register tables
for the lane count before looking up the requested one. The default mode
(640x480) has none for 4 lanes, so on a 4-lane link no format could ever be
set and streaming failed with "this mode is not supported for 4 lanes".
Look the new mode up first and check that one; start in the first mode the
link supports. (The tablet uses 2 lanes; this keeps 4 lanes usable.)
"""

import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "drivers/media/i2c/hi846.c"
text = path.read_text()
marker = "/* MatePad: check the requested mode */"
marker_ctrl = "\t/* MatePad: pm_runtime_get_if_in_use() returned 1 */\n"


def replace(old, new):
    global text
    if text.count(old) != 1:
        sys.exit(f"{path}: anchor not found once: {old.strip()[:60]!r}")
    text = text.replace(old, new)


if marker not in text:
    replace("""	const struct hi846_datafmt *fmt = hi846_find_datafmt(mf->code);
	u32 tgt_fps;
""", """	const struct hi846_datafmt *fmt = hi846_find_datafmt(mf->code);
	const struct hi846_mode *mode;
	u32 tgt_fps;
""")

    replace("""	if (hi846->nr_lanes == 2) {
		if (!hi846->cur_mode->reg_list_2lane.num_of_regs) {
			dev_err(&client->dev,
				"this mode is not supported for 2 lanes\\n");
			return -EINVAL;
		}
	} else {
		if (!hi846->cur_mode->reg_list_4lane.num_of_regs) {
			dev_err(&client->dev,
				"this mode is not supported for 4 lanes\\n");
			return -EINVAL;
		}
	}
""", """	/* MatePad: check the requested mode */
	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height, mf->width, mf->height);
	if (hi846->nr_lanes == 2) {
		if (!mode->reg_list_2lane.num_of_regs) {
			dev_err(&client->dev,
				"this mode is not supported for 2 lanes\\n");
			return -EINVAL;
		}
	} else {
		if (!mode->reg_list_4lane.num_of_regs) {
			dev_err(&client->dev,
				"this mode is not supported for 4 lanes\\n");
			return -EINVAL;
		}
	}
""")

    replace("""	hi846->cur_mode =
		v4l2_find_nearest_size(supported_modes,
				       ARRAY_SIZE(supported_modes),
				       width, height, mf->width, mf->height);
""", """	hi846->cur_mode = mode;
""")

    replace("""	hi846->cur_mode = &supported_modes[0];

	ret = hi846_init_controls(hi846);
""", """	hi846->cur_mode = &supported_modes[0];
	while (hi846->nr_lanes == 4 && !hi846->cur_mode->reg_list_4lane.num_of_regs &&
	       hi846->cur_mode < &supported_modes[ARRAY_SIZE(supported_modes) - 1])
		hi846->cur_mode++;

	ret = hi846_init_controls(hi846);
""")

if marker_ctrl not in text:
    replace("""	ret = pm_runtime_get_if_in_use(&client->dev);
	if (!ret || ret == -EAGAIN)
		return 0;
""", """	ret = pm_runtime_get_if_in_use(&client->dev);
	if (!ret || ret == -EAGAIN)
		return 0;
""" + marker_ctrl + "\tret = 0;\n")

path.write_text(text)
