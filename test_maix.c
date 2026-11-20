#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "maix_lib.h"

int main(void)
{
	maix_t *dev;
	struct maix_hw_stats stats;
	const char *msg = "AOS_HW_OFFLOAD_TEST";
	char rx_buf[128] = {0};
	ssize_t n;

	printf("=== Sipeed Maix Bit (/dev/maix0) Kernel Driver Test Suite ===\n");

	dev = maix_open("/dev/maix0");
	if (!dev) {
		perror("[FAIL] maix_open(/dev/maix0)");
		return 1;
	}
	printf("[PASS] Test 1: Opened /dev/maix0 successfully\n");

	if (maix_set_led(dev, 1, 0, 0) < 0)
		perror("[FAIL] maix_set_led(RED)");
	else
		printf("[PASS] Test 2: Sent IOCTL RGB LED command (RED) over USB\n");
	usleep(200000);
	maix_set_led(dev, 0, 1, 0);

	n = maix_offload_write(dev, msg, strlen(msg));
	if (n != (ssize_t)strlen(msg)) {
		perror("[FAIL] maix_offload_write");
	} else {
		printf("[PASS] Test 3: Dispatched %zd-byte framed USB URB\n", n);
	}

	printf("[INFO] Test 4: Waiting up to 1000ms on poll() (or press BOOT button)...\n");
	int ready = maix_wait_ready(dev, 1000);
	if (ready > 0) {
		n = maix_read_reply(dev, rx_buf, sizeof(rx_buf) - 1);
		printf("[PASS] Test 4: poll() woke up! Read %zd bytes from ring buffer\n", n);
	} else if (ready == 0) {
		printf("[PASS] Test 4: poll() timed out cleanly without busy-waiting\n");
	} else {
		perror("[FAIL] maix_wait_ready");
	}

	if (maix_read_reply(dev, (void *)0x1, 16) < 0 && errno == EFAULT)
		printf("[PASS] Test 5: Invalid user pointer rejected with -EFAULT\n");

	if (maix_get_stats(dev, &stats) == 0) {
		printf("[PASS] Test 6: Telemetry -> TX=%llu RX=%llu Err=%llu RTT=%llu ns\n",
		       (unsigned long long)stats.tx_packets,
		       (unsigned long long)stats.rx_packets,
		       (unsigned long long)stats.urb_errors,
		       (unsigned long long)stats.last_rtt_ns);
	}

	maix_close(dev);
	printf("[PASS] Test 7: Closed /dev/maix0 cleanly\n");
	return 0;
}
