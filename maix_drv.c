// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include "maix_drv.h"

static struct usb_driver maix_usb_driver;
static struct maix_dev *g_active_dev;
static DEFINE_MUTEX(g_dev_lock);
static struct proc_dir_entry *g_proc_entry;

static inline void ring_init(struct maix_ring_buf *rb)
{
	rb->head = 0;
	rb->tail = 0;
	rb->count = 0;
}

static size_t ring_push(struct maix_ring_buf *rb, const __u8 *src, size_t len)
{
	size_t pushed = 0;

	while (pushed < len && rb->count < MAIX_RING_CAPACITY) {
		rb->data[rb->head] = src[pushed++];
		rb->head = (rb->head + 1) % MAIX_RING_CAPACITY;
		rb->count++;
	}
	return pushed;
}

static size_t ring_pop(struct maix_ring_buf *rb, __u8 *dst, size_t len)
{
	size_t popped = 0;

	while (popped < len && rb->count > 0) {
		dst[popped++] = rb->data[rb->tail];
		rb->tail = (rb->tail + 1) % MAIX_RING_CAPACITY;
		rb->count--;
	}
	return popped;
}

static void maix_rx_urb_callback(struct urb *urb)
{
	struct maix_dev *dev = urb->context;
	unsigned long flags;
	int status = urb->status;
	int retval;

	if (!dev || dev->disconnected)
		return;

	if (status) {
		if (status != -ENOENT && status != -ECONNRESET &&
		    status != -ESHUTDOWN) {
			spin_lock_irqsave(&dev->rx_lock, flags);
			dev->stats.urb_errors++;
			spin_unlock_irqrestore(&dev->rx_lock, flags);
		}
		return;
	}

	if (urb->actual_length > 2) {
		size_t payload_len = urb->actual_length - 2;
		ktime_t now = ktime_get();

		spin_lock_irqsave(&dev->rx_lock, flags);
		ring_push(&dev->rx_ring, dev->rx_dma_buf + 2, payload_len);
		dev->stats.rx_packets++;
		dev->stats.ring_used = dev->rx_ring.count;
		if (dev->last_tx_ts)
			dev->stats.last_rtt_ns =
				ktime_to_ns(ktime_sub(now, dev->last_tx_ts));
		spin_unlock_irqrestore(&dev->rx_lock, flags);

		wake_up_interruptible(&dev->rx_waitq);
	}

	retval = usb_submit_urb(dev->rx_urb, GFP_ATOMIC);
	if (retval && retval != -ENODEV)
		dev_err(&dev->interface->dev,
			"maix: failed to resubmit RX URB (%d)\n", retval);
}

static int maix_send_frame(struct maix_dev *dev, __u8 cmd,
			   const __u8 *payload, __u8 len)
{
	__u8 frame[MAIX_MAX_PKT_SIZE];
	__u8 csum = 0;
	int actual_len = 0;
	int ret, i;

	if (len > MAIX_MAX_PKT_SIZE - 4)
		return -EMSGSIZE;

	frame[0] = MAIX_FRAME_MAGIC;
	frame[1] = cmd;
	frame[2] = len;
	csum ^= cmd ^ len;

	for (i = 0; i < len; i++) {
		frame[3 + i] = payload[i];
		csum ^= payload[i];
	}
	frame[3 + len] = csum;

	dev->last_tx_ts = ktime_get();
	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, dev->bulk_out_addr),
			   frame, len + 4, &actual_len, 1000);
	if (!ret) {
		unsigned long flags;

		spin_lock_irqsave(&dev->rx_lock, flags);
		dev->stats.tx_packets++;
		spin_unlock_irqrestore(&dev->rx_lock, flags);
	}
	return ret;
}

static int maix_open(struct inode *inode, struct file *file)
{
	struct usb_interface *intf;
	struct maix_dev *dev;

	intf = usb_find_interface(&maix_usb_driver, iminor(inode));
	if (!intf)
		return -ENODEV;

	dev = usb_get_intfdata(intf);
	if (!dev || dev->disconnected)
		return -ENODEV;

	file->private_data = dev;
	return 0;
}

