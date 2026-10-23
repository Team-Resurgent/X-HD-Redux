// An Xbox debug-monitor extension (DXT) that draws an FPS and video resolution
// overlay using the NV2A PVIDEO hardware video overlay.
//
// Ported from RXDK-FPS (https://github.com/Team-Resurgent/RXDK-FPS), itself a
// port of JayFoxRox's xbox-fps-overlay. Unlike a title, a DXT is not an XBE: it
// is a raw flat PE loaded by xbdm.dll from E:\dxt at debug-monitor init and
// entered at DxtEntry. It runs in the system process, kernel-mode, so it calls
// the kernel (xboxkrnl.exe) and the debug monitor (xbdm.dll) directly -- no
// XAPI, no D3D, no CRT startup.
//
// Build: links libxbdm.lib (Dm* by ordinal) + libkernel.lib (xboxkrnl.exe by
// ordinal) + libc (sprintf/str*), then imagebld /DXT flattens it into a .dxt.
// Deploy copies it to E:\dxt and warm-reboots so xbdm loads it.
//
// NOTE: the FPS/resolution overlay only draws while a Direct3D title is running (it
// reads that title's "frames" performance counter) -- on the dashboard it stays
// idle. Launch a game after the reboot to see it.

#include <xboxkrnl/xboxkrnl.h>

// The kernel APIs (Mm*/Ps*/Hal*/Ke*/Dbg*) and KernelMode come from RXDK's own
// <xboxkrnl/*> headers above -- no hand-rolled "undocumented" declarations. xbdm.h
// still needs a little Win32 glue (WINAPI/SUCCEEDED/MAX_PATH/LPBOOL); we can't get
// it from <windef.h>/<winerror.h> here because those pull the mingw <winnt.h>,
// which collides with the standalone kernel types a DXT is built against.
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef LPBOOL
typedef BOOL *LPBOOL;
#endif

#include <xbdm.h>
#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "font.h"

#ifndef HalReadSMBusByte
#define HalReadSMBusByte(SlaveAddress, CommandCode, DataValue) \
    HalReadSMBusValue((SlaveAddress), (CommandCode), FALSE, (DataValue))
#endif

#ifndef HalWriteSMBusByte
#define HalWriteSMBusByte(SlaveAddress, CommandCode, DataValue) \
    HalWriteSMBusValue((SlaveAddress), (CommandCode), FALSE, (DataValue))
#endif

static LONGLONG lastFrame = 0;
static ULONGLONG lastFrameTime = 0;   // KeQueryInterruptTime (100 ns) of lastFrame

static inline void FlushCpuCache(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("wbinvd");
#else
    __asm { wbinvd }
#endif
}

// 720p/1080i vertical refresh is fixed per encoder (X-HD sweeps, measured clock over
// the CEA totals 1650x750 / 2200x1125): Conexant and Focus 60.00 Hz (74.250 MHz),
// Xcalibur 59.94 Hz (74.176 MHz). The kernel's mode dword doesn't say which, so the
// encoder is found by which address ACKs; probed once, until one answers.
static const char *s_hd_refresh = NULL;

static const char *HdRefresh(void)
{
    ULONG v;
    if (s_hd_refresh == NULL) {
        if (NT_SUCCESS(HalReadSMBusByte(0xE0, 0x00, &v))) {
            s_hd_refresh = "59.94Hz"; // Xcalibur
        } else if (NT_SUCCESS(HalReadSMBusByte(0x8A, 0x00, &v)) ||
                   NT_SUCCESS(HalReadSMBusByte(0xD4, 0x00, &v))) {
            s_hd_refresh = "60Hz";    // Conexant / Focus
        } else {
            return "0";
        }
    }
    return s_hd_refresh;
}

