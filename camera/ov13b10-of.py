#!/usr/bin/env python3
"""OV13B10 (rear camera) from the device tree.

The mainline driver matches on ACPI only (Intel laptops) and switches one
supply. Here it gets an OF match ("ovti,ov13b10") and the sensor's other
two rails, DOVDD (1.8 V I/O) and DVDD (core), as optional regulators,
switched on in the datasheet order DOVDD, AVDD, DVDD and off in reverse.
"""

import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "drivers/media/i2c/ov13b10.c"
text = path.read_text()
marker = "/* MatePad: DOVDD and DVDD */"


def replace(old, new):
    global text
    if text.count(old) != 1:
        sys.exit(f"{path}: anchor not found once: {old.strip()[:60]!r}")
    text = text.replace(old, new)


if marker not in text:
    replace("\tstruct regulator *avdd;\n",
            "\tstruct regulator *avdd;\n"
            "\t/* MatePad: DOVDD and DVDD */\n"
            "\tstruct regulator *dovdd;\n"
            "\tstruct regulator *dvdd;\n")

    replace("""	gpiod_set_value_cansleep(ov13b10->reset, 1);

	if (ov13b10->avdd)
		regulator_disable(ov13b10->avdd);
""", """	gpiod_set_value_cansleep(ov13b10->reset, 1);

	if (ov13b10->dvdd)
		regulator_disable(ov13b10->dvdd);
	if (ov13b10->avdd)
		regulator_disable(ov13b10->avdd);
	if (ov13b10->dovdd)
		regulator_disable(ov13b10->dovdd);
""")

    replace("""	if (ov13b10->avdd) {
		ret = regulator_enable(ov13b10->avdd);
		if (ret < 0) {
			dev_err(dev, "failed to enable avdd: %d", ret);
			clk_disable_unprepare(ov13b10->img_clk);
			return ret;
		}
	}
""", """	if (ov13b10->dovdd) {
		ret = regulator_enable(ov13b10->dovdd);
		if (ret < 0) {
			dev_err(dev, "failed to enable dovdd: %d", ret);
			clk_disable_unprepare(ov13b10->img_clk);
			return ret;
		}
	}

	if (ov13b10->avdd) {
		ret = regulator_enable(ov13b10->avdd);
		if (ret < 0) {
			dev_err(dev, "failed to enable avdd: %d", ret);
			if (ov13b10->dovdd)
				regulator_disable(ov13b10->dovdd);
			clk_disable_unprepare(ov13b10->img_clk);
			return ret;
		}
	}

	if (ov13b10->dvdd) {
		ret = regulator_enable(ov13b10->dvdd);
		if (ret < 0) {
			dev_err(dev, "failed to enable dvdd: %d", ret);
			if (ov13b10->avdd)
				regulator_disable(ov13b10->avdd);
			if (ov13b10->dovdd)
				regulator_disable(ov13b10->dovdd);
			clk_disable_unprepare(ov13b10->img_clk);
			return ret;
		}
	}
""")

    replace("""					     "failed to get avdd regulator\\n");
	}

	return 0;
}
""", """					     "failed to get avdd regulator\\n");
	}

	ov13b->dovdd = devm_regulator_get_optional(ov13b->dev, "dovdd");
	if (IS_ERR(ov13b->dovdd)) {
		ret = PTR_ERR(ov13b->dovdd);
		ov13b->dovdd = NULL;
		if (ret != -ENODEV)
			return dev_err_probe(ov13b->dev, ret,
					     "failed to get dovdd regulator\\n");
	}

	ov13b->dvdd = devm_regulator_get_optional(ov13b->dev, "dvdd");
	if (IS_ERR(ov13b->dvdd)) {
		ret = PTR_ERR(ov13b->dvdd);
		ov13b->dvdd = NULL;
		if (ret != -ENODEV)
			return dev_err_probe(ov13b->dev, ret,
					     "failed to get dvdd regulator\\n");
	}

	return 0;
}
""")

    replace("MODULE_DEVICE_TABLE(acpi, ov13b10_acpi_ids);\n",
            "MODULE_DEVICE_TABLE(acpi, ov13b10_acpi_ids);\n\n"
            "static const struct of_device_id ov13b10_of_match[] = {\n"
            "\t{ .compatible = \"ovti,ov13b10\" },\n"
            "\t{ }\n"
            "};\n"
            "MODULE_DEVICE_TABLE(of, ov13b10_of_match);\n")

    replace("\t\t.acpi_match_table = ACPI_PTR(ov13b10_acpi_ids),\n",
            "\t\t.acpi_match_table = ACPI_PTR(ov13b10_acpi_ids),\n"
            "\t\t.of_match_table = ov13b10_of_match,\n")

    path.write_text(text)
