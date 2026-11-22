#include "ato_cli.h"
#include "ato_cli_core.h"
#include "ato_error.h"
#include "ato_utils.h"

#include <stdio.h>

#include "ato_gpio.h"
#include "stm32g4xx.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_utils.h"

#define PIN(x) | (1 << (x))
#define PIN_LIST(list) (0 list)

#define GPIO_DEFINITION(name) {#name, name}

static const struct gpio_definition {
  const char *name;
  GPIO_TypeDef *addr;
} GPIO_ADDRESSES[] = {
    GPIO_DEFINITION(GPIOA), GPIO_DEFINITION(GPIOB), GPIO_DEFINITION(GPIOC),
    GPIO_DEFINITION(GPIOD), GPIO_DEFINITION(GPIOE), GPIO_DEFINITION(GPIOF),
    GPIO_DEFINITION(GPIOG),
};

// !! in tables below, unless specified otherwise, indexes match hardware
// values. Do not change without checking chip datasheet

static const char *const GPIO_MODE_STR[] = {"IN", "OUT", "AF", "ANL"};

// Alternate function : instead of storing a hollow matrix, use a search table.
// !! SLOW !! only for CLI
static const struct {
  GPIO_TypeDef *addr;
  int pin;
  int af;
  const char *name;
} GPIO_AF_STR[] = {{GPIOA, 2, 12, "LPUART1_TX"}, {GPIOA, 3, 12, "LPUART1_RX"},
                   {GPIOA, 13, 0, "SWDIO-JTMS"}, {GPIOA, 14, 0, "SWCLK-JTCK"},
                   {GPIOA, 15, 0, "JTDI"},       {GPIOB, 3, 7, "USART2_TX"},
                   {GPIOB, 4, 7, "USART2_RX"},   {GPIOC, 4, 7, "USART1_TX"},
                   {GPIOC, 5, 7, "USART1_RX"}};

static const char *const nb[] = {"0", "1", "2",  "3",  "4",  "5",  "6",  "7",
                                 "8", "9", "10", "11", "12", "13", "14", "15"};

static const char *gpio_af_str(GPIO_TypeDef *addr, int pin, int af) {
  if (LL_GPIO_GetPinMode(addr, 1 << pin) != LL_GPIO_MODE_ALTERNATE)
    return "-";
  FOR_EACH(GPIO_AF_STR, a) {
    if (a->addr == addr && a->pin == pin && a->af == af)
      return a->name;
  }
  return nb[af];
}

static const char *const GPIO_PULL[] = {"NO", "UP", "DO", "??"};

static uint32_t get_available_pins(GPIO_TypeDef *addr) {
  switch (LL_GetPackageType()) {
  case LL_UTILS_PACKAGETYPE_LQFP64:
    switch ((uint32_t)addr) {
    case (uint32_t)GPIOA:
    case (uint32_t)GPIOB:
    case (uint32_t)GPIOC:
      return 0xffff;
    case (uint32_t)GPIOD:
      return PIN_LIST(PIN(2));
    case (uint32_t)GPIOE:
      return 0;
    case (uint32_t)GPIOF:
      return PIN_LIST(PIN(0) PIN(1));
    case (uint32_t)GPIOG:
      return PIN_LIST(PIN(10));
    default:
      fatalError("Invalid GPIO handle %p", addr);
    }
  default:
    fatalError("Package not implemented.");
  }
}

static void gshow_command(ato_cli_core_t *cli, const char *cmd_string) {
  (void)cmd_string;
  ato_cli_core_puts(cli, "GPIOx PIN MODE ALTERN_FUNC PUL LVL\r\n");
  for (int i = 0; i < COUNT_OF(GPIO_ADDRESSES); i++) {
    const struct gpio_definition *gpio = &GPIO_ADDRESSES[i];
    if (!LL_AHB2_GRP1_IsEnabledClock(1U << i)) {
      ato_cli_core_printf(cli, "%s : clock is not enabled\r\n", gpio->name);
    } else {
      uint32_t available_pins = get_available_pins(gpio->addr);
      for (int pin = 0; pin < 16; pin++) {
        if (available_pins & (1U << pin)) {
          uint32_t bsr = 1U << pin;
          uint32_t mode =
              LL_GPIO_GetPinMode(gpio->addr, bsr); // LL_GPIO_MODE_ALTERNATE...
          uint32_t af = pin < 8 ? LL_GPIO_GetAFPin_0_7(gpio->addr, bsr)
                                : LL_GPIO_GetAFPin_8_15(gpio->addr, bsr);

          uint32_t pull = LL_GPIO_GetPinPull(gpio->addr, bsr);
          uint32_t lvl = LL_GPIO_IsInputPinSet(gpio->addr, bsr);
          ato_cli_core_printf(cli, "%s %3d %4s %11s %3s %lx\r\n", gpio->name,
                              pin, GPIO_MODE_STR[mode],
                              gpio_af_str(gpio->addr, pin, af), GPIO_PULL[pull],
                              (unsigned long)lvl);
        }
      }
    }
  }
}

ATO_CLI_TABLE(gpio_commands, ATO_GPIO_COMMANDS);

void ato_gpio_init(ato_gpio_ctx_t *ctx, ato_cli_ctx_t *cli) {
  for (size_t i = 0; i < COUNT_OF(gpio_commands); i++) {
    ato_cli_register_command(cli, &gpio_commands[i], &ctx->cmd_items[i]);
  }
}
