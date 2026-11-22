# X-HD Redux Configuration Tool (XBE)

A standalone Xbox executable (`default.xbe`) for configuring, calibrating, and monitoring X-HD hardware over the Xbox SMBus (`0xD2` / `0x69`).

Built with the **Team-Resurgent RXDK** toolchain, featuring a retro **green phosphor cathode terminal** visual aesthetic (pure black background `0x00000000`, phosphor glow green `0xFF28DD28` / `0xFF66FF66`, CRT headers and retro framing).

---

## 1. Features & SMBus Control Matrix

The application communicates directly with the X-HD microcontroller using kernel SMBus HAL APIs (`HalReadSMBusValue`, `HalWriteSMBusValue`):

### Tab 1: Settings (video and HDMI-CEC)
One list: the video settings, then under a heading of their own the HDMI-CEC switches and the TV label.
- **Colour Space** (`WRITE_COLOR_SPACE` 139 / `READ_COLOR_SPACE` 11): Choose between Auto, RGB Full (0-255), RGB Limited (16-235), YCbCr 4:4:4 Passthrough, RGB Direct Passthrough, Auto (SD forced to RGB Full), and YCbCr 4:4:4 (Adjustable) -- YCbCr through the CSC, so Tab 3's picture controls apply (newer firmware only).
- **Aspect Ratio** (`WRITE_ASPECT_RATIO` 144 / `READ_ASPECT_RATIO` 16): Auto (follows console/EEPROM), 16:9 Widescreen, 4:3 Standard (applied immediately if SD mode is active).
- **Scan Mode** (`WRITE_SCAN_MODE` 145 / `READ_SCAN_MODE` 17): Auto (underscan when the TV's EDID says it supports it, else the TV's default), TV (Overscan), PC (Underscan 1:1 Pixel Mapping). Every mode is also flagged as IT/game content, whatever the setting.
- **Wire-VIC Mode** (`WRITE_WIRE_VIC_MODE` 152 / Page 63 offset 227): 0 Strict (CEA VIC only when matching geometry/scan/clock) vs 1 Chip Auto (ADV7511 line detector history).
- **Clock Phase Trim** (`WRITE_PHASE` 141 / `READ_PHASE` 13): 0–7 clock phase delay (0.4ns steps from -1.2ns to +1.6ns), shown with its delay in ns.
- **Audio Output Toggle** (`WRITE_AUDIO_MUTE` 158 / `READ_LIVE_FLAGS` 24 bit 0): Test HDMI audio mute / unmute (live only, never saved).
- **Standalone Freeze Mode** (`WRITE_FREEZE` 159 / `READ_LIVE_FLAGS` 24 bit 1): Ignore console SMBus mode packets for bench testing.
- **CEC 12MHz Clock Output** (`WRITE_CEC_CLOCK_OUT` 156 / Page 63 offset 228): MCU PA8 / Pin 29 clock drive to ADV7511 pin 50.
- **CEC One Touch Play** (`WRITE_CEC_ONE_TOUCH` 157 / Page 63 offset 229): Auto-wake TV and switch to Xbox input at boot.
- **TV at Power-Off** (`WRITE_CEC_STANDBY` 162 / Page 63 offset 231): what the TV does when the Xbox powers off -- Restore (the default: back to the source it was showing, or to standby if it was off), Always Standby, or Disabled (the TV is left alone).
- **SPD TV Label Presets** (`WRITE_SPD_DATA` 149 / `WRITE_SPD_APPLY` 150): Commit 24-byte HDMI SPD name.


### Tab 2: Picture Position & Video Mode
- **Live Horizontal Shift (`pos_h`)** (`WRITE_POSITION_H` 153): -127 .. +127 pixels live sync offset.
- **Live Vertical Shift (`pos_v`)** (`WRITE_POSITION_V` 154): -127 .. +127 lines live sync offset.
- **On-Screen Alignment Reticle**: Visual phosphor alignment target.
- **Video Mode**: `(A)` opens a list of the modes the dashboard settings allow (720p, 1080i, 480p, and 480i -- 576i on a PAL console without PAL-60) to calibrate each profile. SD modes are 720x480 (CEA VIC 2/3, 6/7), never 640x480. The app starts in the best of them and returns to it on leaving Tabs 2 and 3; every mode shows the same 720p screen, scaled.
- **Adjust Position**: `(A)` starts moving the picture: D-pad by 1, triggers by 10, `(Y)` centres, `(X)` saves to flash, `(A)` or `(B)` finishes.
- **Reset Position**: Center coordinates back to `(0, 0)`. `(Y)` does the same from the list, so it works blind.
- A warning under the reticle: moving the picture far from centre can make the TV distort or drop it, and `(Y)` re-centres.