static int maix_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static ssize_t maix_read(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct maix_dev *dev = file->private_data;
	__u8 kbuf[MAIX_RING_CAPACITY];
	unsigned long flags;
	size_t to_read, popped;

	if (!dev || dev->disconnected)
		return -ENODEV;
	if (count == 0)
		return 0;

	to_read = min_t(size_t, count, MAIX_RING_CAPACITY);

	if (wait_event_interruptible(dev->rx_waitq,
				     dev->rx_ring.count > 0 ||
				     dev->disconnected))
		return -ERESTARTSYS;

	if (dev->disconnected && dev->rx_ring.count == 0)
		return -ENODEV;

	spin_lock_irqsave(&dev->rx_lock, flags);
	popped = ring_pop(&dev->rx_ring, kbuf, to_read);
	dev->stats.ring_used = dev->rx_ring.count;
	spin_unlock_irqrestore(&dev->rx_lock, flags);

	if (copy_to_user(buf, kbuf, popped))
		return -EFAULT;

	return popped;
}

static ssize_t maix_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct maix_dev *dev = file->private_data;
	__u8 kbuf[MAIX_MAX_PKT_SIZE - 4];
	size_t chunk;
	int ret;

	if (!dev || dev->disconnected)
		return -ENODEV;
	if (count == 0)
		return 0;

	chunk = min_t(size_t, count, sizeof(kbuf));
	if (copy_from_user(kbuf, buf, chunk))
		return -EFAULT;

	if (mutex_lock_interruptible(&dev->io_mutex))
		return -ERESTARTSYS;

	if (dev->disconnected) {
		mutex_unlock(&dev->io_mutex);
		return -ENODEV;
	}

	if (dev->stats.rate_limit_pps > 0) {
                ktime_t now = ktime_get();
                s64 elapsed_ns = ktime_to_ns(ktime_sub(now, dev->rate_window_ts));
                if (elapsed_ns >= 1000000000LL) {
                        dev->rate_window_ts = now;
                        dev->rate_window_cnt = 0;
                }
                if (dev->rate_window_cnt >= dev->stats.rate_limit_pps) {
                        dev->stats.rate_throttled++;
                        mutex_unlock(&dev->io_mutex);
                        return -EAGAIN;
                }
                dev->rate_window_cnt++;
        }
        ret = maix_send_frame(dev, MAIX_CMD_OFFLOAD, kbuf, (__u8)chunk);
	mutex_unlock(&dev->io_mutex);

	return ret ? ret : (ssize_t)chunk;
}

