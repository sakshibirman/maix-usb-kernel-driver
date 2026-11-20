# Linux USB Character Driver with In-Kernel Packet Validation & Rate Limiting (`maix_drv`)

A custom Ring-0 Linux USB character device driver (`maix_drv.ko`) built for the **Sipeed Maix Bit** hardware controller (`VID:0x0403, PID:0x6001`). Instead of exposing an unstructured serial byte stream like the generic `ftdi_sio` driver (`/dev/ttyUSB0`), this driver exposes `/dev/maix0` (USB Major `180`) and enforces protocol integrity, per-second bandwidth rate limiting, multi-process synchronization, and live `/proc` telemetry directly inside kernel space.

---

## Why a Custom Driver?

Linux's default `ftdi_sio` driver forwards raw user-space `write()` calls straight to the wire with no frame inspection or rate control. On USB-to-UART embedded controllers without hardware `RTS/CTS` flow control:
1. **Preventing Hardware FIFO Overflows:** USB Full-Speed operates at 12+ Mbps while the serial bridge drains at 115,200 baud (~11.5 KB/s) through a 128–512 byte on-chip buffer. Unthrottled user-space writes overflow this FIFO and corrupt command frames.
2. **Preventing Host Bus Starvation:** Tight user-space write loops can flood the xHCI host controller with USB Request Blocks (URBs), starving other USB peripherals on the same hub.
3. **In-Kernel Protocol Integrity:** Validating frame headers (`0xAA 0x55`) and XOR checksums inside Ring 0 rejects malformed or corrupted packets (`-EBADMSG`) before DMA allocation or USB transmission.

---

## Architecture & Data Flow

```text
  User Space (Ring 3)                     Kernel Space (Ring 0: maix_drv.ko)               Hardware
┌─────────────────────┐                ┌────────────────────────────────────────┐      ┌──────────────┐
│ maix_scope /        │  ioctl/write   │ 1. copy_from_user()                    │      │ FTDI Bridge  │
│ test_maix           │ ─────────────► │ 2. Protocol Firewall (0xAA 0x55 + XOR) │      │ (0403:6001)  │
│ (via maix_lib.c)    │                │ 3. Token-Window Rate Limiter (ktime)   │ ───► │      │       │
│                     │  read / poll   │ 4. mutex + DMA Bulk OUT URB            │      │   UART0 TXD  │
│ /proc/maix_stats    │ ◄───────────── │ 5. Bulk IN URB + spinlock RX ring buf  │ ◄─── │ K210 RISC-V  │
└─────────────────────┘                └────────────────────────────────────────┘      └──────────────┘
