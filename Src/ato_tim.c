#include "ato_tim.h"
#include "ato_utils.h"
#include "main.h"
#include "tim.h"

static void pwminit_command(ato_cli_t *cli, void *user_data,
                            const ato_cli_arg_t *args, uint8_t argc) {
  (void)user_data;
  (void)args;
  (void)argc;
  if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1) != HAL_OK) {
    ato_cli_reply_error(cli, "PWM start failed (already running?)");
    return;
  }
  ato_cli_reply_begin(cli);
  ato_cli_reply_end(cli);
}

ATO_CLI_TABLE(tim_commands, ATO_TIM_COMMANDS);

void ato_tim_init(ato_tim_ctx_t *ctx, ato_cli_t *cli) {
  for (size_t i = 0; i < COUNT_OF(tim_commands); i++) {
    ato_cli_register_command(cli, &tim_commands[i], &ctx->cmd_items[i], ctx);
  }
}