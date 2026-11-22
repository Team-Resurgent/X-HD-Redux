#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""xhd: talk to an X-HD Redux board over its debug UART, and flash it.

    xhd.py ping                 is the board's command line answering?
    xhd.py info                 firmware version and build, ADV7511 chip, MCU identity, encoder
    xhd.py status               what the board is sending to the TV, and its health counters
    xhd.py dump                 the raw ADV7511 register dump (the board's `d` command)
    xhd.py reg 3E               read one ADV7511 register (hex)
    xhd.py sb r 16 24           SMBus bridge: 24 reads of opcode 0x16 (opcode hex, count decimal)
    xhd.py sb w 90 01           SMBus bridge: write 0x01 to opcode 0x90 (opcode and values hex)
    xhd.py enc [x|c|f]          show, or force, the video encoder (Xcalibur, Conexant, Focus)
    xhd.py reset                reset the board
    xhd.py bootloader           restart the board in its update mode
    xhd.py log [seconds]        watch the board's log lines until Ctrl-C (or for N seconds)
    xhd.py flash [image]        write a combined firmware image with an ST-Link (needs st-flash)
    xhd.py update-uart [image]  recover the application over the UART, no ST-Link needed

The SMBus opcodes `sb` takes are listed in doc/SMBUS.md. The port is --port, or $XHD_PORT, or
/dev/ttyUSB0 (COM3 on Windows); the board talks 115200 baud. The serial commands need pyserial
(pip install pyserial). Every command exits 0 on success and 1 on failure.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

try:
    import serial
except ImportError:
    serial = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = "COM3" if os.name == "nt" else "/dev/ttyUSB0"
BAUD = 115200
PROMPT = "xhd>"

# SMBus opcodes (src/shared/defines.h, doc/SMBUS.md).
OP_VERSION = (0x01, 0x02, 0x03, 0x04)
OP_TELEMETRY, OP_VIDEO_INFO, OP_ABOUT = 0x0F, 0x16, 0x17
OP_SET_MODE = 0x84
OP_VIDEO_INFO_LATCH, OP_ABOUT_INDEX, OP_TELEMETRY_INDEX = 0x97, 0x9B, 0xA1
VIDEO_INFO_SIZE, TELEMETRY_SIZE, ABOUT_SIZE = 24, 20, 60

# Flash layout (doc/SMBUS.md section 10): bootloader pages 0-19, application 20-62, settings 63.
FLASH_BASE = 0x08000000
APP_OFFSET = 0x5000
SETTINGS_ADDR = 0x0800FC00
SETTINGS_SIZE = 0x400
APP_MAX = SETTINGS_ADDR - (FLASH_BASE + APP_OFFSET)     # 44,032 bytes
PAGE = 1024

ENCODERS = {0x8A: "Conexant", 0xD4: "Focus", 0xE0: "Xcalibur"}
ENCODER_ARG = {"x": "0", "xcal": "0", "xcalibur": "0", "c": "1", "conexant": "1",
               "f": "2", "focus": "2", "0": "0", "1": "1", "2": "2"}
ARB_SOURCE = ["hold", "BIOS packet", "BIOS pending", "Path A", "line detector",
              "width correction", "blind fallback"]
COLOR_SPACE = {1: "RGB full", 2: "RGB limited", 3: "YCbCr passthrough", 4: "RGB passthrough",
               6: "YCbCr through the CSC"}
VIC_NAME = {0: "none (non-CEA raster)", 1: "640x480p", 2: "720x480p 4:3", 3: "720x480p 16:9",
            4: "1280x720p", 5: "1920x1080i", 17: "720x576p 4:3", 18: "720x576p 16:9",
            21: "576i 4:3", 22: "576i 16:9"}


class XhdError(Exception):
    """A failure to report to the user, not a bug."""


def crc8(text):
    """CRC-8 (polynomial 0x07, initial value 0) over the text; the board's line check."""
    crc = 0
    for byte in text.encode("latin-1", "replace"):
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def checked(line, prefix):
    """The payload of a reply line "<prefix><payload> *CC", or None if the line is not one or
    its checksum fails (a byte was lost or flipped on the UART)."""
    line = line.strip()
    if not line.startswith(prefix) or len(line) < len(prefix) + 4 or line[-4:-2] != " *":
        return None
    payload = line[len(prefix):-4]
    try:
        return payload if crc8(payload) == int(line[-2:], 16) else None
    except ValueError:
        return None


def open_port(port, baud=BAUD):
    if serial is None:
        raise XhdError("pyserial is not installed (pip install pyserial)")
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, baud, 0.05
    ser.dsrdtr = ser.rtscts = ser.xonxoff = False
    # Some USB adapters wire DTR or RTS to the board's reset: keep both low from the start.
    ser.dtr = ser.rts = False
    try:
        ser.open()
    except (OSError, serial.SerialException) as e:
        raise XhdError(str(e))
    return ser


class Link:
    """The board's command line: one command at a time, each answered by a prompt."""

    END_QUIET = 0.08    # silence after a prompt at the very end of the output
    MID_QUIET = 0.3     # silence after a prompt that has more text behind it

    def __init__(self, ser):
        self.ser = ser

    def command(self, text, timeout=4.0, expect=None, retry=False):
        """Send `text` and return the reply as a list of lines, prompt removed. With `retry`
        (only for commands that change nothing), a missing or wrong reply is asked for once
        more: bytes can be lost on this UART while the console is changing video mode. `expect`
        is a string every good reply contains."""
        def good(reply, complete):
            return complete and (expect is None or any(expect in l for l in reply))

        reply, complete = self._exchange(text, 2.0 if retry else timeout)
        if retry and not good(reply, complete):
            reply, complete = self._exchange(text, timeout)
        if not complete:
            raise XhdError("no reply to '%s' (is this the board's debug UART?)" % text)
        if not good(reply, complete):
            raise XhdError("unexpected reply to '%s': %s" % (text, " | ".join(reply)))
        return reply

    def send(self, text):
        """Send a command without waiting for its reply (for the ones that reset the board)."""
        self.ser.write(("\n%s *%02X\n" % (text, crc8(text))).encode("ascii", "replace"))
        self.ser.flush()

    def _exchange(self, text, timeout):
        while self.ser.in_waiting:          # board output that arrived between commands
            self.ser.read(self.ser.in_waiting)
        # The leading newline ends any stray bytes the UART's TX-to-RX crosstalk left in the
        # board's line buffer, which would otherwise be glued onto the front of this command.
        self.send(text)
        got, last_rx, end = "", time.time(), time.time() + timeout
        complete = False
        while time.time() < end:
            n = self.ser.in_waiting
            if n:
                got += self.ser.read(n).decode("utf-8", "replace")
                last_rx = time.time()
                continue
            quiet = time.time() - last_rx
            # The prompt has no newline after it, so a log line printed right behind it shares
            # its line: it ends the reply either way, only the wait differs.
            if (got.rstrip().endswith(PROMPT) and quiet >= self.END_QUIET) or \
               ("\n" + PROMPT in got and quiet >= self.MID_QUIET):
                complete = True
                break
            time.sleep(0.01)
        cut = got.rfind(PROMPT)
        body = got[:cut] if cut >= 0 else got
        return [l.strip() for l in body.splitlines() if l.strip()], complete


class Bridge:
    """The SMBus opcodes, through the board's `sb` command (doc/SMBUS.md)."""

    def __init__(self, link):
        self.link = link

    def read(self, op, n=1):
        """`n` reads of read-opcode `op` as bytes, or None if the reply is short or its checksum
        fails. Not repeated here: opcodes that step a cursor must be re-positioned first."""
        hexdata = ""
        for line in self.link.command("sb r %x %d" % (op, n)):
            if line.startswith("[SB] "):
                payload = checked(line, "[SB] ")
                if payload is None:
                    return None
                hexdata += payload
        try:
            data = bytes.fromhex(hexdata)
        except ValueError:
            return None
        return data if len(data) == n else None

    def read_byte(self, op):
        """One byte of a single-value opcode, read twice at most. Not for cursor opcodes."""
        for _ in range(2):
            data = self.read(op)
            if data:
                return data[0]
        return None

    def write(self, op, *values):
        """True once the board confirmed the write. A refused line ("ERR: crc") is sent once
        more; a lost reply is not, because the write may already have run."""
        text = "sb w %x %s" % (op, " ".join("%x" % (v & 0xFF) for v in values))
        for _ in range(2):
            reply = self.link.command(text)
            if any(checked(l, "[SB] ") == "ok" for l in reply):
                return True
            if "ERR: crc" not in reply:
                return False
        return False

    def stream(self, seek_op, seek_value, read_op, n):
        """Position a read cursor, then read `n` bytes; once more from a fresh seek if the first
        try came back short."""
        for _ in range(2):
            if self.write(seek_op, seek_value):
                data = self.read(read_op, n)
                if data:
                    return data
        return None


# ---------------------------------------------------------------- decoding

def mhz(hz):
    return "%d.%03d MHz" % (hz // 1000000, hz % 1000000 // 1000)


def decode_video_info(d):
    """The 24-byte video info block (doc/SMBUS.md section 9) as text lines."""
    if d[0] != 1 or d[1] != VIDEO_INFO_SIZE:
        return ["video info layout v%d (%d bytes) is not understood by this tool" % (d[0], d[1])]
    flags = d[3]
    out = []
    if flags & 0x20:
        width, height = struct.unpack_from("<HH", d, 4)
        clock, refresh = struct.unpack_from("<IH", d, 8)
        aspect = "16:9" if flags & 0x02 else "4:3"
        if flags & 0x04:
            aspect += " (anamorphic)"
        out.append("Output:          %dx%d %s, %s%s" % (
            width, height, "interlaced" if flags & 0x01 else "progressive", aspect,
            ", %d.%03d Hz" % (refresh // 1000, refresh % 1000) if refresh else ""))
        out.append("Pixel clock:     %s%s" % (mhz(clock) if clock else "still measuring",
                                             "" if flags & 0x40 else " (input clock not locked)"))
        out.append("VIC to the TV:   %d (%s); timing built from VIC %d; line detector reads VIC %d" % (
            d[14], VIC_NAME.get(d[14], "VIC %d" % d[14]), d[15], d[16]))
        out.append("Colour:          %s" % COLOR_SPACE.get(d[18], "unknown (%d)" % d[18]))
        out.append("Picture shift:   %+d px horizontal, %+d lines vertical" % (
            struct.unpack_from("<bb", d, 19)))
    else:
        out.append("Output:          no mode applied yet")
    out.append("Mode source:     %s" % ("BIOS packet" if flags & 0x08 else "standalone detection"))
    out.append("Console mode:    %s" % ("none" if d[17] == 0xFF else "index 0x%02X" % d[17]))
    out.append("Sink:            %s" % ("HDMI" if flags & 0x10 else "DVI"))
    out.append("Wire-VIC mode:   %s" % ("chip auto" if flags & 0x80 else "honest"))
    return out


def decode_telemetry(d):
    """The 20-byte telemetry block (doc/SMBUS.md section 8) as text lines."""
    if d[17] != 1 or d[18] != TELEMETRY_SIZE:
        return ["telemetry layout v%d (%d bytes) is not understood by this tool" % (d[17], d[18])]
    flags = d[8]
    on = lambda bit: "yes" if flags & bit else "no"
    uptime, changes = struct.unpack_from("<HH", d, 10)
    source = d[5]
    return [
        "Link:            HPD %s, monitor sense %s, video %s, audio %s, PLL lock %s%s" % (
            on(0x01), on(0x02), "on" if flags & 0x08 else "gated", on(0x10), on(0x80),
            ", console packets IGNORED (freeze)" if flags & 0x20 else ""),
        "Encoder:         %s" % ENCODERS.get(d[7], "not identified yet (probed)"),
        "Decision:        %s, target VIC %d, detector reads VIC %d" % (
            ARB_SOURCE[source] if source < len(ARB_SOURCE) else "source %d" % source, d[6], d[4]),
        "Uptime:          %d s; %d mode changes, %d PLL losses, %d gate cycles, %d detector blind events" % (
            uptime, changes, d[14], d[15], d[16]),
    ]


def decode_about(a):
    """(chip id, chip revision, MCU unique ID, git hash, build time) from the 60-byte block."""
    uid = "".join("%08X" % int.from_bytes(a[4 + 4 * i:8 + 4 * i], "little") for i in range(3))
    text = lambda b: b.split(b"\0")[0].decode("ascii", "replace")
    return a[2], a[3], uid, text(a[16:40]), text(a[40:60])


# ---------------------------------------------------------------- log codes

def load_log_codes(path=None):
    """Map "~XX" -> printf template from the firmware's log_codes.h. The board sends a code
    and its arguments instead of text; the header's trailing comment is the text."""
    path = path or os.path.join(ROOT, "src", "shared", "log_codes.h")
    codes = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r'#define LOG_\w+\s+"(~[0-9A-F]{2})[^"]*"\s*//\s*(.*?)\s*$', line)
                if m:
                    codes[m.group(1)] = m.group(2)
    except OSError:
        pass
    return codes


_SPEC = re.compile(r"%([-0 #+]*\d*)(?:hh|h|ll|l|z)?([diuxXcs%])")


def expand_log(line, codes):
    """A coded log line as text; any line it cannot expand is returned unchanged."""
    if not line.startswith("~"):
        return line
    key, *values = line.split("\t")
    template = codes.get(key)
    if template is None:
        return line
    args = iter(values)

    def fill(m):
        flags, conv = m.groups()
        if conv == "%":
            return "%"
        arg = next(args, None)
        if arg is None:
            raise ValueError("the line has fewer arguments than its template")
        if conv in "xX":
            return ("%" + flags + conv) % int(arg, 16)
        if conv in "diu":
            return ("%" + flags + "d") % int(arg)
        return ("%" + flags + conv) % (arg[:1] if conv == "c" else arg)

    try:
        text = _SPEC.sub(fill, template)
    except (ValueError, TypeError):
        return line
    return line if next(args, None) is not None else text   # a spare argument: stale table


# ---------------------------------------------------------------- commands

def cmd_ping(link, args):
    link.command("ping", expect="PONG", retry=True)
    print("PONG")


def cmd_info(link, args):
    bridge = Bridge(link)
    about = bridge.stream(OP_ABOUT_INDEX, 0, OP_ABOUT, ABOUT_SIZE)
    if not about:
        raise XhdError("could not read the about block (is the application running?)")
    chip, rev, uid, git, built = decode_about(about)
    version = [bridge.read_byte(op) for op in OP_VERSION]
    print("Firmware:   %s" % ("unknown" if None in version else
                              ".".join(str(v) for v in (version[1:] if version[0] == 0 else version))))
    print("Build:      %s, built %s" % (git, built))
    print("ADV7511:    chip 0x%02X, revision 0x%02X" % (chip, rev))
    print("MCU ID:     %s" % uid)
    for line in link.command("enc", retry=True, expect="[ENC]"):
        if line.startswith("[ENC]"):
            print("Encoder:    %s" % line[len("[ENC]"):].strip())


def cmd_status(link, args):
    bridge = Bridge(link)
    video = bridge.stream(OP_VIDEO_INFO_LATCH, 1, OP_VIDEO_INFO, VIDEO_INFO_SIZE)
    health = bridge.stream(OP_TELEMETRY_INDEX, 0, OP_TELEMETRY, TELEMETRY_SIZE)
    if not video or not health:
        raise XhdError("could not read the board's status (is the application running?)")
    print("\n".join(decode_video_info(video) + decode_telemetry(health)))


def cmd_dump(link, args):
    print("\n".join(link.command("d", retry=True, expect="[DUMP]")))


def cmd_reg(link, args):
    try:
        reg = int(args.register, 16)
    except ValueError:
        raise XhdError("the register is a hex number, e.g. 3E")
    for _ in range(2):                    # reading a register changes nothing: ask again if lost
        for line in link.command("r %02X" % reg):
            m = re.fullmatch(r"([0-9A-Fa-f]+)=([0-9A-Fa-f]+)", checked(line, "R ") or "")
            if m and int(m.group(1), 16) == reg:
                print("0x%02X = 0x%s" % (reg, m.group(2).upper()))
                return
    raise XhdError("no valid answer for register 0x%02X" % reg)


def cmd_sb(link, args):
    bridge = Bridge(link)
    try:
        op = int(args.opcode, 16)
        if args.direction == "r":
            data = bridge.read(op, int(args.values[0]) if args.values else 1)
            if data is None:
                raise XhdError("the read failed or its reply was damaged")
            print(data.hex().upper())
        else:
            if not args.values:
                raise XhdError("sb w needs at least one hex value")
            if not bridge.write(op, *[int(v, 16) for v in args.values]):
                raise XhdError("the write was not confirmed")
            print("ok")
    except ValueError:
        raise XhdError("opcode and write values are hex; the read count is decimal")


def cmd_enc(link, args):
    text = "enc"
    if args.encoder:
        choice = ENCODER_ARG.get(args.encoder.lower())
        if choice is None:
            raise XhdError("encoder is x (Xcalibur), c (Conexant) or f (Focus)")
        text += " " + choice
    for line in link.command(text):
        if line.startswith("[ENC]"):
            print(line[len("[ENC]"):].strip())


def cmd_reset(link, args):
    link.send("rst")
    print("reset requested")


def request_bootloader(link):
    """Ask a running application to restart in its update mode. The board resets about 10 ms
    after the write, so no reply is waited for."""
    link.send("sb w %x 1" % OP_SET_MODE)


def cmd_bootloader(link, args):
    request_bootloader(link)
    print("update mode requested")


def cmd_log(link, args):
    codes = load_log_codes()
    end = time.time() + args.seconds if args.seconds else None
    pending = ""
    try:
        while end is None or time.time() < end:
            data = link.ser.read(512)
            if not data:
                continue
            pending += data.decode("utf-8", "replace")
            while "\n" in pending:
                line, pending = pending.split("\n", 1)
                line = line.strip("\r ")
                if line.startswith(PROMPT):         # a log line printed right behind the prompt
                    line = line[len(PROMPT):].lstrip()
                line = expand_log(line, codes)
                if line:
                    now = time.time()
                    print("%s.%03d %s" % (time.strftime("%H:%M:%S", time.localtime(now)),
                                          int(now * 1000) % 1000, line))
                    sys.stdout.flush()
    except KeyboardInterrupt:
        pass


def reset_vector(data, offset):
    """The reset handler address of the vector table at `offset` (Thumb bit cleared), or 0."""
    return int.from_bytes(data[offset + 4:offset + 8], "little") & ~1 if len(data) >= offset + 8 else 0


def looks_combined(data):
    """True for a bootloader + application image: both reset vectors point into their region."""
    return (FLASH_BASE <= reset_vector(data, 0) < FLASH_BASE + APP_OFFSET and
            FLASH_BASE + APP_OFFSET <= reset_vector(data, APP_OFFSET) < SETTINGS_ADDR)


def default_image():
    for candidate in (os.path.join(ROOT, ".pio", "build", "combined_stm32f0", "firmware.bin"),
                      os.path.join(ROOT, "bin", "firmware.bin")):
        if os.path.exists(candidate):
            return candidate
    return None


def read_image(path):
    path = path or default_image()
    if not path or not os.path.exists(path):
        raise XhdError("no firmware image: give a path, or build one first (pio run -e combined_stm32f0)")
    with open(path, "rb") as f:
        return path, f.read()


def cmd_flash(args):
    path, data = read_image(args.image)
    if not looks_combined(data) or len(data) > SETTINGS_ADDR - FLASH_BASE:
        raise XhdError("%s is not a combined bootloader + application image of at most %d bytes "
                       "(writing an application-only image at 0x08000000 would erase the "
                       "bootloader)" % (path, SETTINGS_ADDR - FLASH_BASE))
    if not shutil.which("st-flash"):
        raise XhdError("st-flash not found (install stlink-tools)")
    if not args.keep_settings:
        # A plain write leaves the settings page alone; erase it so the board starts from
        # defaults and re-detects its encoder.
        print("erasing the settings page...")
        if subprocess.run(["st-flash", "erase", "0x%08X" % SETTINGS_ADDR,
                           "0x%X" % SETTINGS_SIZE]).returncode:
            raise XhdError("erasing the settings page failed; nothing was written")
    print("writing %s..." % path)
    if subprocess.run(["st-flash", "--reset", "write", path, "0x%08X" % FLASH_BASE]).returncode:
        raise XhdError("st-flash failed")
    print("flashed; the board was reset")


def application_part(data):
    """The application slice of a combined image or of an application-only image."""
    low, high = FLASH_BASE + APP_OFFSET, SETTINGS_ADDR
    if low <= reset_vector(data, APP_OFFSET) < high:
        return data[APP_OFFSET:]
    if low <= reset_vector(data, 0) < high:
        return data
    raise XhdError("not an X-HD application or combined image (reset vector outside 0x%08X-0x%08X)"
                   % (low, high))


class UartUpdater:
    """The bootloader's update protocol over the debug UART (src/bootloader/bl_uart_update.h):
    a hello frame, then 1 KB pages with a CRC each, stop-and-wait, then a verdict."""

    REPLY_TIMEOUT = 8.0     # seconds to wait for the answer to one frame
    DONE_TIMEOUT = 10.0     # and for the verdict on the whole image

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def reply(self, want="XHD", timeout=None):
        """The next "XHD..." line starting with `want`, or None. Other lines are the
        bootloader's own log and are shown dimly."""
        end = time.time() + (timeout or self.REPLY_TIMEOUT)
        while True:
            while b"\n" in self.buf:
                i = self.buf.index(b"\n")
                line = bytes(self.buf[:i]).decode("utf-8", "replace").strip()
                del self.buf[:i + 1]
                if line.startswith("XHD"):
                    if line.startswith(want):
                        return line
                elif line:
                    print("      . %s" % line)
            if time.time() >= end:
                return None
            self.buf.extend(self.ser.read(512))

    def run(self, app, keep_settings):
        image_crc = zlib.crc32(app) & 0xFFFFFFFF
        pages = (len(app) + PAGE - 1) // PAGE
        print("application: %d bytes, %d pages, crc32 %08X (%s the settings page)" % (
            len(app), pages, image_crc, "keeping" if keep_settings else "erasing"))
        self.ser.reset_input_buffer()
        self.ser.write(b"XHDU" + struct.pack("<BBII", 3, 1 if keep_settings else 0, len(app), image_crc))
        self.ser.flush()
        hello = self.reply()
        if not hello or not hello.startswith("XHDK"):
            raise XhdError("the bootloader did not accept the transfer (%r); is the board in "
                           "its update mode? Try `xhd.py bootloader`" % hello)
        seq, naks = 0, 6 * pages + 16
        while seq < pages:
            chunk = app[seq * PAGE:(seq + 1) * PAGE].ljust(PAGE, b"\xFF")
            self.ser.write(b"\xA5\x5A" + struct.pack("<H", seq) + chunk +
                           struct.pack("<I", zlib.crc32(chunk) & 0xFFFFFFFF))
            self.ser.flush()
            answer = self.reply()
            time.sleep(0.008)       # let the board drain its own TX-to-RX crosstalk
            if answer and answer.startswith("XHDK"):
                acked = int(answer.split()[1])
                if acked >= seq:    # also covers a repeated ACK for an earlier page
                    seq = acked + 1
                    if seq % 4 == 0 or seq == pages:
                        print("    [%d/%d]" % (seq, pages))
                continue
            if answer and answer.startswith("XHDE"):
                raise XhdError("the bootloader aborted the transfer: %s" % answer)
            naks -= 1               # a NAK, or a lost or damaged reply: send the page again
            if naks <= 0:
                raise XhdError("too many failed pages, giving up (last reply %r)" % answer)
        done = self.reply(want="XHDD", timeout=self.DONE_TIMEOUT)
        if not done:
            raise XhdError("no verdict from the bootloader")
        status, crc = int(done.split()[1]), done.split()[2]
        if status:
            raise XhdError("whole-image check failed (the board computed %s); it stays in its "
                           "update mode: run the update again" % crc)
        print("verified (crc %s); the board is restarting into the new application" % crc)


def cmd_update_uart(args):
    path, data = read_image(args.image)
    app = application_part(data)
    if not 0 < len(app) <= APP_MAX:
        raise XhdError("the application is %d bytes; it must be 1 to %d" % (len(app), APP_MAX))
    ser = open_port(args.port)
    try:
        request_bootloader(Link(ser))    # harmless if the board is already in its update mode
        time.sleep(2.0)
        UartUpdater(ser).run(app, args.keep_settings)
    finally:
        ser.close()


# ---------------------------------------------------------------- main

SERIAL_COMMANDS = {"ping": cmd_ping, "info": cmd_info, "status": cmd_status, "dump": cmd_dump,
                   "reg": cmd_reg, "sb": cmd_sb, "enc": cmd_enc, "reset": cmd_reset,
                   "bootloader": cmd_bootloader, "log": cmd_log}


def build_parser():
    p = argparse.ArgumentParser(prog="xhd.py", description=__doc__.split("\n\n")[0])
    p.add_argument("--port", default=os.environ.get("XHD_PORT", DEFAULT_PORT),
                   help="serial port (default: $XHD_PORT or %s)" % DEFAULT_PORT)
    sub = p.add_subparsers(dest="command", required=True)
    for name in ("ping", "info", "status", "dump", "reset", "bootloader"):
        sub.add_parser(name)
    reg = sub.add_parser("reg")
    reg.add_argument("register", help="ADV7511 register, hex")
    sb = sub.add_parser("sb")
    sb.add_argument("direction", choices=("r", "w"))
    sb.add_argument("opcode", help="SMBus opcode, hex")
    sb.add_argument("values", nargs="*", help="read: count (decimal); write: values (hex)")
    enc = sub.add_parser("enc")
    enc.add_argument("encoder", nargs="?", help="x, c or f to force the encoder")
    log = sub.add_parser("log")
    log.add_argument("seconds", nargs="?", type=float, help="stop after this long")
    flash = sub.add_parser("flash")
    flash.add_argument("image", nargs="?")
    flash.add_argument("--keep-settings", action="store_true", help="leave the settings page alone")
    upd = sub.add_parser("update-uart")
    upd.add_argument("image", nargs="?")
    upd.add_argument("--keep-settings", action="store_true", help="keep the settings page")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.command == "flash":
            cmd_flash(args)
        elif args.command == "update-uart":
            cmd_update_uart(args)
        else:
            ser = open_port(args.port)
            try:
                SERIAL_COMMANDS[args.command](Link(ser), args)
            finally:
                ser.close()
    except XhdError as e:
        print("xhd: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
