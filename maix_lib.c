#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "maix_lib.h"

struct maix_handle {
	int fd;
};

maix_t *maix_open(const char *dev_path)
{
	maix_t *h;
	const char *path = dev_path ? dev_path : "/dev/maix0";

	h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;

	h->fd = open(path, O_RDWR);
	if (h->fd < 0) {
		free(h);
		return NULL;
	}
	return h;
}

void maix_close(maix_t *handle)
{
	if (!handle)
		return;
	if (handle->fd >= 0)
		close(handle->fd);
	free(handle);
}

int maix_set_led(maix_t *handle, uint8_t r, uint8_t g, uint8_t b)
{
	struct maix_led_req req = { .r = r, .g = g, .b = b, .reserved = 0 };

	if (!handle || handle->fd < 0) {
		errno = EINVAL;
		return -1;
	}
	return ioctl(handle->fd, MAIX_IOC_SET_LED, &req);
}

ssize_t maix_offload_write(maix_t *handle, const void *buf, size_t len)
{
	if (!handle || handle->fd < 0) {
		errno = EINVAL;
		return -1;
	}
	return write(handle->fd, buf, len);
}

ssize_t maix_read_reply(maix_t *handle, void *buf, size_t len)
{
	if (!handle || handle->fd < 0) {
		errno = EINVAL;
		return -1;
	}
	return read(handle->fd, buf, len);
}

int maix_wait_ready(maix_t *handle, int timeout_ms)
{
	struct pollfd pfd;

	if (!handle || handle->fd < 0) {
		errno = EINVAL;
		return -1;
	}
	pfd.fd = handle->fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	return poll(&pfd, 1, timeout_ms);
}

int maix_get_stats(maix_t *handle, struct maix_hw_stats *out_stats)
{
	if (!handle || handle->fd < 0 || !out_stats) {
		errno = EINVAL;
		return -1;
	}
	return ioctl(handle->fd, MAIX_IOC_GET_STATS, out_stats);
}

int maix_flush(maix_t *handle)
{
	if (!handle || handle->fd < 0) {
		errno = EINVAL;
		return -1;
	}
	return ioctl(handle->fd, MAIX_IOC_FLUSH_RING);
}
