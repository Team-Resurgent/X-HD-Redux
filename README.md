# X-HD Redux

Alternative EXPERIMENTAL firmware for the MakeMHz HD+ (Original Xbox HDMI), with a configuration 
app and an optional on-screen overlay. It continues the work of
[XboxHDMI](https://github.com/Ryzee119/XboxHDMI-Ryzee119) by Ryzee119 and
[X-HD](https://github.com/Team-Resurgent/X-HD) by Team-Resurgent (now archived).

Do not expect 100% completion, please report issues

> Not made or supported by MakeMHz.

> [!CAUTION]
> **USE AT YOUR OWN RISK!!! IF YOU HAVE NO WAY TO RECOVER WITH A HARDWARE PROGRAMMER THEN PLEASE
> WAIT FOR OTHERS TO TEST AND CONFIRM IT WORKS ON YOUR REVISION XBOX FIRST**

## What's different

- **One firmware for every board.** The video encoder (Conexant, Focus or Xcalibur) is detected
  at boot, so there is a single `firmware.bin`.
- **Mode detection from the signal.** When the BIOS doesn't report the video mode, the firmware
  works it out from the incoming timing and pixel clock, and reads the console's EEPROM for
  region and widescreen.
- **Accurate HDMI signalling.** The refresh rate is measured, and a CEA video code is sent only
  when the picture really matches it. Colour space follows what the TV's EDID supports.
- **HDMI-CEC.** Optionally wakes the TV and switches to the Xbox input at boot. When the console
  turns off, the TV is put back the way it was found (back to the input it was showing, or to
  standby if it was off), or always sent to standby, or left alone -- your choice. The board
  also answers the TV's discovery requests and relays remote-control keys to the console.
- **Picture controls.** Contrast, brightness, saturation, hue, white balance and the black and
  white clip levels, kept separately for SD, HD and non-standard clocks.
- **640-wide SD modes your way.** Fill (a 640-pixel window, no bars) or Exact (sent as the
  standard 480p/576p a TV recognises, with black bars either side).
- **Settings saved on the board.** Colour space, aspect ratio, scan mode, picture position and
  controls, clock phase, the CEC options and the name shown on the TV are stored in flash.
- **X-HD-Config** (`default.xbe`). Change all of the above from the console, check the TV's
  EDID, watch live video status, line up black level with test patterns, (on PAL consoles)
  set the EEPROM up for HD modes, and update the firmware or convert a stock HD+.
- **X-OUT** (`X-OUT.dxt`). An FPS and resolution overlay for consoles running the debug monitor.

## Installing

- **X-HD-Config:** put `firmware.bin` in the `X-HD-Config` folder on the Xbox, run
  `default.xbe` and use the Firmware Update tab. It updates an X-HD or converts a stock HD+.
- **XHDUpdater:** copy `firmware.bin` to the Xbox as `firmware_conexant.bin`,
  `firmware_focus.bin` and `firmware_xcalibur.bin` (the same file under each name), and run
  XHDUpdater. It can also convert a stock HD+.
- **ST-Link:** write `firmware.bin` at `0x08000000`. The programming header pinout is in the
  [X-HD README](https://github.com/Team-Resurgent/X-HD).

For X-HD-Config, copy the `X-HD-Config` folder to the Xbox and run `default.xbe`. For X-OUT,
copy `X-OUT.dxt` to `E:\dxt` and reboot.


## Known limitations

- **4:3 or 16:9 on 480p.** For the 480p modes the console does not tell the board which aspect
  the picture is in; the choice never reaches the bus. The board follows the dashboard's
  Widescreen setting (stored in the console's EEPROM) and, on consoles that provide it, a flag in
  the mode packet. Enabling Cerbios Screenshot in settings avoids this issue. 
- **CEC depends on the TV.** Each Manufacturer does CEC a little differently. What is supported
  varies. 

## Building

- Firmware: `pio run -e combined_stm32f0` (output in `.pio/build/combined_stm32f0/`).
- X-HD-Config and X-OUT need the Team-Resurgent RXDK: `X-HD-Config/build.sh`, and
  `Rxdk.Cli build --project-root X-OUT`. On Linux, run `X-OUT/patch_rxdk_imagebld.sh` once
  first.

