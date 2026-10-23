#ifndef __BL_UART_UPDATE_H__
#define __BL_UART_UPDATE_H__

// Firmware update over the debug UART (USART2, 115200 8N1) -- a probe-free,
// Xbox-free recovery path for a bad application image. Bootloader-only; it can
// only rewrite the application region (pages 20..62), never itself.
//
// Wire protocol v3 (all multi-byte fields little-endian). Stop-and-wait: the host
// waits for each MCU reply before sending the next frame. Host->MCU frames are
// binary; MCU->host replies are "XHD*\r\n" text lines sharing the wire with the
// human [BL UART] log (the host ignores any line not starting with "XHD").
//
//   Host -> MCU  HELLO : "XHDU" | ver:u8(=3) | flags:u8 | img_size:u32 | img_crc32:u32
//                        flags bit0 = keep page 63: preserve user_config (encoder
//                        self-latch, HS/VS calibration, ...) across the update; clear =
//                        erase the whole page once the image verifies (factory defaults).
//                        The app-invalid flag in that page is armed for the transfer either way.
//   MCU  -> Host        : "XHDK <page_size> <n_pages>\r\n"   (accepted)
//                       | "XHDE <reason>\r\n"                 (rejected)
//   Host -> MCU  PAGE  : A5 5A | seq:u16 | data[1024] | crc32:u32
//                        (A5 5A preamble; final page 0xFF-padded; seq from 0)
//   MCU  -> Host        : "XHDK <seq>\r\n"   (verified & written; also re-ACKs a
//                                             page whose earlier ACK was lost)
//                       | "XHDN <seq>\r\n"   (CRC/write fail -- host resends seq)
//   after last page:
//   MCU  -> Host  DONE  : "XHDD <status> <crc_hex>\r\n"
//                         status 0 = whole-image CRC OK -> MCU clears the
//                                    app-invalid flag and resets into the app
//                         status 1 = mismatch -> flag stays set; board waits
//
// The A5 5A preamble + a ring drain after every reply make the framing immune to
// the USART2 TX->RX crosstalk on this board (the reader resyncs past any noise).
//
// Call once per bootloader main-loop iteration. Cheap when idle (scans the RX
// ring for the 4-byte "XHDU" magic); blocks for the duration of a transfer once
// it starts, with per-frame receive timeouts.
void bl_uart_update_poll(void);

#endif // __BL_UART_UPDATE_H__