### Tab 3: Picture Controls & Test Pattern
The firmware's proc-amp (wire format in [doc/SMBUS.md](../doc/SMBUS.md)): `READ_PICTURE` 27 (43 bytes: the
profile in use, then three 14-byte profiles), staged one profile at a time with
`WRITE_PICTURE_DATA` 163 (index + 14 bytes) and committed with `WRITE_PICTURE_APPLY` 164, saved with
the other settings by `WRITE_CONFIG_ACTION` 1. Every write is read back; the board rejects an
out-of-range block.
- **Video Mode** and **Colour Space**: the same settings as Tabs 2 and 1, here so they can be judged on the pattern. The line above the list shows the colour space on the wire. Colour space 6, **YCbCr Adjustable** (YCbCr 4:4:4 through the CSC), takes the controls where the passthroughs don't; a sink without YCbCr 4:4:4 gets RGB Full (`READ_VIDEO_INFO` byte 18 reads 1). Every colour-space write is read back with `READ_COLOR_SPACE` 11, so firmware that rejects 6 shows "needs newer firmware".
- **Profile**: the board keeps three sets -- SD (SD raster on the 27 MHz CEA clock), HD (720p/1080i) and Non-standard (SD raster on any other clock) -- and picks the one in use itself; the app re-reads it every second. The controls edit the profile in use unless `(A)` picks another (then shown in amber with the one in use).
- **White Clip** (128-255, default 250), **Black Clip** (0-64, default 0), **Contrast** (50-150 %), **Brightness** (-64..+64), **Saturation** (0-200 %), **Hue** (-30..+30 degrees; saturation and hue are sent together as the Q12 pair `chroma_cos`/`chroma_sin`). `(A)` on one starts changing it: Left/Right by 1, triggers by 10, Up/Down to the next control, `(A)`/`(B)` finishes; live, with a slider showing the value and a tick at the default.
- **Test pattern**, beside the list: near-black steps (0, 1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 32), large bars (0, 4, 8, 16), near-white steps (210-255) and 75% colour bars, in framebuffer codes on a code-0 background.
- **Full-Screen Pattern**: the same pattern on the whole screen with how to read it; Up/Down picks the control (colour space, the six picture controls, the six white balance ones), Left/Right/triggers change it, `(X)` saves, `(B)` returns.
- **Reset Picture to Defaults**: the edited profile back to neutral (white balance included), live.
- **White Balance (R/G/B)...**: a sub-page with Red/Green/Blue **Gain** (50-150 %, pivots on black) and **Offset** (-32..+32 codes) for the edited profile, plus Reset White Balance; `(B)` returns. Rows A/B/C are R/G/B in the RGB colour spaces and Cr/Y/Cb in YCbCr Adjustable; the labels follow the colour space on the wire.
- **Copy Profile to All**: the edited profile over the other two (three writes), after a second `(A)`.
- The controls are greyed out with a passthrough colour space (the board sends the encoder's levels untouched there) and on firmware without the v2 `READ_PICTURE` (byte 0 not a profile index, or a field out of range: v1 and older firmware).

### Tab 4: CEC (TV remote and bus)
- **Remote test**: a drawn remote whose keys light up as the TV reports them (`READ_CEC_RX_DATA` 20, relayed by the board), with the last key, the raw frame and the latest traffic. `(A)` or the remote's OK starts it; `(B)` or holding the remote's Back ends it; `(Y)` clears the log. Outside a test the remote drives the app.
- **Only keys a TV can pass to a playback device are drawn**: arrows, OK, Back, menu (any of the five menu codes the board announces in its `<Report Features>` RC profile), Info, play/pause/stop, rewind/fast forward, skip, channel, the colour keys and digits. Power, volume, mute and input have CEC codes but TVs keep them; a TV going to standby shows as `<Standby>` in the log. `<Play>` and `<Deck Control>` (how many TVs send transport keys) light the same buttons.
- **CEC bus** (`READ_CEC_STATUS` 18): sending/idle, last frame acknowledged or timed out, arbitration lost, and the frames received since the app started.
- A note that which keys arrive varies by TV make and model.

### Tab 5: Status
- **System**: this app's version and build time; the board's firmware version, git hash and build time; ADV7511 chip ID and revision; STM32 96-bit unique ID (`READ_ABOUT` 23). Says so when no board answers.
- **Video** (`READ_VIDEO_INFO` 22 / `READ_TELEMETRY` 15), refreshed every second: the raster, colour format and VIC being sent; the measured pixel clock with PLL lock and the encoder; the mode chosen with its decision source, beside the line detector's VIC.
- **Connection**: Hot Plug, Monitor Sense, video output on, HDMI audio on.
- **Counters**: uptime, mode changes, PLL losses, TMDS gate cycles, detector blind events.

### Tab 6: EDID Inspector
- Real-time HDMI DDC EDID reader and decoder.
- Decodes Monitor name, native preferred DTD timing, CEA-861 SVDs (supported 480p, 720p, 1080i, 1080p modes), and audio capabilities (LPCM, Dolby Digital AC3, DTS).
- Press `(X)` to toggle between Decoded Summary and 256-byte Hex Dump.

### Tab 7: EEPROM & Flash
- **Xbox Motherboard EEPROM Video & Audio Configuration** (the heading also shows the TV's audio from the EDID):
  - Displays console Video Region (NTSC-M, NTSC-J, PAL-I, PAL-M) with live PAL detection.
  - Automatic PAL -> NTSC-M recommendation: On stock and softmodded PAL consoles, the MS Dashboard completely suppresses and disables HDTV modes (480p/720p/1080i). When PAL is detected, the X-HD Redux configuration tool recommends and enables switching to NTSC-M.
  - Live comparison of Current EEPROM Video Flags vs Recommended (EDID) Flags (480p, 720p, 1080i, 16:9 Widescreen, PAL 60Hz), and of the Audio Flags: Surround when the TV reports 6+ channels (else Stereo), Dolby Digital and DTS only when the EDID lists them. A sink without audio (no EDID, DVI) leaves the audio flags unchanged.
  - **Apply Recommended EEPROM Settings (from EDID)**: writes the Video/Audio flags and, on a PAL console, switches the region to NTSC-M, recalculating the factory-section checksum. Asks for a second (A) press before writing; (B) or (BACK) cancels. Once written, a further (A) power-cycles the console (the dashboard and the X-HD only read the EEPROM at boot).
- **Flash Actions** (`WRITE_CONFIG_ACTION` 142), selected with the D-pad and run with (A):
  - Save live settings & calibration to Flash Page 63 (`action 1`).
  - Reload saved configuration from Flash Page 63 (`action 4`).
  - Restore factory defaults and commit to Flash (`action 2`) -- asks for a second (A) press; (B) or (BACK) cancels.
  - Exit to Dashboard (asks first when settings are unsaved).

### Tab 8: Firmware Update
Opens on its own at startup when no X-HD application answers (a stock HD+, a board left in
update mode, or no board).
- **Images**: the `.bin` files in the XBE's own folder, `firmware.bin` first; `(A)` on the Image
  line opens a list to pick one. Each is checked before use: bootloader and application reset vectors, and a size
  that stays clear of page 63. Its build (git hash and date) is read from the image.
- **Compare**: on opening the tab when there is one image, or after picking one when there are several, every page's CRC is checked against the
  board (`WRITE_READ_PAGE` 133 / `READ_RAM_PAGE_CRC1-4` 7-10), so the screen says how many
  pages differ. It also warns when the image is the installed build or an older one, and when
  the saved settings would reset (the image doesn't contain the running settings layout tag).
- **Update firmware** (two (A) presses): first the bootloader pages that differ, written by the
  running application (the bootloader holds the mode tables the new application reads); then
  restarts the board in update mode and writes only the application pages that differ, then starts the new application and checks its build. With
  firmware that answers `READ_FLASH_STATUS` (26) each page's write is polled; older firmware
  gets XHDUpdater's fixed waits. A page is tried four times; two failures drop the rest of the
  run to the slower waits. If a page still fails, the board stays in update mode and the tab
  offers **Finish the update**, which writes only the pages still missing.
  Don't switch the display to another input while it runs: the bootloader doing the writing
  doesn't follow hot-plug, so the picture may not come back until the update ends.
- **If the power goes mid-update**, nothing restarts on its own. The board stays in update mode
  and, on the next power-on, its bootloader shows a basic picture in the console's own video
  mode (PAL and 480i included) so the dashboard can be used: start X-HD-Config again and it
  opens on this tab to finish.
- **Bootloader not finished**: if a bootloader page won't write, the tab offers only **Retry**
  (keep the console on). Powered off before it is written, recovering the board needs an
  ST-Link.
- **Convert this HD+ to X-HD** (two (A) presses, about 6 minutes): the X-HD application through
  the HD+ bootloader, then the X-HD bootloader through the new application, with XHDUpdater's
  timings. No backup of the HD+ firmware is made. If anything goes wrong, recovering the board
  needs an ST-Link.
- **Back up the board's firmware**: pages 0-62 to `backup_<git>.bin` in the XBE's folder, each page checked
  against the board's CRC. The file is itself a flashable image (the settings page is left out).
- **Reboot the console**: a full power cycle through the SMC (two (A) presses), so the X-HD
  sees the console boot. With unsaved settings it first offers to save them.

At startup the app points `D:` at the folder `default.xbe` was started from (some launchers leave
`D:` on the DVD drive), so the images, the backups and `Media` are always read from there.

While a job runs the tab keeps the screen: tabs, buttons and Hold (BACK) are ignored until it
ends.

### Tab 9: Help
- **Help** (`help_text.c`): sections in tab order -- Controls, then one per tab.
  Each item is a name and a one-line summary; (A) opens its full explanation (what it does, why,
  and when to change it), wrapped to the screen; (B) closes it, or the section. (X) opens or
  closes every section; the triggers or the left stick scroll.

---

## 2. Controls & Navigation

| Button | Action |
|---|---|
| **WHITE / BLACK** | Cycle Tabs (Settings -> Alignment -> Picture -> CEC -> Status -> EDID -> EEPROM & Flash -> Firmware -> Help); White goes back, Black forward |
| **D-PAD UP / DOWN** | Navigate menu items within active tab |
| **D-PAD LEFT / RIGHT** | Only where the D-pad is the content: moving the picture, changing a picture control, the full-screen pattern, the keyboard |
| **(A)** | Run the selected item, flip an on/off setting, or open a list of values (Up/Down, (A) picks, (B) cancels) |
| **(B)** | Cancel / leave a sub-screen (list, keyboard, full-screen pattern, position adjust, picture control, remote test) / cancel a confirmation |
| **(X)** | Save to Flash (Tabs 1, 2, 3, 7, and while adjusting) / Toggle Hex Dump in EDID / Open/close all Help sections |
| **(Y)** | Reload from Flash (Tabs 1, 3, 7) / Re-centre the picture on Tab 2 / Re-read EDID / Clear the remote log / Rescan in Firmware |
| **(BACK)** | Same as (B) |
| **LEFT / RIGHT TRIGGER** | Move the picture or change a picture control by 10 while adjusting / Switch hex block in EDID / Scroll in Help |
| **Hold (BACK)** | Exit to the dashboard (1.5 s; a tap keeps BACK's normal action). With unsaved settings it asks: save and exit, exit without saving, or stay |
| **TV remote** | Up/Down/OK/Back as D-pad Up/Down, (A), (B); Left/Right switch tabs, except where the D-pad is the content. Tab 4: OK starts the remote test, holding Back ends it. |

### Screen layout
- **Header**: the board state on the right (`FW: v…`, `X-HD IN UPDATE MODE`, `HD+ (STOCK FIRMWARE)`
  or `NO X-HD BOARD`), `[UNSAVED CHANGES]` in amber while settings differ from the last save, and
  a tab strip: every tab's number, the current one bracketed with its name (`[3 PICTURE]`),
  and `Blk\Wht` on the right as the reminder of how to move between them.
- **Message line** (above the keys): plain messages clear after 3 s; errors and the outcome of
  actions (save, reload, restore, EEPROM write) stay in amber until the next button press. Empty
  with nothing to report: the header already shows the board state and unsaved changes.
- **Unsaved changes** are tracked by the app: any setting the board saves (everything on Tabs 1-3
  but audio mute) marks them, a save, reload, restore or board restart clears them. Changes
  left unsaved by an earlier run of the app aren't known to a new one.
- **Two-press actions** (EEPROM write, factory defaults, firmware update/convert, reboot) lapse
  if the second (A) doesn't come within 10 s.

---

## 3. Building

Needs the RXDK (VS Code extension) toolchain; set `RXDK_ROOT` if it isn't in the default
Flatpak VS Code location. To build using the bundled script:
```bash
# Debug build (generates default.xbe and XISO)
./build.sh Debug

# Release build
./build.sh Release
```

Output locations:
- Executable: `out/Release/Build/X-HD-Config/default.xbe`
- ISO image: `out/Release/XISO/X-HD-Config.iso`
