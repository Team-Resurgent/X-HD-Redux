# X-HD Redux

Alternative EXPERIMENTAL firmware for the Original Xbox HDMI solutions, with a configuration app and an
optional on-screen overlay. It continues the work of
[XboxHDMI](https://github.com/Ryzee119/XboxHDMI-Ryzee119) by Ryzee119 and
[X-HD](https://github.com/Team-Resurgent/X-HD) by Team-Resurgent.

Do not expect 100% completion, please report issues.

> Back up your original firmware (ST-Link) before flashing,
> and make sure the install works with the stock firmware first.

## What's different

- **One firmware for every board.** The video encoder (Conexant, Focus or Xcalibur) is detected
  at boot, so there is a single `firmware.bin`.
- **Mode detection from the signal.** When the BIOS doesn't report the video mode, the firmware
   attempts to work it out from the incoming timing and pixel clock, and reads the console's 
   EEPROM for region and widescreen.
- **Accurate HDMI signalling.** The refresh rate is measured, and a CEA video code is sent only
  when the picture really matches it. Colour space follows what the TV's EDID supports.
- **HDMI-CEC.** Optionally wakes the TV and switches to the Xbox input at boot, and puts the TV
  in standby when the console turns off.
- **Settings saved on the board.** Colour space, aspect ratio, scan mode, picture position,
  clock phase and the name shown on the TV are stored in flash.
- **X-HD-Config** (`default.xbe`). Change all of the above from the console, check the TV's
  EDID, watch live video status, and (on PAL consoles) set the EEPROM up for HD modes.
- **X-OUT** (`X-OUT.dxt`). An FPS and resolution overlay for consoles running the debug monitor.

## Installing

- **XHDUpdater:** copy `firmware.bin` to the Xbox as `firmware_conexant.bin`,
  `firmware_focus.bin` and `firmware_xcalibur.bin` (the same file under each name), and run
  XHDUpdater.
  [X-HDUpdater](https://github.com/Team-Resurgent/XHDUpdater)
- **ST-Link:** write `firmware.bin` at `0x08000000`. The programming header pinout is in the
  [X-HD README](https://github.com/Team-Resurgent/X-HD).

For X-HD-Config, copy the `X-HD-Config` folder to the Xbox and run `default.xbe`. For X-OUT,
copy `X-OUT.dxt` to `E:\dxt` and reboot. (Console needs to be configured for debug mode for dxt).

## Building

- Firmware: `pio run -e combined_stm32f0` (output in `.pio/build/combined_stm32f0/`).
- X-HD-Config and X-OUT need the Team-Resurgent RXDK: `X-HD-Config/build.sh`, and
  `Rxdk.Cli build --project-root X-OUT`. On Linux, run `X-OUT/patch_rxdk_imagebld.sh` once
  first.
