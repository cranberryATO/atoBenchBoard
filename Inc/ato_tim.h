#ifndef ATO_TIM_H
#define ATO_TIM_H

#include "ato_cli.h"

#define ATO_TIM_COMMANDS(X)                                                   \
  X("pwm", "Start the PWM of TIM3 channel 1", pwminit_command)

typedef struct ato_tim_ctx {
  ATO_CLI_ITEMS(ATO_TIM_COMMANDS, cmd_items);
} ato_tim_ctx_t;


void ato_tim_init(ato_tim_ctx_t *ctx, ato_cli_ctx_t *cli);

#endif // ATO_TIM_H