#ifndef ATO_GPIO_H
#define ATO_GPIO_H

#include "ato_cli.h"

#define ATO_GPIO_COMMANDS(X)                                                   \
  X("gshow", "\r\ngshow:\r\n Show GPIOs configuration\r\n\r\n", gshow_command, 0)

typedef struct ato_gpio_ctx {
  ATO_CLI_ITEMS(ATO_GPIO_COMMANDS);
} ato_gpio_ctx_t;


void ato_gpio_init(ato_gpio_ctx_t *ctx, ato_cli_ctx_t *cli);

#endif // ATO_GPIO_H