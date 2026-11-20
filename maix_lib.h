#ifndef MAIX_LIB_H
#define MAIX_LIB_H

#include <stddef.h>
#include <stdint.h>
#include "maix_drv.h"

typedef struct maix_handle maix_t;

maix_t *maix_open(const char *dev_path);
void    maix_close(maix_t *handle);

int     maix_set_led(maix_t *handle, uint8_t r, uint8_t g, uint8_t b);
ssize_t maix_offload_write(maix_t *handle, const void *buf, size_t len);
ssize_t maix_read_reply(maix_t *handle, void *buf, size_t len);
int     maix_wait_ready(maix_t *handle, int timeout_ms);
int     maix_get_stats(maix_t *handle, struct maix_hw_stats *out_stats);
int     maix_flush(maix_t *handle);

#endif /* MAIX_LIB_H */
