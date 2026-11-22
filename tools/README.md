# xhd.py

A command-line tool for an X-HD Redux board's debug UART and for flashing it. One file, no
configuration; the serial commands need [pyserial](https://pypi.org/project/pyserial/)
(`pip install pyserial`), flashing needs `st-flash` (stlink-tools).

```
xhd.py ping                 is the board's command line answering?
xhd.py info                 firmware version and build, ADV7511 chip, MCU identity, encoder
xhd.py status               what the board is sending to the TV, and its health counters
xhd.py dump                 the raw ADV7511 register dump
xhd.py reg 3E               read one ADV7511 register (hex)
xhd.py sb r 16 24           SMBus bridge: 24 reads of opcode 0x16 (opcode hex, count decimal)
xhd.py sb w 90 01           SMBus bridge: write 0x01 to opcode 0x90 (opcode and values hex)
xhd.py enc [x|c|f]          show, or force, the video encoder (Xcalibur, Conexant, Focus)
xhd.py reset                reset the board
xhd.py bootloader           restart the board in its update mode
xhd.py log [seconds]        watch the board's log lines (Ctrl-C to stop)
xhd.py flash [image]        write a combined firmware image with an ST-Link
xhd.py update-uart [image]  recover the application over the UART, no ST-Link needed
```

The port is `--port`, or `$XHD_PORT`, or `/dev/ttyUSB0` (`COM3` on Windows), at 115200 baud:
`xhd.py --port /dev/ttyUSB1 status`. The board's UART is 3.3 V logic: use a USB-serial adapter
for that voltage. Every command exits 0 on success and 1 with a message on failure.

Only one program can use the port at a time, so close any terminal before running a command.

## What the commands do

- **status** reads the video info and telemetry blocks over the SMBus bridge
  ([`doc/SMBUS.md`](../doc/SMBUS.md) sections 8 and 9): the raster, refresh and pixel clock, the
  VIC the TV is told, the colour format, and the link, encoder and arbiter state with the
  board's counters. It works while the console is running.
- **info** reads the firmware version (opcodes 1-4) and the about block (section 11).
- **sb** is the SMBus opcode set itself: every setting the config app changes can be read or
  written here. Cursor opcodes (EDID, the structures above) need their `WRITE_*_INDEX` opcode
  written first; see the cursor table in `doc/SMBUS.md`. A write is not saved to flash until
  `sb w 8E 01` (`WRITE_CONFIG_ACTION` = 1).
- **flash** erases the settings page (so the board starts from defaults and re-detects its
  encoder; `--keep-settings` skips that), then writes the combined image at `0x08000000`. It
  refuses an application-only image, which would overwrite the bootloader. The image defaults
  to `.pio/build/combined_stm32f0/firmware.bin`.
- **update-uart** rewrites only the application, through the bootloader, with a CRC on every
  1 KB page and on the whole image. It is the recovery path for a board whose application will
  not start and that you have no programmer for. It asks a running application to restart in
  its update mode first; a board already waiting in the bootloader just continues.
  `--keep-settings` leaves the settings page alone. It cannot rewrite the bootloader.
- **log** prints the board's lines with a timestamp. The firmware sends most log lines as a
  short code and its arguments to save flash; the tool expands them from the comments in
  `src/shared/log_codes.h`, so run it from a checkout. Lines it cannot expand are shown as
  they arrived.

## The UART protocol

For writing your own client. 115200 baud, 8N1, no flow control. Keep DTR and RTS low: on some
USB adapters they are wired to the board's reset.

- A command is a line of text. Append ` *CC`, the CRC-8 (polynomial 0x07, initial value 0) of the
  text before it as two hex digits. The board refuses a line whose checksum does not match
  (`ERR: crc`) and refuses `sb` lines that have none, because bytes are sometimes lost or
  flipped on this UART while the console is changing video mode.
- Send a newline before the command as well. The UART's transmit side leaks into its receive
  side, and the stray bytes would otherwise be glued onto the front of the command.
- A command is answered with its output and then the prompt `xhd> ` (no newline after it).
  The prompt is the only reliable end of a reply; a log line can be printed right behind it.
- `sb r <op> [n]` reads opcode `<op>` `n` times and answers `[SB] <hex bytes> *CC` (up to 64
  bytes a line, checksum over the hex digits); `sb w <op> <v> [<v> ...]` writes each value and
  answers `[SB] ok *CC`. `<op>` and values are hex, `n` is decimal.
- Commands that change nothing (`ping`, `d`, `enc`, `r`) are safe to send again if no reply
  came. Writes are not: a lost reply may mean the write already ran.
- Other commands: `ping` (answers `PONG`), `d` (register dump), `r <reg>` (answers
  `R <reg>=<value> *CC`), `enc [0|1|2]`, `rst`.
- The bootloader's UART update (`update-uart`) is a separate binary protocol, described in
  `src/bootloader/bl_uart_update.h`.