static void GetVideoFormat(int *width, int *height, const char **refresh, const char **aspect)
{
    unsigned long base = 0xFD000000 + NV_PRAMDAC;
    unsigned long w = (*(volatile unsigned long *)(base + NV_PRAMDAC_FP_HDISPLAY_END) & 0x0FFF) + 1;
    unsigned long h = (*(volatile unsigned long *)(base + NV_PRAMDAC_FP_VDISPLAY_END) & 0x0FFF) + 1;

    *width = (int)w;
    *height = (int)h;

    // The console's current AV mode dword (AvSendTVEncoderOption option 13, query mode).
    ULONG mode = 0;
    AvSendTVEncoderOption(NULL, 13, 0, &mode);

    // Refresh rate from the output type in the mode's top two bits:
    //   0x00000000 SD 60 Hz (NTSC, PAL-60)     -> 59.94 Hz
    //   0x40000000 SD 50 Hz (PAL, 480 or 576)  -> 50 Hz
    //   0x80000000 HDTV: 480p is 59.94 Hz, 720p/1080i depend on the encoder (HdRefresh)
    //   0xC0000000 VGA                         -> 60 Hz
    // Without a mode, 576 lines can only be PAL 50 Hz; anything else is shown as 0.
    if (mode != 0) {
        ULONG out_mode = mode & 0xC0000000;
        if (out_mode == 0x40000000) {
            *refresh = "50Hz";
        } else if (out_mode == 0x00000000) {
            *refresh = "59.94Hz";
        } else if (out_mode == 0x80000000) {
            *refresh = (h == 480) ? "59.94Hz" : HdRefresh();
        } else {
            *refresh = "60Hz";
        }
    } else if (h == 576) {
        *refresh = "50Hz";
    } else {
        *refresh = "0";
    }

    // Aspect ratio: HD modes (720p, 1080i) are 16:9. For SD, bit 28 (0x10000000) of the
    // mode dword is the widescreen (WSS) flag.
    if (w >= 1280) {
        *aspect = "16:9";
    } else if (mode != 0) {
        *aspect = (mode & 0x10000000) ? "16:9" : "4:3";
    } else {
        *aspect = "4:3";
    }
}

static int DrawText(unsigned char *framebuffer, int start_y, const char *text)
{
    int x = 0;
    int len = (int)strlen(text);
    for (int i = 0; i < len; i++) {
        int symbol, symbol_col, symbol_row, symbol_offset, y, dx;
        int gap = WIDTH - x;
        if (gap < 8) {
            break;
        }

        symbol = text[i] - ' ';
        if (symbol < 0 || symbol >= 96) {
            symbol = 0;
        }

        symbol_col = (symbol & 0xf);
        symbol_row = (symbol >> 4) & 0xf;
        symbol_offset = symbol_col + ((symbol_row << 4) * 13);

        for (y = 0; y < 13; y++) {
            unsigned char *fb_row = &framebuffer[((start_y + y) * PITCH) + (x << 1)];
            const unsigned char *font_row = &FontImageData[(y << 4) + symbol_offset];
            for (dx = 0; dx < 8; dx++) {
                fb_row[dx << 1] = ((font_row[0] >> (7 - dx)) & 0x1) * 0xFF;
            }
        }

        x += 8;
    }
    return x;
}

static XHD_VIDEO_INFO s_cached_vi = {0};
static BOOL s_cached_xhd_detected = FALSE;
static int s_poll_misses = 0;   // consecutive failed polls
static int s_poll_countdown = 0;

// Line 2 hides only after this many failed polls in a row, so one NACKed transaction
// doesn't blink it off and resize the overlay.
#define XHD_POLL_MISS_LIMIT 3
static unsigned char s_backbuffer[PITCH * MAX_HEIGHT];

