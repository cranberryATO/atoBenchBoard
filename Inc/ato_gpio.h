#ifndef ATO_GPIO_H
#define ATO_GPIO_H

#include "ato_cli.h"

#define ATO_GPIO_COMMANDS(X)                                                   \
  X("gpio show", "Show the GPIOs configuration", gshow_command)

typedef struct ato_gpio_ctx {
  ATO_CLI_ITEMS(ATO_GPIO_COMMANDS);
} ato_gpio_ctx_t;


void ato_gpio_init(ato_gpio_ctx_t *ctx, ato_cli_ctx_t *cli);

#endif // ATO_GPIO_H