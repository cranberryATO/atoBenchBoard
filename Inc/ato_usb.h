#ifndef ATO_USB_H
#define ATO_USB_H

/*
 * USB device (TinyUSB): composite device, see ato_usb_desc.c for its
 * interfaces. The USB peripheral clock and pins are set up by CubeMX
 * (MX_USB_PCD_Init); TinyUSB then drives the peripheral.
 */

/**
 * @brief  Start the USB device: create the task running the TinyUSB stack.
 *         Call once, before or after the scheduler starts. Failure to create
 *         the task calls fatalError().
 */
void ato_usb_start(void);

#endif /* ATO_USB_H */