static __poll_t maix_poll(struct file *file, poll_table *wait)
{
	struct maix_dev *dev = file->private_data;
	__poll_t mask = 0;
	unsigned long flags;

	if (!dev)
		return EPOLLERR;

	poll_wait(file, &dev->rx_waitq, wait);

	spin_lock_irqsave(&dev->rx_lock, flags);
	if (dev->disconnected)
		mask |= EPOLLHUP | EPOLLERR;
	if (dev->rx_ring.count > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	mask |= EPOLLOUT | EPOLLWRNORM;
	spin_unlock_irqrestore(&dev->rx_lock, flags);

	return mask;
}

static long maix_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct maix_dev *dev = file->private_data;
	struct maix_led_req led;
	struct maix_hw_stats stats_snap;
	unsigned long flags;
	int ret = 0;

	if (!dev || dev->disconnected)
		return -ENODEV;

	switch (cmd) {
	case MAIX_IOC_SET_LED: {
		__u8 payload[3];

		if (copy_from_user(&led, (void __user *)arg, sizeof(led)))
			return -EFAULT;

		payload[0] = led.r;
		payload[1] = led.g;
		payload[2] = led.b;

		if (mutex_lock_interruptible(&dev->io_mutex))
			return -ERESTARTSYS;
		ret = maix_send_frame(dev, MAIX_CMD_LED, payload, 3);
		mutex_unlock(&dev->io_mutex);
		return ret;
	}
	case MAIX_IOC_GET_STATS:
		spin_lock_irqsave(&dev->rx_lock, flags);
		stats_snap = dev->stats;
		stats_snap.ring_used = dev->rx_ring.count;
		stats_snap.connected = !dev->disconnected;
		spin_unlock_irqrestore(&dev->rx_lock, flags);

		if (copy_to_user((void __user *)arg, &stats_snap,
				 sizeof(stats_snap)))
			return -EFAULT;
		return 0;

	case MAIX_IOC_FLUSH_RING:
		spin_lock_irqsave(&dev->rx_lock, flags);
		ring_init(&dev->rx_ring);
		dev->stats.ring_used = 0;
		spin_unlock_irqrestore(&dev->rx_lock, flags);
		return 0;

	case MAIX_IOC_SET_RATE: {
                __u32 pps;
                if (copy_from_user(&pps, (void __user *)arg, sizeof(pps)))
                        return -EFAULT;
                if (mutex_lock_interruptible(&dev->io_mutex))
                        return -ERESTARTSYS;
                dev->stats.rate_limit_pps = pps;
                dev->rate_window_ts = ktime_get();
                dev->rate_window_cnt = 0;
                mutex_unlock(&dev->io_mutex);
                return 0;
        }
        case MAIX_IOC_RAW_FRAME: {
                __u8 raw[8];
                __u8 csum = 0;
                int i;
                if (copy_from_user(raw, (void __user *)arg, sizeof(raw)))
                        return -EFAULT;
                for (i = 2; i < 7; i++)
                        csum ^= raw[i];
                if (raw[0] != MAIX_FRAME_MAGIC || raw[1] != 0x55 || csum != raw[7]) {
                        spin_lock_irqsave(&dev->rx_lock, flags);
                        dev->stats.fw_dropped++;
                        spin_unlock_irqrestore(&dev->rx_lock, flags);
                        return -EBADMSG;
                }
                if (mutex_lock_interruptible(&dev->io_mutex))
                        return -ERESTARTSYS;
                ret = maix_send_frame(dev, raw[2], &raw[4], 3);
                mutex_unlock(&dev->io_mutex);
                return ret;
        }
        default:
		return -ENOTTY;
	}
}

static const struct file_operations maix_fops = {
	.owner          = THIS_MODULE,
	.open           = maix_open,
	.release        = maix_release,
	.read           = maix_read,
	.write          = maix_write,
	.poll           = maix_poll,
	.unlocked_ioctl = maix_ioctl,
};

static int maix_proc_show(struct seq_file *m, void *v)
{
	struct maix_hw_stats s = {0};
	unsigned long flags;

	mutex_lock(&g_dev_lock);
	if (g_active_dev) {
		spin_lock_irqsave(&g_active_dev->rx_lock, flags);
		s = g_active_dev->stats;
		s.ring_used = g_active_dev->rx_ring.count;
		s.connected = !g_active_dev->disconnected;
		spin_unlock_irqrestore(&g_active_dev->rx_lock, flags);
	}
	mutex_unlock(&g_dev_lock);

	seq_printf(m, "connected:    %u\n", s.connected);
	seq_printf(m, "tx_packets:   %llu\n", s.tx_packets);
	seq_printf(m, "rx_packets:   %llu\n", s.rx_packets);
	seq_printf(m, "urb_errors:   %llu\n", s.urb_errors);
	seq_printf(m, "last_rtt_ns:  %llu\n", s.last_rtt_ns);
	seq_printf(m, "ring_used:    %u / %d\n", s.ring_used, MAIX_RING_CAPACITY);
	return 0;
}

static int maix_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, maix_proc_show, NULL);
}

