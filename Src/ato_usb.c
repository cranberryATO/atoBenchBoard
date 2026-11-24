#include "ato_usb.h"

#include "FreeRTOS.h"
#include "ato_error.h"
#include "main.h"
#include "task.h"
#include "tusb.h"

#define USB_STACK_WORDS 256 // 1 KB
// above the tasks using the USB interfaces (CLI...): events processed first
#define USB_PRIORITY (tskIDLE_PRIORITY + 3)
// TinyUSB calls FreeRTOS from its interrupt: not above this priority
#define USB_IRQ_PRIORITY configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY

static void usb_task(void *argument) {
  (void)argument;
  const tusb_rhport_init_t init = {
      .role = TUSB_ROLE_DEVICE,
      .speed = TUSB_SPEED_FULL,
  };
  if (!tusb_init(0, &init)) {
    fatalError("USB: TinyUSB init failed");
  }
  for (;;) {
    tud_task(); // waits for the events of the USB interrupt
  }
}

void ato_usb_start(void) {
  // TinyUSB enables both USB interrupts; CubeMX only configures USB_LP
  HAL_NVIC_SetPriority(USB_HP_IRQn, USB_IRQ_PRIORITY, 0);
  HAL_NVIC_SetPriority(USB_LP_IRQn, USB_IRQ_PRIORITY, 0);

  if (xTaskCreate(usb_task, "USB", USB_STACK_WORDS, NULL, USB_PRIORITY, NULL) !=
      pdPASS) {
    fatalError("USB: task creation failed");
  }
}

/* Interrupts (their CubeMX handlers are not generated) --------------------- */

void USB_HP_IRQHandler(void) { tud_int_handler(0); }

void USB_LP_IRQHandler(void) { tud_int_handler(0); }