static BOOL ReadXhdVideoInfo(XHD_VIDEO_INFO *outInfo)
{
    ULONG modeVal = 0;
    NTSTATUS status;

    // Check if an X-HD answers at 0xD2 in application mode (READ_MODE == 2)
    status = HalReadSMBusByte(XHD_SMBUS_ADDR, I2C_HDMI_COMMAND_READ_MODE, &modeVal);
    if (!NT_SUCCESS(status) || modeVal != 2) {
        return FALSE;
    }

    // Latch the current video info snapshot (WRITE 151 = 1)
    status = HalWriteSMBusByte(XHD_SMBUS_ADDR, I2C_HDMI_COMMAND_WRITE_VIDEO_INFO_LATCH, 1);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    // Stream the whole snapshot into a local copy; outInfo is only touched once all
    // 24 bytes arrived and the header checks out, so a failed read never leaves a
    // half-updated struct behind. With no mode applied (XHD_VI_APPLIED clear) the mode
    // fields are not valid and not checked -- the caller shows "no mode".
    XHD_VIDEO_INFO vi;
    unsigned char *p = (unsigned char *)&vi;
    for (int i = 0; i < (int)sizeof(vi); i++) {
        ULONG b = 0;
        status = HalReadSMBusByte(XHD_SMBUS_ADDR, I2C_HDMI_COMMAND_READ_VIDEO_INFO, &b);
        if (!NT_SUCCESS(status)) {
            return FALSE;
        }
        p[i] = (unsigned char)(b & 0xFF);
    }

    if (vi.struct_version != 1 || vi.struct_size != sizeof(XHD_VIDEO_INFO)) {
        return FALSE;
    }
    if ((vi.flags & XHD_VI_APPLIED) &&
        (vi.width < 320 || vi.width > 1920 || vi.height < 240 || vi.height > 1080 ||
         vi.color_space > 5)) {
        return FALSE;
    }

    *outInfo = vi;
    return TRUE;
}