static const struct proc_ops maix_proc_ops = {
	.proc_open    = maix_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static int maix_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct maix_dev *dev;
	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->rx_lock);
	init_waitqueue_head(&dev->rx_waitq);
	ring_init(&dev->rx_ring);

	ret = usb_find_common_endpoints(interface->cur_altsetting,
					&bulk_in, &bulk_out, NULL, NULL);
	if (ret) {
		dev_err(&interface->dev, "maix: bulk endpoints not found\n");
		goto err_free_dev;
	}

	dev->bulk_in_addr = bulk_in->bEndpointAddress;
	dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
	dev->bulk_out_addr = bulk_out->bEndpointAddress;

	dev->rx_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->rx_urb) {
		ret = -ENOMEM;
		goto err_free_dev;
	}

	dev->rx_dma_buf = usb_alloc_coherent(dev->udev, dev->bulk_in_size,
					     GFP_KERNEL,
					     &dev->rx_urb->transfer_dma);
	if (!dev->rx_dma_buf) {
		ret = -ENOMEM;
		goto err_free_urb;
	}

	usb_fill_bulk_urb(dev->rx_urb, dev->udev,
			  usb_rcvbulkpipe(dev->udev, dev->bulk_in_addr),
			  dev->rx_dma_buf, dev->bulk_in_size,
			  maix_rx_urb_callback, dev);
	dev->rx_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0),
			0x03, 0x40, 0x001A, 0, NULL, 0, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x01, 0x40, 0x0303, 0, NULL, 0, 1000);

	usb_set_intfdata(interface, dev);

	dev->class_drv.name = "maix%d";
	dev->class_drv.fops = &maix_fops;
	ret = usb_register_dev(interface, &dev->class_drv);
	if (ret) {
		dev_err(&interface->dev, "maix: usb_register_dev failed\n");
		goto err_free_dma;
	}

	dev->stats.connected = 1;
	mutex_lock(&g_dev_lock);
	g_active_dev = dev;
	mutex_unlock(&g_dev_lock);

	ret = usb_submit_urb(dev->rx_urb, GFP_KERNEL);
	if (ret) {
		dev_err(&interface->dev, "maix: initial URB submit failed\n");
		goto err_dereg_dev;
	}

	dev_info(&interface->dev,
		 "maix: bound Sipeed Maix Bit to /dev/maix%d (IN=0x%02x OUT=0x%02x)\n",
		 interface->minor, dev->bulk_in_addr, dev->bulk_out_addr);
	return 0;

err_dereg_dev:
	mutex_lock(&g_dev_lock);
	if (g_active_dev == dev)
		g_active_dev = NULL;
	mutex_unlock(&g_dev_lock);
	usb_deregister_dev(interface, &dev->class_drv);
err_free_dma:
	usb_free_coherent(dev->udev, dev->bulk_in_size,
			  dev->rx_dma_buf, dev->rx_urb->transfer_dma);
err_free_urb:
	usb_free_urb(dev->rx_urb);
err_free_dev:
	usb_put_dev(dev->udev);
	kfree(dev);
	return ret;
}

static void maix_disconnect(struct usb_interface *interface)
{
	struct maix_dev *dev = usb_get_intfdata(interface);

	if (!dev)
		return;

	dev->disconnected = true;
	wake_up_interruptible(&dev->rx_waitq);

	usb_kill_urb(dev->rx_urb);
	usb_deregister_dev(interface, &dev->class_drv);

	mutex_lock(&g_dev_lock);
	if (g_active_dev == dev)
		g_active_dev = NULL;
	mutex_unlock(&g_dev_lock);

	usb_free_coherent(dev->udev, dev->bulk_in_size,
			  dev->rx_dma_buf, dev->rx_urb->transfer_dma);
	usb_free_urb(dev->rx_urb);
	usb_put_dev(dev->udev);
	kfree(dev);

	dev_info(&interface->dev, "maix: Sipeed Maix Bit disconnected cleanly\n");
}

static const struct usb_device_id maix_id_table[] = {
	{ USB_DEVICE(MAIX_USB_VENDOR_ID, MAIX_USB_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, maix_id_table);

static struct usb_driver maix_usb_driver = {
	.name       = MAIX_DEV_NAME,
	.probe      = maix_probe,
	.disconnect = maix_disconnect,
	.id_table   = maix_id_table,
};

static int __init maix_module_init(void)
{
	g_proc_entry = proc_create(MAIX_PROC_NAME, 0444, NULL, &maix_proc_ops);
	return usb_register(&maix_usb_driver);
}

static void __exit maix_module_exit(void)
{
	usb_deregister(&maix_usb_driver);
	if (g_proc_entry)
		proc_remove(g_proc_entry);
}

module_init(maix_module_init);
module_exit(maix_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sakshi Birman");
MODULE_DESCRIPTION("USB Hardware Offload & Telemetry Driver for Sipeed Maix Bit");
