/*
 * USB descriptors of the composite device: for now a CDC interface (virtual
 * serial port) for the CLI. Further interfaces (e.g. a HID game controller)
 * are added to the enum, the configuration descriptor and tusb_config.h.
 */

#include "stm32g4xx.h" // UID_BASE
#include "tusb.h"

#include <string.h>

// Development identifiers: 0xCafe is the demonstration VID of TinyUSB, not
// to be distributed (get a PID from pid.codes first).
#define ATO_USB_VID 0xCafe
#define ATO_USB_PID 0x4001
#define ATO_USB_BCD_DEVICE 0x0100

enum {
  ITF_NUM_CDC_CLI = 0,  // CDC: communication interface
  ITF_NUM_CDC_CLI_DATA, // CDC: data interface
  ITF_NUM_TOTAL
};

enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_CDC_CLI,
};

#define EPNUM_CDC_CLI_NOTIF 0x81
#define EPNUM_CDC_CLI_OUT 0x02
#define EPNUM_CDC_CLI_IN 0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

/* Device ------------------------------------------------------------------- */

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // composite with an interface association (IAD), required by the CDC
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = ATO_USB_VID,
    .idProduct = ATO_USB_PID,
    .bcdDevice = ATO_USB_BCD_DEVICE,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}

/* Configuration ------------------------------------------------------------ */

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CLI, STRID_CDC_CLI, EPNUM_CDC_CLI_NOTIF, 16,
                       EPNUM_CDC_CLI_OUT, EPNUM_CDC_CLI_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index; // a single configuration
  return desc_configuration;
}

/* Strings ------------------------------------------------------------------ */

static const char *const strings[] = {
    [STRID_MANUFACTURER] = "ATO",
    [STRID_PRODUCT] = "atoBench",
    [STRID_CDC_CLI] = "atoBench CLI",
};

#define DESC_STR_MAX 32 // characters

// Serial number: the 96-bit unique ID of the chip, in hexadecimal.
static size_t serial_string(uint16_t *out) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  for (size_t w = 0; w < 3; w++) {
    uint32_t word = ((const uint32_t *)UID_BASE)[w];
    for (int shift = 28; shift >= 0; shift -= 4) {
      out[n++] = hex[(word >> shift) & 0xF];
    }
  }
  return n; // 24
}

// The buffer must stay valid until the transfer is complete: static.
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  static uint16_t desc[1 + DESC_STR_MAX];
  size_t count;

  if (index == STRID_LANGID) {
    desc[1] = 0x0409; // English (United States)
    count = 1;
  } else if (index == STRID_SERIAL) {
    count = serial_string(&desc[1]);
  } else {
    if (index >= TU_ARRAY_SIZE(strings) || strings[index] == NULL) {
      return NULL;
    }
    const char *s = strings[index];
    count = strlen(s);
    if (count > DESC_STR_MAX) {
      count = DESC_STR_MAX;
    }
    for (size_t i = 0; i < count; i++) {
      desc[1 + i] = (uint8_t)s[i]; // ASCII to UTF-16
    }
  }
  // first word: length in bytes (header included), descriptor type
  desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
  return desc;
}
