#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

/*
 * Configuration of TinyUSB (Middlewares/Third_Party/tinyusb): USB device on
 * the full-speed peripheral of the STM32G4, with FreeRTOS. The interfaces of
 * the composite device are described in ato_usb_desc.c.
 */

#define CFG_TUSB_MCU OPT_MCU_STM32G4
#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG 0

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE 64

/* Classes: number of interfaces of each one */
#define CFG_TUD_CDC 1 // CLI
#define CFG_TUD_HID 0 // later: game controller
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

/* CDC: software FIFOs, and bulk endpoint size (full speed: 64) */
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 1024
#define CFG_TUD_CDC_RX_EPSIZE 64
#define CFG_TUD_CDC_TX_EPSIZE 64

/* FreeRTOS 10.3.1 (CubeMX) predates this macro, used by the TinyUSB OSAL; same
 * definition as in later FreeRTOS versions (projdefs.h) */
#ifndef pdTICKS_TO_MS
#define pdTICKS_TO_MS(xTimeInTicks)                                            \
  ((uint32_t)(((uint64_t)(xTimeInTicks) * (uint64_t)1000U) /                   \
              (uint64_t)configTICK_RATE_HZ))
#endif

#endif /* TUSB_CONFIG_H_ */
