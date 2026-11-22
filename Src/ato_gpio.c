
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

static void gshow_command(ato_cli_core_t *cli, const char *cmd_string) {
  (void)cmd_string;
  ato_cli_core_puts(cli, "GPIO state\r\n");
  FOR_EACH(GPIO_ADDRESSES, gpio) {
    for (int pin = 0; pin < 16; pin++) {
      uint32_t bsr = 1 << pin;
      uint32_t mode =
          LL_GPIO_GetPinMode(gpio->addr, bsr); // LL_GPIO_MODE_ALTERNATE...
      uint32_t af = LL_GPIO_GetAFPin_0_7(gpio->addr,
                                         bsr); // _8_15 for pins 8..15
      uint32_t pull = LL_GPIO_GetPinPull(gpio->addr, bsr);
      uint32_t lvl = LL_GPIO_IsInputPinSet(gpio->addr, bsr);
      ato_cli_core_printf(cli, "%s PIN %d %lx %lx %lx %lx\r\n", gpio->name,
                          pin, (unsigned long)mode, (unsigned long)af,
                          (unsigned long)pull, (unsigned long)lvl);
    }
  }
}

ATO_CLI_TABLE(gpio_commands, ATO_GPIO_COMMANDS);

void ato_gpio_init(ato_gpio_ctx_t *ctx, ato_cli_ctx_t *cli) {
  for (size_t i = 0; i < COUNT_OF(gpio_commands); i++) {
    ato_cli_register_command(cli, &gpio_commands[i], &ctx->cmd_items[i]);
  }
}