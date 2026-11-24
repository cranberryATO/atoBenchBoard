#include "ato_tim.h"
#include "ato_utils.h"
#include "stm32g4xx_hal_tim.h"
#include "tim.h"

static void pwminit_command(ato_cli_core_t *cli, const char *cmd_string) {
  (void)cmd_string;
  ato_cli_core_puts(cli, "PWM\r\n");
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
}

ATO_CLI_TABLE(tim_commands, ATO_TIM_COMMANDS);

void ato_tim_init(ato_tim_ctx_t *ctx, ato_cli_ctx_t *cli) {
  for (size_t i = 0; i < COUNT_OF(tim_commands); i++) {
    ato_cli_register_command(cli, &tim_commands[i], &ctx->cmd_items[i]);
  }
}