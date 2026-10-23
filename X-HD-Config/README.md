# X-HD Configuration Tool (XBE)

A standalone Xbox executable (`default.xbe`) for configuring, calibrating, and monitoring X-HD hardware over the Xbox SMBus (`0xD2` / `0x69`).

Built with the **Team-Resurgent RXDK** toolchain, featuring a retro **green phosphor cathode terminal** visual aesthetic (pure black background `0x00000000`, phosphor glow green `0xFF28DD28` / `0xFF66FF66`, CRT headers and retro framing).

---

## 1. Features & SMBus Control Matrix

The application communicates directly with the X-HD microcontroller using kernel SMBus HAL APIs (`HalReadSMBusValue`, `HalWriteSMBusValue`):

### Tab 1: Video Settings
- **Colour Space** (`WRITE_COLOR_SPACE` 139 / `READ_COLOR_SPACE` 11): Cycle between Auto, RGB Full (0-255), RGB Limited (16-235), YCbCr 4:4:4 Passthrough, RGB Direct Passthrough, and Auto (SD forced to RGB Full).
- **Aspect Ratio** (`WRITE_ASPECT_RATIO` 144 / `READ_ASPECT_RATIO` 16): Auto (follows console/EEPROM), 16:9 Widescreen, 4:3 Standard (applied immediately if SD mode is active).
- **Scan Mode** (`WRITE_SCAN_MODE` 145 / `READ_SCAN_MODE` 17): Auto (EDID IT Content Game Mode), TV (Overscan), PC (Underscan 1:1 Pixel Mapping).
- **Wire-VIC Mode** (`WRITE_WIRE_VIC_MODE` 152 / Page 63 offset 227): 0 Honest (CEA VIC only when matching geometry/scan/clock) vs 1 Chip-Auto (ADV7511 line detector history).
- **Clock Phase Trim** (`WRITE_PHASE` 141 / `READ_PHASE` 13): 0–7 clock phase delay (0.4ns steps from -1.2ns to +1.6ns).
- **Audio Output Toggle** (`WRITE_AUDIO_MUTE` 158 / `READ_LIVE_FLAGS` 24 bit 0): Test HDMI audio mute / unmute.
- **Standalone Freeze Mode** (`WRITE_FREEZE` 159 / `READ_LIVE_FLAGS` 24 bit 1): Ignore console SMBus mode packets for bench testing.

### Tab 2: Picture Position, Video Mode Cycling & Black Level Verifier
- **Live Horizontal Shift (`pos_h`)** (`WRITE_POSITION_H` 153): -127 .. +127 pixels live sync offset.
- **Live Vertical Shift (`pos_v`)** (`WRITE_POSITION_V` 154): -127 .. +127 lines live sync offset.
- **On-Screen Alignment Reticle**: Visual phosphor alignment target with coordinate legend `[ SCREEN CENTER : X=320, Y=240 ]`.
- **Mode Cycling**: `(START)` cycles next video mode; `(BACK)` cycles previous video mode.
- **Black Level & PLUGE Verifier**: Press `(A)` to open dedicated verifier.
  - Press `(X)` to toggle between **LIMITED (16-235)** and **FULL (0-255)** quantization ranges.
  - Step ramps: Sub-black patches (0-12), Reference Black (16), near-black shadow steps (17-32), and highlight contrast steps (210-255).
  - Broadcast PLUGE bar: Sub-8 (-4%), 16 Ref, +2(20), +4(24) reference blocks.
  - Press `(B)` to return to Picture Alignment.
- **Quick Reset (Y Button)**: Center coordinates back to `(0, 0)`.

### Tab 3: CEC & SPD InfoFrame
- **CEC 12MHz Clock Output** (`WRITE_CEC_CLOCK_OUT` 156 / Page 63 offset 228): MCU PA8 / Pin 29 clock drive to ADV7511 pin 50.
- **CEC One Touch Play** (`WRITE_CEC_ONE_TOUCH` 157 / Page 63 offset 229): Auto-wake TV and switch to Xbox input at boot.
- **CEC Shutdown Standby** (`WRITE_CEC_STANDBY` 162 / Page 63 offset 231): Send TV `<Standby>` when Xbox powers off.
- **CEC Bus Diagnostic Bits** (`READ_CEC_STATUS` 18): Real-time monitor of TX busy, ACK status, arbitration lost, and RX queue.
- **SPD TV Label Presets** (`WRITE_SPD_DATA` 149 / `WRITE_SPD_APPLY` 150): Commit 24-byte HDMI SPD name.