static void UpdateFramebuffer(unsigned char *framebuffer)
{
    HANDLE h;
    DM_COUNTDATA count_data;
    LONGLONG frame, delta;
    char message[MAX_PATH];
    int i, width, height;
    int resWidth = 0, resHeight = 0;
    const char *refresh = "60Hz";
    const char *aspect = "4:3";
    unsigned long base;

    // No "frames" counter (no Direct3D title running) is the normal idle state, retried
    // every second -- so it isn't logged.
    HRESULT hr = DmOpenPerformanceCounter("frames", &h);
    if (!SUCCEEDED(hr)) {
        return;
    }

    // Read counter, then release the handle.
    hr = DmQueryPerformanceCounterHandle(h, 0x11, &count_data);
    DmClosePerformanceCounter(h);
    if (!SUCCEEDED(hr)) {
        return;
    }

    // Get frame info, but avoid frame 0.
    frame = count_data.CountValue.QuadPart;
    ULONGLONG now = KeQueryInterruptTime();
    if (frame == 0) {
        return;
    }

    // Bootstrap the counter on first pass.
    if (lastFrame == 0) {
        lastFrame = frame;
        lastFrameTime = now;
        return;
    }

    // Frames per measured second: the tick is 1 s of sleep plus this function's work
    // (the X-HD poll's SMBus reads), so a raw per-tick delta would read high.
    ULONGLONG elapsed = now - lastFrameTime;
    delta = frame - lastFrame;
    if (elapsed > 0 && delta >= 0) {
        delta = (LONGLONG)(((ULONGLONG)delta * 10000000ULL + elapsed / 2) / elapsed);
    }
    lastFrame = frame;
    lastFrameTime = now;

    // Clear offscreen backbuffer to neutral black (Y=0, Cb/Cr=127)
    for (i = 0; i < WIDTH * MAX_HEIGHT; i++) {
        s_backbuffer[i * 2 + 0] = 0x00;
        s_backbuffer[i * 2 + 1] = 0x7f;
    }

    // Poll the X-HD every 3 seconds (every third 1-second tick), keeping the last good
    // snapshot through up to XHD_POLL_MISS_LIMIT-1 failed polls.
    if (s_poll_countdown <= 0) {
        if (ReadXhdVideoInfo(&s_cached_vi)) {
            s_cached_xhd_detected = TRUE;
            s_poll_misses = 0;
        } else if (++s_poll_misses >= XHD_POLL_MISS_LIMIT) {
            s_cached_xhd_detected = FALSE;
        }
        s_poll_countdown = 2;
    } else {
        s_poll_countdown--;
    }

    GetVideoFormat(&resWidth, &resHeight, &refresh, &aspect);

    MM_STATISTICS memStats;
    unsigned int freeMb = 0, totalMb = 0;
    memset(&memStats, 0, sizeof(memStats));
    memStats.Length = sizeof(MM_STATISTICS);
    if (NT_SUCCESS(MmQueryStatistics(&memStats))) {
        freeMb = (unsigned int)(memStats.AvailablePages / 256);
        totalMb = (unsigned int)(memStats.TotalPhysicalPages / 256);
    }

    char fpsStr[32];
    if (delta < 0 || delta > 99999) {
        strcpy(fpsStr, "FPS: --");
    } else {
        snprintf(fpsStr, sizeof(fpsStr), "FPS: %i", (int)delta);
    }

    if (resWidth <= 1 || resHeight <= 1) {
        snprintf(message, MAX_PATH, "%s Fmem: %u/%u", fpsStr, freeMb, totalMb);
    } else {
        snprintf(message, MAX_PATH, "%s RES: %ix%i %s %s Fm: %u/%u",
                 fpsStr, resWidth, resHeight, refresh, aspect, freeMb, totalMb);
    }

    // Render Line 1 into offscreen backbuffer
    int w1 = DrawText(s_backbuffer, 0, message);
    int disp_w = w1;
    int disp_h = 14; // 13 glyph lines + 1 pad line = 14 scanlines (even)

    if (s_cached_xhd_detected) {
        char line2[128];
        char refreshStr[32];
        const char *xhd_aspect = (s_cached_vi.flags & XHD_VI_WIDESCREEN) ? "16:9" : "4:3";
        const char *csc_str = "None";

        // X-HD's own values only -- never the console's, or a mismatch would be hidden.
        int w = (int)s_cached_vi.width;
        int h = (int)s_cached_vi.height;

        if (s_cached_vi.refresh_mhz > 0) {
            int whole = s_cached_vi.refresh_mhz / 1000;
            int frac = (s_cached_vi.refresh_mhz % 1000) / 10;
            if (frac > 0) {
                sprintf(refreshStr, "%i.%02iHz", whole, frac);
            } else {
                sprintf(refreshStr, "%iHz", whole);
            }
        } else {
            strcpy(refreshStr, "0Hz");
        }

        switch (s_cached_vi.color_space) {
            case 1: csc_str = "RGB-Full"; break;
            case 2: csc_str = "RGB-Limited"; break;
            case 3: csc_str = "YCbCr-Pass"; break;
            case 4: csc_str = "RGB-Pass"; break;
            default: csc_str = "None"; break;
        }

        if (!(s_cached_vi.flags & XHD_VI_APPLIED)) { // the mode fields aren't valid
            strcpy(line2, "X-HD: no mode");
        } else {
            sprintf(line2, "X-HD: %ix%i %s %s VIC%u %s",
                    w, h, refreshStr, xhd_aspect, (unsigned int)s_cached_vi.wire_vic, csc_str);
        }

        // Render Line 2 into offscreen backbuffer with a 2-pixel black gap
        int w2 = DrawText(s_backbuffer, 15, line2);
        if (w2 > disp_w) {
            disp_w = w2;
        }
        disp_h = 28; // Line 1 (0..12) + Gap (13..14) + Line 2 (15..27) = 28 scanlines (even)
    }

    // Ensure width and height are strictly even numbers of pixels
    disp_w = (disp_w + 1) & ~1;
    width = disp_w;
    height = disp_h;

    // Atomically copy the completed backbuffer into contiguous framebuffer memory
    memcpy(framebuffer, s_backbuffer, PITCH * height);

    // Flush CPU write-back cache BEFORE hardware DMA starts fetching
    FlushCpuCache();

    // Program the NV2A PVIDEO overlay to display the framebuffer.
    base = 0xFD000000 + NV_PVIDEO;

    *(volatile unsigned long *)(base + NV_PVIDEO_STOP) = 0;
    *(volatile unsigned long *)(base + NV_PVIDEO_INTR_EN) = 0;
    *(volatile unsigned long *)(base + NV_PVIDEO_INTR) = *(volatile unsigned long *)(base + NV_PVIDEO_INTR);

    *(volatile unsigned long *)(base + NV_PVIDEO_LUMINANCE) = (0x0 << 16) | 0x1000;
    *(volatile unsigned long *)(base + NV_PVIDEO_CHROMINANCE) = (0x0 << 16) | 0x1000;

    *(volatile unsigned long *)(base + NV_PVIDEO_BASE) = 0x00000000;
    *(volatile unsigned long *)(base + NV_PVIDEO_LIMIT) = 0xFFFFFFFF;
    *(volatile unsigned long *)(base + NV_PVIDEO_OFFSET) = (unsigned long)MmGetPhysicalAddress(framebuffer);

    *(volatile unsigned long *)(base + NV_PVIDEO_POINT_IN) = (0 << 16) | 0;
    *(volatile unsigned long *)(base + NV_PVIDEO_SIZE_IN) = (height << 16) | width;

    *(volatile unsigned long *)(base + NV_PVIDEO_POINT_OUT) = (OUT_Y << 16) | OUT_X;
    *(volatile unsigned long *)(base + NV_PVIDEO_SIZE_OUT) = (height << 16) | width;

    *(volatile unsigned long *)(base + NV_PVIDEO_DS_DX) = 0x00100000;
    *(volatile unsigned long *)(base + NV_PVIDEO_DT_DY) = 0x00100000;

    *(volatile unsigned long *)(base + NV_PVIDEO_FORMAT) =
        NV_PVIDEO_FORMAT_MATRIX | (NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8 << 16) | PITCH;

    *(volatile unsigned long *)(base + NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
}

// Worker thread: repaint the overlay once per second. Passed to
// PsCreateSystemThreadEx as the SystemRoutine (StartContext = framebuffer).
static void STDCALL Process(PKSTART_ROUTINE StartRoutine, PVOID StartContext)
{
    unsigned char *framebuffer = (unsigned char *)StartContext;
    LARGE_INTEGER interval;
    (void)StartRoutine;

    DbgPrint("dxt X-HD: Worker thread started\n");

    for (;;) {
        UpdateFramebuffer(framebuffer);
        interval.QuadPart = -10000000LL; // 1 second, relative
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
}

// DXT entry point. xbdm calls this (via its CallDxtEntry asm thunk) once the
// image is loaded and relocated, passing a BOOL* the extension sets to request
// immediate unload. We spin up the overlay worker and stay resident.
void DxtEntry(unsigned long *pfUnload)
{
    PVOID framebuffer;
    HANDLE handle;
    NTSTATUS status;

    DbgPrint("dxt X-HD: DxtEntry, starting overlay thread\n");

    framebuffer = MmAllocateContiguousMemory(PITCH * MAX_HEIGHT);
    if (framebuffer == NULL) {
        DbgPrint("dxt X-HD: Failed to allocate framebuffer memory\n");
        *pfUnload = 1;
        return;
    }

    status = PsCreateSystemThreadEx(&handle, 0, 8192, 0, NULL, NULL, framebuffer,
        FALSE, FALSE, Process);

    // Stay loaded only if the worker thread was created successfully.
    if (!NT_SUCCESS(status)) {
        DbgPrint("dxt X-HD: Failed to create worker thread, status=0x%08lX\n", (unsigned long)status);
        MmFreeContiguousMemory(framebuffer);
        *pfUnload = 1;
        return;
    }
    NtClose(handle);    // the thread runs on; the handle isn't needed
    *pfUnload = 0;
    DbgPrint("dxt X-HD: Worker thread created successfully\n");
}
