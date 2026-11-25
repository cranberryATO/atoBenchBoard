#include "ato_cli.h"

#include "FreeRTOS.h"
#include "ato_cli_core.h"
#include "ato_error.h"
#include "ato_utils.h"
#include "task.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CLI_STACK_WORDS 512 // 2 KB: vsnprintf
#define CLI_PRIORITY (tskIDLE_PRIORITY + 1)

/* Line editor -------------------------------------------------------------- */

static void cli_run(ato_cli_ctx_t *ctx, char *command) {
  ato_cli_core_process(&ctx->cli, command);
}

static void write_string(ato_cli_ctx_t *ctx, const char *str) {
  ctx->cli.write_fn(ctx->cli.io_arg, (uint8_t *)str, strlen(str));
}
// Line editor: echo, end of line (\r, \n or \r\n), backspace, overlong lines.
// No echo in json mode (scripts).
static uint32_t line_editor(ato_cli_ctx_t *ctx, const uint8_t *bytes,
                            uint32_t size) {

  for (uint32_t i = 0; i < size; i++) {
    uint8_t c = bytes[i];
    // read for each byte: a command of the line may change the mode
    bool echo = ato_cli_core_mode(&ctx->cli) == ATO_CLI_MODE_PLAIN;

    if (c == '\r' || c == '\n') {
      uint8_t is_lf_after_cr = (c == '\n') && ctx->last_char_cr;
      ctx->last_char_cr = (c == '\r');
      if (is_lf_after_cr) {
        continue; // second half of \r\n, line already handled
      }
      if (echo) {
        write_string(ctx, "\r\n");
      }
      if (!ctx->discard && ctx->cmd_idx > 0) {
        ctx->cmd_buf[ctx->cmd_idx] = 0;
        cli_run(ctx, ctx->cmd_buf);
      }
      ctx->cmd_idx = 0;
      ctx->discard = 0;
      ato_cli_core_prompt(&ctx->cli);
      continue;
    }
    ctx->last_char_cr = 0;

    if (ctx->discard) {
      continue;
    }

    if (c == 0x7f || c == '\b') { // DEL (most terminals) or BS
      if (ctx->cmd_idx > 0) {
        ctx->cmd_idx--;
        if (echo) {
          write_string(ctx, "\b \b"); // erase the character on screen
        }
      }
      continue;
    }

    if (c < 0x20) {
      continue; // ignore other control characters (no echo)
    }

    if (ctx->cmd_idx >= sizeof(ctx->cmd_buf) - 1) {
      if (echo) {
        write_string(ctx, "\r\n"); // the error after the echoed text
      }
      ato_cli_reply_error(&ctx->cli, "command too long, line ignored");
      ctx->discard = 1;
      continue;
    }

    ctx->cmd_buf[ctx->cmd_idx++] = (char)c;
    if (echo) {
      ctx->cli.write_fn(ctx->cli.io_arg, &c, 1);
    }
  }
  return size;
}

/* CLI task ----------------------------------------------------------------- */

static void cli_task(void *argument) {
  ato_cli_ctx_t *ctx = argument;

  ato_cli_core_prompt(&ctx->cli);
  for (;;) {
    uint32_t n = ctx->cli.read_fn(ctx->cli.io_arg, &ctx->rx_buf[0],
                                  COUNT_OF(ctx->rx_buf));
    line_editor(ctx, &ctx->rx_buf[0], n);
  }
  /* A task must never return; if it must end, call vTaskDelete(NULL). */
}

void ato_cli_create_task(ato_cli_ctx_t *ctx, const char *task_name,
                         ato_cli_read_fn read_fn, ato_cli_write_fn write_fn,
                         void *io_arg, TaskHandle_t *const pxCreatedTask) {
  ato_cli_core_init(&ctx->cli, read_fn, write_fn, io_arg);

  if (xTaskCreate(cli_task, task_name, CLI_STACK_WORDS, ctx, CLI_PRIORITY,
                  pxCreatedTask) != pdPASS) {
    fatalError("Task creation failed for task %s", task_name);
  }
}

void ato_cli_register_command(ato_cli_ctx_t *ctx, const ato_cli_command_t *def,
                              ato_cli_list_item_t *item, void *user_data) {
  configASSERT(ctx != NULL);
  ato_cli_core_register(&ctx->cli, def, item, user_data);
}

void ato_cli_register_command_table(ato_cli_ctx_t *ctx,
                                    const ato_cli_command_t *const commands,
                                    size_t nb_commands,
                                    ato_cli_list_item_t *item,
                                    void *user_data) {
  for (int i = 0; i < nb_commands; i++) {
    ato_cli_register_command(ctx, &commands[i], &item[i], user_data);
  }
}