### Tab 4: CEC Remote Tester
- Interactive test screen capturing incoming HDMI-CEC consumer control keys from TV remote control.

### Tab 5: Live Telemetry & Diagnostics
- **Measured Input Pixel Clock** (`READ_TELEMETRY` 15 / `READ_VIDEO_INFO` 22): Live Hz with PLL lock status.
- **Arbiter Decision Tracking**: Shows target VIC and decision source.
- **Hardware Integrity**: Hot Plug Detect (HPD), Monitor Sense, Video Ungated, Audio ON, and Fault Codes.

### Tab 6: EDID Inspector
- Real-time HDMI DDC EDID reader and decoder.
- Decodes Monitor name, native preferred DTD timing, CEA-861 SVDs (supported 480p, 720p, 1080i, 1080p modes), and audio capabilities (LPCM, Dolby Digital AC3, DTS).
- Press `(X)` to toggle between Decoded Summary and 256-byte Hex Dump.

### Tab 7: Flash Page 63 & Hardware / EEPROM Settings
- **Hardware Identity** (`READ_ABOUT` 23): ADV7511 chip ID, STM32 MCU 96-bit Unique ID, Git commit hash, and build timestamp.
- **Xbox Motherboard EEPROM Video Configuration**:
  - Displays console Video Region (NTSC-M, NTSC-J, PAL-I, PAL-M) with live PAL detection.
  - Automatic PAL -> NTSC-M recommendation: On stock and softmodded PAL consoles, the MS Dashboard completely suppresses and disables HDTV modes (480p/720p/1080i). When PAL is detected, X-HD Config recommends and enables switching to NTSC-M.
  - Live comparison of Current EEPROM Video Flags vs Recommended (EDID) Flags (480p, 720p, 1080i, 16:9 Widescreen, PAL 60Hz).
  - **Apply Recommended EEPROM Settings (from EDID)**: writes the Video/Audio flags and, on a PAL console, switches the region to NTSC-M, recalculating the factory-section checksum. Asks for a second (A) press before writing; (BACK) cancels.
- **Flash Actions** (`WRITE_CONFIG_ACTION` 142), selected with the D-pad and run with (A); (X), (Y) and (START) are also shortcuts:
  - Save live settings & calibration to Flash Page 63 (`action 1`) -- `(X)`.
  - Reload saved configuration from Flash Page 63 (`action 4`) -- `(Y)`.
  - Reapply current video mode immediately (`action 3`) -- `(START)`.
  - Restore factory defaults and commit to Flash (`action 2`) -- asks for a second (A) press; (BACK) cancels.
  - Exit to Dashboard.

### Tab 8: About
- Expandable per-tab summary of every setting and screen.

---

## 2. Controls & Navigation

| Button | Action |
|---|---|
| **WHITE / BLACK** | Cycle Tabs (Video -> Position -> CEC/SPD -> CEC Remote -> Telemetry -> EDID -> Flash -> About) |
| **D-PAD UP / DOWN** | Navigate menu items within active tab |
| **D-PAD LEFT / RIGHT** | Decrement / Increment selected setting value |
| **(A)** | Execute selected action / Open Black Level Verifier in Tab 2 |
| **(B)** | Return / Exit sub-screens (OSK, Black Level Verifier) |
| **(X)** | Toggle Hex Dump in EDID tab / Toggle Limited-Full in Black Level / Expand-collapse all in About / Quick Save Flash |
| **(Y)** | Reset Position to (0,0) in Tab 2 / Reload from Flash |
| **(START)** | Cycle next video mode in Tab 2 / Reapply mode |
| **(BACK)** | Cycle prev video mode in Tab 2 / Cancel a pending confirmation in Flash tab |
| **LEFT / RIGHT TRIGGER** | Fast position shift (+/- 10 px) in Picture Alignment tab / Scroll in About |
| **Hold (BACK)** | Exit to the dashboard (1.5 s; a tap keeps BACK's normal action) |

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
