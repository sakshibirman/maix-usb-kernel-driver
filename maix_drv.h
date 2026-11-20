/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MAIX_DRV_H
#define MAIX_DRV_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MAIX_USB_VENDOR_ID      0x0403
#define MAIX_USB_PRODUCT_ID     0x6001

#define MAIX_DEV_NAME           "maix"
#define MAIX_PROC_NAME          "maix_stats"
#define MAIX_MAX_PKT_SIZE       64
#define MAIX_RING_CAPACITY      1024

#define MAIX_FRAME_MAGIC        0xAA
#define MAIX_CMD_LED            0x01
#define MAIX_CMD_OFFLOAD        0x02
#define MAIX_CMD_PING           0x03

#define MAIX_IOC_MAGIC          'M'

struct maix_led_req {
        __u8 r;
        __u8 g;
        __u8 b;
        __u8 reserved;
};

struct maix_hw_stats {
        __u64 tx_packets;
        __u64 rx_packets;
        __u64 urb_errors;
        __u64 last_rtt_ns;
        __u64 fw_dropped;
        __u64 rate_throttled;
        __u32 ring_used;
        __u32 connected;
        __u32 rate_limit_pps;
};

#define MAIX_IOC_SET_LED        _IOW(MAIX_IOC_MAGIC, 1, struct maix_led_req)
#define MAIX_IOC_GET_STATS      _IOR(MAIX_IOC_MAGIC, 2, struct maix_hw_stats)
#define MAIX_IOC_FLUSH_RING     _IO(MAIX_IOC_MAGIC,  3)
#define MAIX_IOC_SET_RATE       _IOW(MAIX_IOC_MAGIC, 4, __u32)
#define MAIX_IOC_RAW_FRAME      _IOW(MAIX_IOC_MAGIC, 5, __u8[8])

#ifdef __KERNEL__
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/ktime.h>

struct maix_ring_buf {
        __u8 data[MAIX_RING_CAPACITY];
        size_t head;
        size_t tail;
        size_t count;
};

struct maix_dev {
        struct usb_device       *udev;
        struct usb_interface    *interface;
        struct usb_class_driver class_drv;

        __u8                    bulk_in_addr;
        __u8                    bulk_out_addr;
        size_t                  bulk_in_size;

        struct urb              *rx_urb;
        unsigned char           *rx_dma_buf;

        struct mutex            io_mutex;
        spinlock_t              rx_lock;
        wait_queue_head_t       rx_waitq;

        struct maix_ring_buf    rx_ring;
        struct maix_hw_stats    stats;
        ktime_t                 last_tx_ts;
        ktime_t                 rate_window_ts;
        __u32                   rate_window_cnt;
        bool                    disconnected;
};
#endif /* __KERNEL__ */

#endif /* MAIX_DRV_H */
