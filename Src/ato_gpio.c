
#include "ato_cli.h"
#include "ato_cli_core.h"
#include "ato_utils.h"

#include <stdio.h>

#include "ato_gpio.h"
#include "stm32g474xx.h"
#include "stm32g4xx_ll_gpio.h"

static struct gpio_definition {
  const char *name;
  GPIO_TypeDef *addr;
} GPIO_ADDRESSES[] = {{"GPIOA", GPIOA}};

static BaseType_t gshow_command(ato_cli_core_t *cli, char *out, size_t out_len,
                                const char *cmd_string) {
  (void)cli;
  int n = snprintf(out, out_len, "GPIO state\r\n");
  out += n;
  out_len -= n;
  FOR_EACH(GPIO_ADDRESSES, gpio) {
    for (int pin = 0; pin < 16; pin++) {
      uint32_t bsr = 1 << pin;
      uint32_t mode =
          LL_GPIO_GetPinMode(gpio->addr, bsr); // LL_GPIO_MODE_ALTERNATE...
      uint32_t af = LL_GPIO_GetAFPin_0_7(gpio->addr,
                                         bsr); // _8_15 for pins 8..15
      uint32_t pull = LL_GPIO_GetPinPull(gpio->addr, bsr);
      uint32_t lvl = LL_GPIO_IsInputPinSet(gpio->addr, bsr);
      n = snprintf(out, out_len, "%s PIN %d %x %x %x %x\r\n", gpio->name, pin,
                   mode, af, pull, lvl);
      out += n;
      out_len -= n;
    }
  }
  return pdFALSE;
}

ATO_CLI_TABLE(gpio_commands, ATO_GPIO_COMMANDS);

void ato_gpio_init(ato_gpio_ctx_t *ctx, ato_cli_ctx_t *cli) {
  for (size_t i = 0; i < COUNT_OF(gpio_commands); i++) {
    ato_cli_register_command(cli, &gpio_commands[i], &ctx->cmd_items[i]);
  }
}