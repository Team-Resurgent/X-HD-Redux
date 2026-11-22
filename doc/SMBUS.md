# X-HD Redux SMBus interface

How software on the Xbox (a BIOS, dashboard, DXT or homebrew app) talks to the X-HD board.
Everything here is checked against `src/application/smbus_i2c.c` and `src/bootloader/smbus_i2c.c`;
opcode numbers are in `src/shared/defines.h`, struct layouts in `src/application/smbus_i2c.h`.

## 1. Bus basics

- **Address:** 7-bit `0x69` (8-bit `0xD2` write / `0xD3` read). On the Xbox kernel:
  `HalReadSMBusByte(0xD2, opcode, &value)` and `HalWriteSMBusByte(0xD2, opcode, value)`.
- **One byte per transaction.** Every operation is a standard SMBus *Read Byte* or *Write Byte*
  with the opcode as the command byte. There are no block transfers; multi-byte data is read or
  written one byte at a time through a cursor (see §2).
- **Opcodes 0-127 are reads, 128-255 are writes** (bit 7 = write).
- **Byte order:** every multi-byte value is little-endian unless stated otherwise (the CRC in §10
  is read MSB first).
- **Cost:** each byte is a full SMBus transaction (address, command, data -- roughly 0.3-0.5 ms on
  the Xbox's 100 kHz bus). Don't stream big structures every frame; see the polling pattern in §9.

## 2. Conventions

**Two firmware images answer at 0x69:**

| | Application (normal) | Bootloader (firmware update / recovery) |
|---|---|---|
| `READ_MODE` (5) | `2` | `1` |
| `READ_VERSION1-4` (1-4) | firmware version | same version bytes (so XHDUpdater can find a board stuck in the bootloader) |
| Supported | everything below | only §10's update opcodes (including `READ_FLASH_STATUS`), `READ_MODE`, `READ_VERSION1-4`, `WRITE_SET_MODE` |

**Unknown or retired opcodes** read back `0x42` from the application, `0xFF` from the bootloader.
Writes to unknown opcodes are ignored.

**Cursors.** Several reads/writes step through a buffer with an auto-incrementing cursor. Each
has its own cursor and its own way to reset it:

| Data | Read | Write | Reset / position the cursor |
|---|---|---|---|
| Mode packet (14 B) | `READ_CONFIG` (0) | `WRITE_CONFIG` (128) | `WRITE_CONFIG_BANK` (129, also zeroes the index) / `WRITE_CONFIG_INDEX` (130) |
| Flash page buffer (1 KB) | `READ_RAM` (6) | `WRITE_RAM` (134) | `WRITE_RAM_BANK` (135) / `WRITE_RAM_INDEX` (136); offset = bank x 256 + index |
| EDID (256 B) | `READ_EDID` (12) | -- | `WRITE_EDID_INDEX` (140); wraps 255 -> 0 |
| Telemetry (20 B) | `READ_TELEMETRY` (15) | -- | `WRITE_TELEMETRY_INDEX` (161) = byte offset; wraps after 20 bytes |
| Video info (24 B) | `READ_VIDEO_INFO` (22) | -- | `WRITE_VIDEO_INFO_LATCH` (151) = 1 |
| About block (60 B) | `READ_ABOUT` (23) | -- | `WRITE_ABOUT_INDEX` (155) = byte offset |
| Live settings (238 B) | `READ_USER_CONFIG` (25) | -- | `WRITE_USER_CONFIG_INDEX` (160) = byte offset; wraps |
| SPD name (24 B) | `READ_SPD_DATA` (21) | `WRITE_SPD_DATA` (149) | `WRITE_SPD_APPLY` (150), any value |
| Picture profiles (43 B read, 15 B write) | `READ_PICTURE` (27) | `WRITE_PICTURE_DATA` (163) | `WRITE_PICTURE_APPLY` (164), any value |
| CEC Rx frame | `READ_CEC_RX_DATA` (20) | -- | `WRITE_CEC_RX_ACK` (148) = 1 |
| CEC Tx frame | -- | `WRITE_CEC_TX_DATA` (146) | `WRITE_CEC_TX_APPLY` (147), any value |

Reading past the end of the mode packet or flash buffer returns an undefined byte (the previous
response); writing past it is ignored.

**Writes that change settings** (139, 141-145, 152-154, 156, 157, 159, 162, 165) take effect from the main loop within a few
milliseconds, never inside the SMBus interrupt. They change the *live* settings only; persist them
with `WRITE_CONFIG_ACTION` = 1 (§6).

## 3. Read opcodes (application)

| Op | Name | Returns |
|---|---|---|
| 0 | `READ_CONFIG` | Next byte of the last *applied* mode packet (§5) |
| 1-4 | `READ_VERSION1-4` | Release version, one byte each: `0`, then major.minor.patch (currently `0 2 0 2` = 2.0.2). `0xFF 0xFF 0xFF 0xFF` means "not an X-HD" to XHDUpdater -- never use that value |
| 5 | `READ_MODE` | `2` application / `1` bootloader |
| 6 | `READ_RAM` | Next byte of the flash page buffer (§10) |
| 7-10 | `READ_RAM_PAGE_CRC1-4` | CRC-32 of the last `WRITE_READ_PAGE`, MSB first (§10) |
| 11 | `READ_COLOR_SPACE` | The colour-space *setting*: 0 Auto, 1 RGB full, 2 RGB limited, 3 YCbCr passthrough, 4 RGB passthrough, 5 Auto with SD forced to RGB full, 6 YCbCr through the CSC (picture controls apply). What is actually on the wire is the video info's `color_space` (§9) |
| 12 | `READ_EDID` | Next byte of the sink's cached EDID (256 bytes, both blocks) |
| 13 | `READ_PHASE` | Clock-phase trim (0-7) saved for the self-latched encoder |
| 14 | -- | Retired (was a packed status byte). Use §9 |
| 15 | `READ_TELEMETRY` | Next byte of the telemetry struct (§8) |
| 16 | `READ_ASPECT_RATIO` | Aspect setting for SD modes: 0 Auto, 1 16:9, 2 4:3 |
| 17 | `READ_SCAN_MODE` | 0 Auto, 1 TV (overscan), 2 PC (underscan) |
| 18 | `READ_CEC_STATUS` | Bits: 0 Tx busy, 1 last Tx acked, 2 last Tx lost arbitration, 3 last Tx retry timeout, 4 Rx frame pending, 5 CEC clock output on (`WRITE_CEC_CLOCK_OUT`), 6 One Touch Play on (`WRITE_CEC_ONE_TOUCH`), 7 a shutdown action is on, i.e. the setting is not Disabled (`WRITE_CEC_STANDBY`; which action is in `READ_USER_CONFIG`, §6) |
| 19 | `READ_CEC_RX_LEN` | Length of the pending Rx frame (1-16), 0 if none |
| 20 | `READ_CEC_RX_DATA` | Next byte of the pending Rx frame (header first); 0 past its end |
| 21 | `READ_SPD_DATA` | Next byte of the live SPD name (24 bytes, wraps) |
| 22 | `READ_VIDEO_INFO` | Next byte of the latched video info (§9) -- also carries the wire-VIC mode and picture position |
| 23 | `READ_ABOUT` | Next byte of the about block (§11): chip ID, MCU UID, git hash, build date |
| 24 | `READ_LIVE_FLAGS` | Bits: 0 audio mute held (`WRITE_AUDIO_MUTE`), 1 freeze on (`WRITE_FREEZE`), 2 the TV sent CEC `<Standby>` (§7) |
| 25 | `READ_USER_CONFIG` | Next byte of the live settings -- the page-63 layout of `user_config.h`'s `xhd_user_config` (238 bytes: colour, aspect, scan, encoder, per-encoder phase and HS/VS calibration, wire-VIC, CEC switches, freeze, SD Fill/Exact) -- at a cursor `WRITE_USER_CONFIG_INDEX` sets; wraps |
| 26 | `READ_FLASH_STATUS` | The last `WRITE_RAM_APPLY` (§10): 0 none since boot, 1 queued, 2 written and verified, 3 rejected or not verified. Bootloader too. Firmware older than this answers `0x42`/`0xFF` |
| 27 | `READ_PICTURE` | Next byte of the picture state (43 bytes, wraps): byte 0 the live profile (0 SD on 27 MHz, 1 HD, 2 non-standard clock), then profiles 0, 1, 2 at 14 bytes each: white clip (128-255), black clip (0-64), contrast % (50-150), brightness (-64..64), saturation x cos(hue) and x sin(hue) as little-endian Q12 int16 (4096 = 100%; +sin turns red toward yellow), gain % for output rows A, B, C (50-150; R, G, B in RGB), bias codes for rows A, B, C (-32..32) |

## 4. Write opcodes (application)

| Op | Name | Value |
|---|---|---|
| 128 | `WRITE_CONFIG` | Next byte of the mode packet being staged (§5) |
| 129 | `WRITE_CONFIG_BANK` | Mode-packet cursor bank (use 0); zeroes the index |
| 130 | `WRITE_CONFIG_INDEX` | Mode-packet cursor index |
| 131 | `WRITE_CONFIG_APPLY` | `1` = apply the staged packet (other values are ignored and logged) |
| 132 | `WRITE_SET_MODE` | `1` = reboot into the bootloader, `2` = into the application. The board resets ~10 ms later whatever the value |
| 133 | `WRITE_READ_PAGE` | Flash page number (0-63) to copy into the page buffer and CRC (§10) |
| 134 | `WRITE_RAM` | Next byte of the page buffer |
| 135 | `WRITE_RAM_BANK` | Page-buffer cursor bank (0-3); zeroes the index |
| 136 | `WRITE_RAM_INDEX` | Page-buffer cursor index |
| 137 | `WRITE_RAM_APPLY` | Page number to program from the buffer (§10) |
| 138 | `WRITE_APP_FLASH_MODE` | Bootloader only (§10); ignored by the application |
| 139 | `WRITE_COLOR_SPACE` | 0-6 as in `READ_COLOR_SPACE`; reapplies the current mode |
| 140 | `WRITE_EDID_INDEX` | Position the `READ_EDID` cursor (0-255) |
| 141 | `WRITE_PHASE` | Clock-phase trim 0-7 for the current encoder, applied live |
| 142 | `WRITE_CONFIG_ACTION` | `1` save settings to flash, `2` restore factory defaults (saved, then reapplied), `3` reapply the current mode, `4` reload the saved settings from flash (then reapply) |
| 143 | -- | Unused (folded into 142) |
| 144 | `WRITE_ASPECT_RATIO` | 0 Auto, 1 16:9, 2 4:3 -- SD modes only (HD is always 16:9); reapplies at once if an SD mode is up |
| 145 | `WRITE_SCAN_MODE` | 0 Auto, 1 TV, 2 PC; reapplies the current mode |
| 146 | `WRITE_CEC_TX_DATA` | Next byte of the CEC frame to send (header first, max 16) |
| 147 | `WRITE_CEC_TX_APPLY` | Frame length (1-16) to send from the staged bytes; always re-arms staging |
| 148 | `WRITE_CEC_RX_ACK` | `1` = release the pending Rx frame (read or not); `2` = clear `READ_LIVE_FLAGS` bit 2 (TV standby) |
| 149 | `WRITE_SPD_DATA` | Next byte of a new SPD name (§7) |
| 150 | `WRITE_SPD_APPLY` | `1` = commit the 24 staged bytes and resend the SPD InfoFrame; `0` = just reset the cursors |
| 151 | `WRITE_VIDEO_INFO_LATCH` | `1` = latch the live video info and reset its cursor (§9) |
| 152 | `WRITE_WIRE_VIC_MODE` | `0` honest (the CEA VIC only when the signal really is that format, else 0), `1` chip-auto (the ADV7511's own detection); reapplies the current mode |
| 153 | `WRITE_POSITION_H` | Picture position, horizontal: signed offset in pixels (-127..127, two's complement) for the active mode; applied live, no blackout |
| 154 | `WRITE_POSITION_V` | Picture position, vertical: signed offset in lines, as above |
| 155 | `WRITE_ABOUT_INDEX` | Position the `READ_ABOUT` cursor (0 = start; out of range = start) |
| 156 | `WRITE_CEC_CLOCK_OUT` | `1` = drive the ADV7511's CEC clock (12 MHz) from the MCU on PA8 / pin 29 (default), `0` = off. Pin 29 is wired to ADV7511 pin 50 (§7). Persist with `WRITE_CONFIG_ACTION` = 1 |
| 157 | `WRITE_CEC_ONE_TOUCH` | `1` = CEC One Touch Play once per boot: wake the TV and switch it to this input (§7) (default), `0` = off; persist with `WRITE_CONFIG_ACTION` = 1 |
| 158 | `WRITE_AUDIO_MUTE` | `1` = mute HDMI audio and hold it through mode changes, `0` = release (unmute). Not saved |
| 159 | `WRITE_FREEZE` | `1` = ignore the console's mode packets (`WRITE_CONFIG_APPLY`) -- pure standalone, a test setting -- `0` = off (default). Live; persist with `WRITE_CONFIG_ACTION` = 1 to apply at every boot |
| 160 | `WRITE_USER_CONFIG_INDEX` | Position the `READ_USER_CONFIG` cursor (byte offset; out of range = 0) |
| 161 | `WRITE_TELEMETRY_INDEX` | Position the `READ_TELEMETRY` cursor (out of range = 0) |
| 162 | `WRITE_CEC_STANDBY` | What the TV does when the console shuts down (§7): `1` = restore it as One Touch Play found it (default), `0` = nothing, `2` = always send `<Standby>`. Other values are ignored. Persist with `WRITE_CONFIG_ACTION` = 1 |
| 163 | `WRITE_PICTURE_DATA` | Next byte of a profile update: the profile index (0-2), then that profile's 14 bytes (layout as in `READ_PICTURE`) |
| 164 | `WRITE_PICTURE_APPLY` | `1` = commit the 15 staged bytes into that profile and refold the live one (CSC and output clip, in place, no blackout); rejected unless all 15 are staged, the index is 0-2 and every field is in range. `0` = just reset the cursors. Persist with `WRITE_CONFIG_ACTION` = 1; `2` resets every profile. The passthrough colour spaces ignore them |
| 165 | `WRITE_SD_EXACT` | 640 SD modes: `0` Fill (a 640 window, default), `1` Exact (the ones on a CEA raster -- Xcalibur 0x0F/0x11/0x05/0x12, Conexant 0x11, Focus 0x11/0x05/0x12 -- go out as that CEA mode: 720 window with the GPU's black either side, CEA sync, VIC 2/3 or 17/18); reapplies the current mode; persist with `WRITE_CONFIG_ACTION` = 1 |

## 5. Mode packet (BIOS -> X-HD)

How an X-HD-aware BIOS tells the board which mode it just set. Stage 14 bytes, then apply:

```
WRITE_CONFIG_BANK (129) = 0
WRITE_CONFIG (128) x 14     -- the struct below, byte by byte
WRITE_CONFIG_APPLY (131) = 1
```

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `encoder`: `0x8A` Conexant, `0xD4` Focus, `0xE0` Xcalibur (the board self-latches it) |
| 1 | 1 | `region`: unused -- always `0x01` in practice, PAL and NTSC alike (the mode index carries 50 Hz) |
| 2 | 4 | `mode`: the console's AV mode dword (below) |
| 6 | 4 | `titleid`: running title's ID (`0` if none) |
| 10 | 4 | `avinfo`: the console's AV info (bit layout below; CerBIOS sends it when that is enabled, otherwise `0`). The board reads bit 16 only: it says 16:9 for the 480p modes |

The `mode` dword is the console's AV mode value:

- bits 31-30: output family (`00` 480-line SDTV, `01` 525-line SDTV, `10` HDTV, `11` VGA)
- bit 28: widescreen. Only the interlaced SD modes carry it. For the 480p mode indices `0x07`,
  `0x08` and `0x11` the console sends the same dword for 4:3 and 16:9 (the choice lives in
  Direct3D's present flags, which never reach the bus in this word), so the board uses `avinfo`
  bit 16, which does follow the title's widescreen flag; only when `avinfo` is `0` does it fall
  back to the EEPROM Widescreen setting (a guess: the HDMI active format then stays `1000`, "same as
  the picture", see §6 Aspect). The Aspect setting (§6) overrides all of this
- bits 23-16: **mode index**, which selects the timing; bits 15-8: CRTC table; bits 7-0: TV table

`avinfo` is the Direct3D library's AV info word, as the running title sees it:

- bits 7-0: AV pack (0 none, 1 composite, 2 RF, 3 SCART, 4 component, 5 VGA, 6 S-Video)
- bits 15-8: region (1 NTSC-M, 2 NTSC-J, 3 PAL-I, 4 PAL-M)
- bit 16 widescreen, 17 720p enabled, 18 1080i enabled, 19 480p enabled, 20 letterbox
- bit 21 encoder interlaced, 22 encoder 60 Hz (NTSC), 23 encoder 50 Hz (PAL)
- bit 24 field (half-height framebuffer) rendering, 25 10x11 pixel aspect rendering

Bits 17-19 describe what the console is set up to offer, so a mode test app only lists 480p
modes once bit 19 is set. Bit 16 is per mode: with the identical 480p mode dword, `00480104`
means 4:3 and `00490104` means 16:9 (Focus rig, verified for indices `0x07`, `0x08`, `0x11`).
For reference, the mode dword's own bits 31-24 are the analog output mode (28 widescreen,
29 SCART, 30 PAL, 31 HDTV) and the DAC enables (27-24).

The packet is applied by the main loop once the input clock confirms it. With the board in
"freeze" (a bench tool) packets are logged but not applied. `READ_CONFIG` reads back the last
applied packet.

## 6. Settings

- **Colour space** (139 / 11): the setting. `Auto` resolves in this order: RGB passthrough if the
  encoder is putting RGB on the bus; RGB limited for an SD mode on an HDMI sink; YCbCr
  passthrough if the sink's EDID supports YCbCr 4:4:4; otherwise RGB full. Value 5 is Auto with
  every SD mode forced to RGB full. Value 6 is YCbCr 4:4:4 through the CSC (an identity table,
  so the picture controls apply), RGB full on a sink without YCbCr 4:4:4. §9's `color_space`
  shows the result.
- **Aspect** (144 / 16): SD only. Takes effect at once if an SD mode is up; while HD is up it is
  stored for the next SD mode. `Auto` follows the console (packet bit 28, `avinfo`, WSS, EEPROM).
  The HDMI AVI InfoFrame also states the aspect in its active format: 16:9 centre (`1010`) or 4:3
  centre (`1001`) whenever it came from HD, the console, WSS or this setting, and "same as the
  picture" (`1000`) only when the board fell back to the EEPROM guess.
- **Scan** (145 / 17): sets the AVI scan field. `Auto` = underscan if the sink's EDID says it
  supports underscan, otherwise "no data".
- **Phase** (141 / 13): DDR clock-delay trim, per encoder.
- **Wire-VIC mode** (152 / video info flag 0x80): honest is the default. Chip-auto is a
  compatibility switch for a TV that misbehaves on VIC 0.
- **SD Fill/Exact** (165, config byte 232): Fill is the default. Exact trades the 640 window for
  the standard 480p/576p a TV recognises, with black bars either side of the picture.
- **Picture position** (153-154 / video info `pos_h`, `pos_v`): an offset from the mode table's
  sync delay, stored per encoder and per mode profile -- each mode keeps its own. Applied live
  to the ADV7511 (no blackout). An offset that would push the delay out of range is ignored.
- **TV at power-off** (162; stored in `READ_USER_CONFIG` byte 231 as `0` restore, `1` disabled,
  `2` always standby -- the wire value for restore is `1`, so older writers keep their meaning).
  See §7.
- **Persisting:** all of the above change the live state only. `WRITE_CONFIG_ACTION` = 1 writes
  them (and the SPD name, the CEC switches and freeze) to flash page 63. `2` restores factory defaults, saves and reapplies.

## 7. CEC relay and SPD name

**CEC** is mostly a thin relay: beyond the pieces listed below (claiming an address, One Touch
Play, the requests it answers, shutdown Standby), X-HD Redux does no CEC protocol work (message
meaning). The chip resends an unacknowledged frame up to 3 times (4 attempts, as Linux).

- Send: `WRITE_CEC_TX_DATA` x N (header first), then `WRITE_CEC_TX_APPLY` = N. Poll
  `READ_CEC_STATUS` until bit 0 (busy) clears; bit 1 = acked, 2 = arbitration lost, 3 = timeout.
- Receive: when `READ_CEC_STATUS` bit 4 is set, read `READ_CEC_RX_LEN`, then that many
  `READ_CEC_RX_DATA`, then `WRITE_CEC_RX_ACK` = 1. Frames are copied out of the chip as they
  arrive into a queue of 4 (the oldest is dropped when it is full), so the chip's own buffers
  stay free and the TV never sees a NACK. A frame not acked within 1 s is dropped, so a console
  with no CEC software cannot block reception.
- CEC clock: the ADV7511's CEC block needs a 12 MHz clock on its pin 50. The MCU drives it
  from PA8 (pin 29), which is wired there -- by design on current hardware, and as a repair on
  a board whose 12 MHz oscillator failed (every send stays busy, nothing is received). On by
  default; `WRITE_CEC_CLOCK_OUT` = 0 turns it off.
- Address and One Touch Play: every boot, from power-on (a TV in standby drops HPD but still
  hears CEC), X-HD Redux claims a Playback logical address (4, else 8, else 11) -- without one
  the chip takes no frames addressed to it -- and, once the TV's EDID gives a physical address,
  broadcasts `<Report Physical Address>`, then CEC 2.0's
  `<Report Features>` (playback; RC profile: a source with root, setup, contents, media top and
  media context menus -- some TVs pass a source only the menu keys it lists). With One Touch
  Play (`WRITE_CEC_ONE_TOUCH`, on by default) it first asks the TV for its power status and
  broadcasts `<Request Active Source>`, and waits up to 0.5 s for the answers (the shutdown
  action below uses them). It then sends `<Image View On>` to wake the TV before the report,
  broadcasts `<Active Source>` so the TV switches to this input, and sends the TV
  `<Set OSD Name>`. A new physical address later (another HDMI port, a receiver in
  between) gives up the logical address and claims one again, as CEC requires, with the same
  report and features but never One Touch Play.
- Answers: to frames addressed to its claimed address, X-HD Redux itself answers
  `<Give OSD Name>` (the SPD product, trimmed, first 14 characters), `<Give Physical Address>`,
  `<Give Device Vendor ID>` (with `<Feature Abort>`: X-HD Redux has no vendor ID),
  `<Give Device Power Status>` (on), `<Get CEC Version>` (2.0), `<Give Features>` (`<Report Features>`, broadcast, so
  also to Unregistered) and `<Menu Request>` (menu
  activated -- some TVs only pass remote keys on after it); those frames never reach the relay.
  Any other request addressed to it gets `<Feature Abort>` [opcode, unrecognized] (`<Abort>`:
  [0xFF, refused]) and still reaches the relay; replies, key presses (`<User Control
  Pressed/Released>`, `<Play>`, `<Deck Control>`), `<Standby>` and broadcasts are never
  aborted, and nothing is answered directly to Unregistered (15). A broadcast-only message
  (`<Active Source>`, `<Routing Change>`, `<Set Stream Path>`, `<Report Features>`, ...) sent
  to it directly has the wrong addressing mode and is ignored: no abort, no routing change. `<Request Active Source>` from the TV is answered with
  `<Active Source>` while this input is shown. Its own frames queue (up to 4) and go out in
  order as the bus frees.
- Shutdown action (`WRITE_CEC_STANDBY`): when the console shuts down -- the power button, or a
  software shutdown; never a reboot or a power cycle -- X-HD Redux acts on the TV (logical
  address 0), from its claimed address, or 15 without one. Three settings:
  - *Restore* (default): put the TV back as One Touch Play found it. A TV that was on goes back
    to the source it was showing (`<Set Stream Path>` to that source's physical address, taken
    from the `<Active Source>` that answered the request above), or, if none answered,
    `<Inactive Source>` (whether a TV then switches input is up to the TV). A TV that was in
    standby, or never answered, gets `<Standby>`.
  - *Always standby*: `<Standby>` whatever the TV was doing.
  - *Disabled*: nothing is sent.

  It sees the shutdown on the console's own SMBus writes to the SMC (0x20): `0x0D` = `0x02` (power button,
  first write) or `0x02` = `0x80` (software shutdown, last write before power-off). A power
  cycle (`0x02` = `0x40`, as dashboards and X-HD-Config reboot) makes the kernel write the same
  `0x0D` = `0x02` about 30 ms later, so one within 1 s of a `0x40` is not a shutdown. Skipped when the TV
  shows another input: X-HD Redux tracks the broadcast `<Active Source>`, `<Routing Change>`,
  `<Routing Information>` and `<Set Stream Path>` and is active only while they name its own
  physical address (assumed active until one says otherwise). An input change that sends no
  CEC goes unseen. A `<Set Stream Path>` to its address is answered with `<Active Source>`.
  Also skipped when the TV started it (below): the TV is already off.
- TV standby (`READ_LIVE_FLAGS` bit 2): set when `<Standby>` arrives from the TV (logical
  address 0, broadcast or to this device; not from other devices), for a BIOS that shuts the
  console down with the TV. Input changes send other messages and never set it. It stays set
  until the TV shows it is on again -- any other message from the TV, or HPD or Monitor Sense
  rising -- or the BIOS clears it (`WRITE_CEC_RX_ACK` = 2). RAM only: clear at every power-on.
  A BIOS should check it again a few seconds later before shutting down, so a TV that is back
  on (the bit cleared) doesn't turn the console off.

**SPD name** (the TV's input label, also what triggers Game Mode on many TVs; the product part is also the CEC OSD name): 24 bytes,
printable ASCII, space-padded -- bytes 0-7 vendor (default `MSFT`), 8-23 product (default `Xbox`).
Not configurable: to a TV whose EDID lists HDMI 2.1 ALLM (Auto Low Latency Mode), X-HD Redux
also sends the HDMI Forum VSIF with ALLM on, which newer TVs take as "switch to Game Mode".

- Read: `WRITE_SPD_APPLY` = 0 (resets the cursors), then 24 x `READ_SPD_DATA`.
- Write: `WRITE_SPD_APPLY` = 0, 24 x `WRITE_SPD_DATA`, `WRITE_SPD_APPLY` = 1. Rejected (old name
  kept) unless all 24 bytes were staged and every byte is printable. Persist with
  `WRITE_CONFIG_ACTION` = 1.

## 8. Telemetry (`READ_TELEMETRY`, 15)

Diagnostics with no other source. 20 bytes, version 1, refreshed every ~30-40 ms. Write
`161 = 0`, then read all 20 bytes in a row (`struct_size` lets you check); the cursor wraps.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `pixel_clock_hz`: measured input clock, 0 while re-acquiring |
| 4 | 1 | `detected_input_vic`: ADV7511 line detector (0x3E) |
| 5 | 1 | `arb_source`: which level owns the mode -- 0 hold, 1 BIOS packet, 2 BIOS pending, 3 Path A, 4 line detector, 5 width correction, 6 blind fallback |
| 6 | 1 | `arb_target_vic`: arbiter's target (internal) VIC |
| 7 | 1 | `probed_encoder`: `0x00` unknown, `0x8A`, `0xD4`, `0xE0` |
| 8 | 1 | `hw_flags`: 0x01 HPD, 0x02 Monitor Sense, 0x08 video ungated, 0x10 audio on, 0x20 freeze, 0x80 PLL lock (0x04, 0x40 reserved) |
| 9 | 1 | `fault_code` -- reserved, always 0 (no faults are reported yet) |
| 10 | 2 | `uptime_s` (saturates at 65535) |
| 12 | 2 | `mode_change_count`: applied mode changes since boot (saturates) |
| 14 | 1 | `pll_loss_count` (saturates at 255) |
| 15 | 1 | `gate_cycle_count`: TMDS / input-gate cycles (saturates) |
| 16 | 1 | `blind_event_count`: line detector went blind (saturates) |
| 17 | 1 | `struct_version` (1) |
| 18 | 1 | `struct_size` (20) |
| 19 | 1 | reserved |

## 9. Video info (`WRITE_VIDEO_INFO_LATCH` 151, `READ_VIDEO_INFO` 22)

What the console sends and what X-HD Redux put on the HDMI output -- for an on-screen overlay or a
dashboard. 24 bytes, version 1, refreshed every ~30-40 ms.

**Read it like this:** write `151 = 1` (takes a consistent snapshot and resets the cursor), then
read `22` up to 24 times. A mode change in the middle of your read can't mix two modes. To poll
cheaply, latch and read only bytes 0-2, and read the rest only when `change_count` moves.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `struct_version` (1) -- check it before trusting the layout |
| 1 | 1 | `struct_size` (24) |
| 2 | 1 | `change_count`: bumps on every applied mode change (low byte, wraps) |
| 3 | 1 | `flags`: 0x01 interlaced, 0x02 16:9, 0x04 anamorphic (16:9 on a 640-wide raster), 0x08 mode from a BIOS packet (else standalone autodetect), 0x10 HDMI sink (else DVI), 0x20 a mode is applied, 0x40 input clock locked, 0x80 wire-VIC mode chip-auto (else honest) |
| 4 | 2 | `width`: applied raster |
| 6 | 2 | `height`: applied raster, full frame (1080 for 1080i) |
| 8 | 4 | `pixel_clock_hz`: measured input clock (Hz), 0 while re-acquiring |
| 12 | 2 | `refresh_mhz`: exact refresh in milli-Hz (59940 = 59.940 Hz), from the measured clock; field rate for 1080i; 0 = unknown |
| 14 | 1 | `wire_vic`: the VIC the TV is told. 0 for a raster that isn't a CEA format (e.g. 640x480 at 24.5 MHz) |
| 15 | 1 | `internal_vic`: the CEA family X-HD Redux built the timing from |
| 16 | 1 | `detected_vic`: the console's raster as the ADV7511 line detector sees it |
| 17 | 1 | `console_mode_index`: mode index of the console's last mode packet, `0xFF` = none (standalone) |
| 18 | 1 | `color_space`: what the last apply programmed, i.e. on the wire -- 1 RGB full, 2 RGB limited, 3 YCbCr passthrough, 4 RGB passthrough, 6 YCbCr through the CSC. RGB output = 1, 2 or 4. A setting change (`WRITE_COLOR_SPACE`) shows here once its reapply lands |
| 19 | 1 | `pos_h`: picture position, horizontal offset in pixels (signed) for the active mode |
| 20 | 1 | `pos_v`: picture position, vertical offset in lines (signed) |
| 21 | 3 | reserved |

Fields 4-7, 12-15 and 18-20 are only meaningful while flag 0x20 is set.

## 10. Firmware update

The two images rewrite each other, never themselves:

| Running | `WRITE_RAM_APPLY` accepts | Used by |
|---|---|---|
| Bootloader | pages 20-63 (the application) | normal update |
| Application | pages 0-19 (the bootloader) | HD+ -> X-HD conversion |

Flash pages are 1 KB; the image is the combined `firmware.bin` (bootloader in its first 20 KB).
The sequence Team-Resurgent's XHDUpdater uses (its `HDHelper`):

1. `READ_MODE`; if `2`, `WRITE_SET_MODE` = 1 and wait for `READ_MODE` = 1.
2. `WRITE_RAM_BANK` = 0, `WRITE_APP_FLASH_MODE` = 1 (marks the application invalid until done).
3. For each page: `WRITE_RAM_BANK` = 0, 1024 x `WRITE_RAM`, `WRITE_RAM_APPLY` = page; wait ~1 s;
   `WRITE_READ_PAGE` = page; wait ~250 ms; read `READ_RAM_PAGE_CRC1-4` (MSB first) and compare
   with the standard CRC-32 (poly 0xEDB88320) of the 1024 bytes; repeat the page if it differs.
4. `WRITE_APP_FLASH_MODE` = 0 (clears the invalid flag, keeps the saved settings in page 63).
5. `WRITE_SET_MODE` = 2.

A board left in the bootloader by an interrupted update still answers `READ_VERSION1-4` and
`READ_MODE` = 1, so the same sequence (from step 2) recovers it.

**`READ_FLASH_STATUS` (26)** replaces step 3's first wait. After `WRITE_RAM_APPLY`, read it until
it isn't `1`: `2` means the page was written and read back with the buffer's CRC, `3` that the
page was outside the range above or didn't verify. The bootloader writes the page inside the
SMBus interrupt, so it only ever answers `2` or `3`; the application queues the write for its
main loop and answers `1` until then. While the application has a write queued it ignores
`WRITE_READ_PAGE`, and the next page's `WRITE_RAM` bytes must wait for the status to leave `1`
(they would overwrite the buffer being written). Firmware without the opcode answers `0x42`
(application) or `0xFF` (bootloader): fall back to the fixed waits.

## 11. About block (`READ_ABOUT` 23, `WRITE_ABOUT_INDEX` 155)

What `tools/xhd.py info` shows: which board and which build. 60 bytes, version 1, fixed after
boot (the chip fields fill in once the ADV7511 is first configured). Write `155 = 0`, then read
`23` up to 60 times; `155 = 16` jumps straight to the git hash.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `struct_version` (1) |
| 1 | 1 | `struct_size` (60) |
| 2 | 1 | `chip_id`: ADV7511 register 0x00 (0 until the chip is configured) |
| 3 | 1 | `chip_rev`: ADV7511 register 0xF5 |
| 4 | 12 | `mcu_uid`: STM32 96-bit unique ID, as stored in system memory (0x1FFFF7AC) |
| 16 | 24 | `git`: git hash of the build, ASCII, NUL-padded (e.g. `b349db5e-dirty-c1d75bd8`) |
| 40 | 20 | `built`: build date/time `YYYY-MM-DDTHH:MM:SS`, ASCII, NUL-padded |

The 4-byte firmware version (opcodes 1-4) is separate: XHDUpdater uses it to detect the board
and only prints it, so it can change freely as long as it isn't `FF FF FF FF`.
