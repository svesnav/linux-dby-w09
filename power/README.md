# Battery, charger and charging LED

## Fuel gauge and charger (`matepad-fg.c`)

The driver exposes the PM8150B GEN4 fuel gauge as the `battery` power supply
and the valid USB/DC input as `matepad-charger`; UPower and Plasma read them
through the standard power-supply interface.

- **Read-only gauge access.** It never resets the gauge, loads battery
  profiles, changes charge limits or writes SRAM; it relies on the gauge as
  the boot firmware initialised it. If the algorithm is not ready or a sample
  fails, it reports no data rather than a guessed percentage. Register
  type/subtype are checked before binding.
- **Samples.** Percentage, voltage and current come from matching shadow
  registers, polled every second; changes of capacity, status, presence or
  input raise power-supply events. Current keeps the stock sign: negative
  while charging. Time estimates, learned capacity and temperature are not
  exposed.
- **Status.** Charging/discharging/full come from the charger's input-path
  validity, current direction and termination state. `online` requires
  `POWER_PATH_STATUS` to select USB/DC with `VALID_INPUT_POWER_SOURCE`, as in
  the stock driver.
- **Charger watchdog** (`huawei,service-charger-watchdog`). The firmware
  leaves the SMB5 watchdog enabled with a 128-second bark timeout that
  disables charging on bite (`WD_CFG=0x62`, `SNARL_BARK_BITE_WD_CFG=0x8c`).
  While a valid battery sample and external power are present, the driver
  services it every eight seconds by writing bit 0 of `0x1643`, the same
  operation as the stock `wdog_bark_irq_handler`. It never enables charging,
  clears faults, disables the watchdog or touches thermal and charge-limit
  settings; if the firmware disabled the watchdog, nothing is written.

Register definitions and conversions follow Huawei's GPL 4.19 sources
(`fg-reg.h`, `fg-util.c`, `smb5-reg.h`).

## Charging LED

`power.dtsi` describes the gauge and enables the upstream PM8150L LPG
driver; the red, green and blue channels are LPG 1/2/3 (the stock tree's PWM
channels 0/1/2) and start off. The userspace service `matepad-chargeled`
(kubuntu-dby-w09 `power/`) shows red while charging, green when full and
nothing otherwise, with the stock brightness levels; without a valid battery
sample the LED stays off.

## Build

`configure-and-build.sh` installs the driver and the device-tree include and
builds the gauge, the LPG, the multicolor LED class and the PBS dependency
into the kernel.

## Limits

- Charging may pause while power stays connected: the gauge then reports
  `Not charging` and the LED is off; firmware decisions are not overridden.
- Host-only docks (data without power delivery) are not validated.
- This is battery reporting and indication, not a full charger driver.
