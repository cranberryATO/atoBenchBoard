#ifndef ATO_USB_CDC_H
#define ATO_USB_CDC_H

/*
 * A CDC interface of the USB device as a byte stream: one reader task, and
 * blocking writes. The read / write functions have the signature of the CLI
 * transport callbacks; `arg` is the interface, from ato_usb_cdc_get().
 */

#include <stddef.h>
#include <stdint.h>

typedef struct ato_usb_cdc ato_usb_cdc_t;

/** CDC interface number `itf` (0..CFG_TUD_CDC-1). */
ato_usb_cdc_t *ato_usb_cdc_get(uint8_t itf);

/**
 * Wait until at least one byte is received, copy up to `size` bytes in
 * `buf`; returns the number of bytes copied (> 0). Only one task may read a
 * given interface.
 */
size_t ato_usb_cdc_read(void *arg, char *buf, size_t size);

/**
 * Write `len` bytes, blocking while the transmit FIFO is full. The output is
 * dropped (and counted) when no terminal has the port open (DTR off), or when
 * the host does not read for 100 ms: a writer is never blocked for good.
 */
void ato_usb_cdc_write(void *arg, const char *data, size_t len);

/** Number of bytes dropped by ato_usb_cdc_write() since the start. */
uint32_t ato_usb_cdc_dropped(const ato_usb_cdc_t *c);

#endif /* ATO_USB_CDC_H */
