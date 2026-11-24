#include "ato_usb_cdc.h"

#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"

#define WRITE_STALL_MS 100 // host not reading: drop the output after this
#define WRITE_WAIT_MS 10   // safety period between two write attempts

struct ato_usb_cdc {
  uint8_t itf;
  TaskHandle_t reader; // notified on reception
  TaskHandle_t writer; // notified on transmit completion
  volatile uint32_t dropped;
};

static ato_usb_cdc_t s_cdc[CFG_TUD_CDC];

ato_usb_cdc_t *ato_usb_cdc_get(uint8_t itf) {
  configASSERT(itf < CFG_TUD_CDC);
  s_cdc[itf].itf = itf;
  return &s_cdc[itf];
}

uint32_t ato_usb_cdc_dropped(const ato_usb_cdc_t *c) { return c->dropped; }

/*
 * Notifications may be shared with other events of the task (e.g. reader and
 * writer are the same task): after each wake-up, the state is read again,
 * so a notification consumed by the other side is never lost.
 */

size_t ato_usb_cdc_read(void *arg, char *buf, size_t size) {
  ato_usb_cdc_t *c = arg;
  c->reader = xTaskGetCurrentTaskHandle();
  for (;;) {
    // the stack is initialized by the USB task, which may not have run yet
    if (tud_inited()) {
      uint32_t n = tud_cdc_n_read(c->itf, buf, size);
      if (n > 0) {
        return n;
      }
    }
    // woken by tud_cdc_rx_cb(); the timeout covers the start of the stack
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
  }
}

void ato_usb_cdc_write(void *arg, const char *data, size_t len) {
  ato_usb_cdc_t *c = arg;
  c->writer = xTaskGetCurrentTaskHandle();
  TickType_t progress = xTaskGetTickCount();

  while (len > 0) {
    if (!tud_inited() || !tud_cdc_n_connected(c->itf)) {
      c->dropped += len; // no terminal: nobody to read the output
      return;
    }
    uint32_t n = tud_cdc_n_write(c->itf, data, len);
    tud_cdc_n_write_flush(c->itf);
    data += n;
    len -= n;
    if (n > 0) {
      progress = xTaskGetTickCount();
    } else if (xTaskGetTickCount() - progress > pdMS_TO_TICKS(WRITE_STALL_MS)) {
      c->dropped += len; // the host does not read
      return;
    }
    if (len > 0) {
      // woken by tud_cdc_tx_complete_cb(), when room is made in the FIFO
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WRITE_WAIT_MS));
    }
  }
}

/* TinyUSB callbacks (USB task context) ------------------------------------- */

void tud_cdc_rx_cb(uint8_t itf) {
  TaskHandle_t t = s_cdc[itf].reader;
  if (t != NULL) {
    xTaskNotifyGive(t);
  }
}

void tud_cdc_tx_complete_cb(uint8_t itf) {
  TaskHandle_t t = s_cdc[itf].writer;
  if (t != NULL) {
    xTaskNotifyGive(t);
  }
}
