#include "ato_cli.h"

#include "FreeRTOS.h"
#include "ato_cli_core.h"
#include "ato_error.h"
#include "ato_uart.h"
#include "ato_utils.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define CLI_CMD_SIZE 100    // max command line length, null included
#define CLI_STACK_WORDS 512 // 2 KB: vsnprintf
#define CLI_PRIORITY (tskIDLE_PRIORITY + 1)

static void ps_command(ato_cli_core_t *cli, const ato_cli_arg_t *args,
                       uint8_t argc);
static void so_command(ato_cli_core_t *cli, const ato_cli_arg_t *args,
                       uint8_t argc);
static void stats_command(ato_cli_core_t *cli, const ato_cli_arg_t *args,
                          uint8_t argc);

static const ato_cli_command_t cli_commands[] = {
    {"ps", "Show the FreeRTOS tasks", ps_command},
    {"so", "Overflow the stack of this task (test)", so_command},
    {"stats", "Show the UART error and drop counters", stats_command},
};

struct ato_cli_ctx {
  ato_uart_t uart;    // byte stream
  ato_cli_core_t cli; // command interpreter, its user pointer is this ctx
  ato_cli_list_item_t
      cmd_items[COUNT_OF(cli_commands)]; // one per registered command

  // line editor state
  char cmd_buf[CLI_CMD_SIZE];
  size_t cmd_idx;
  uint8_t discard;      // line too long: ignore until end of line
  uint8_t last_char_cr; // swallow the \n of a \r\n pair
};

/* CLI commands ------------------------------------------------------------- */

static char task_state_char(eTaskState state) {
  switch (state) {
  case eRunning:
    return 'X';
  case eReady:
    return 'R';
  case eBlocked:
    return 'B';
  case eSuspended:
    return 'S';
  case eDeleted:
    return 'D';
  default:
    return '?';
  }
}

// Same columns as vTaskList(), which needs an unbounded output buffer.
static void ps_command(ato_cli_core_t *cli, const ato_cli_arg_t *args,
                       uint8_t argc) {
  (void)args;
  (void)argc;
  // margin for tasks created between the count and the snapshot
  UBaseType_t size = uxTaskGetNumberOfTasks() + 2;
  TaskStatus_t *tasks = pvPortMalloc(size * sizeof(*tasks));
  if (tasks == NULL) {
    fatalError("ps: allocation failed");
  }
  UBaseType_t count = uxTaskGetSystemState(tasks, size, NULL);

  ato_cli_core_printf(cli, "%-*s state prio stack num\r\n",
                      configMAX_TASK_NAME_LEN, "name");
  for (UBaseType_t i = 0; i < count; i++) {
    const TaskStatus_t *t = &tasks[i];
    ato_cli_core_printf(
        cli, "%-*s   %c   %4lu %5u %3lu\r\n", configMAX_TASK_NAME_LEN,
        t->pcTaskName, task_state_char(t->eCurrentState),
        (unsigned long)t->uxCurrentPriority, (unsigned)t->usStackHighWaterMark,
        (unsigned long)t->xTaskNumber);
  }
  vPortFree(tasks);
}

static void so_command(ato_cli_core_t *cli, const ato_cli_arg_t *args,
                       uint8_t argc) {
  (void)cli;
  (void)args;
  (void)argc;
  volatile int onStack[512]; // 2 KB: the whole task stack
  for (int i = 0; i < 512; i++) {
    onStack[i] = 0xbaadfeed;
  }
  (void)onStack[0];
}

static void stats_command(ato_cli_core_t *cli, const ato_cli_arg_t *args,
                          uint8_t argc) {
  (void)args;
  (void)argc;
  const ato_cli_ctx_t *ctx = ato_cli_core_user(cli);
  const ato_uart_t *u = &ctx->uart;
  ato_cli_core_printf(cli, "tx dropped bytes : %lu\r\n",
                      (unsigned long)u->tx_dropped_bytes);
  ato_cli_core_printf(cli, "uart rx errors   : %lu\r\n",
                      (unsigned long)u->rx_errors);
  ato_cli_core_printf(cli, "uart tx errors   : %lu\r\n",
                      (unsigned long)u->tx_errors);
  ato_cli_core_printf(cli, "rx restart fails : %lu\r\n",
                      (unsigned long)u->rx_restart_failures);
}

/* Output of the commands --------------------------------------------------- */

// Blocking write: a command output can be larger than the TX buffer. Called
// from the CLI task, which is the notify task of the UART (see cli_task).
static void uart_write(void *arg, const char *data, size_t len) {
  ato_uart_write_wait(arg, (const uint8_t *)data, len);
}

/* Line editor -------------------------------------------------------------- */

static void cli_run(ato_cli_ctx_t *ctx, char *command) {
  ato_cli_core_process(&ctx->cli, command);
}

// Line editor: echo, end of line (\r, \n or \r\n), backspace, overlong lines.
static uint32_t line_editor(ato_cli_ctx_t *ctx, const uint8_t *bytes,
                            uint32_t size) {
  ato_uart_t *u = &ctx->uart;

  for (uint32_t i = 0; i < size; i++) {
    uint8_t c = bytes[i];

    if (c == '\r' || c == '\n') {
      uint8_t is_lf_after_cr = (c == '\n') && ctx->last_char_cr;
      ctx->last_char_cr = (c == '\r');
      if (is_lf_after_cr) {
        continue; // second half of \r\n, line already handled
      }
      ato_uart_write_string(u, "\r\n");
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
        ato_uart_write_string(u, "\b \b"); // erase the character on screen
      }
      continue;
    }

    if (c < 0x20) {
      continue; // ignore other control characters (no echo)
    }

    if (ctx->cmd_idx >= sizeof(ctx->cmd_buf) - 1) {
      ato_uart_write_string(u, "\r\nerror: command too long, line ignored\r\n");
      ctx->discard = 1;
      continue;
    }

    ctx->cmd_buf[ctx->cmd_idx++] = (char)c;
    ato_uart_write(u, &c, 1); // echo
  }
  return size;
}

/* CLI task ----------------------------------------------------------------- */

static void cli_task(void *argument) {
  ato_cli_ctx_t *ctx = argument;

  if (ato_uart_start_rx(&ctx->uart, xTaskGetCurrentTaskHandle()) != HAL_OK) {
    fatalError("%s: UART RX DMA start failed", pcTaskGetName(NULL));
  }
  ato_cli_core_prompt(&ctx->cli);
  for (;;) {
    const uint8_t *data;
    uint32_t n = ato_uart_rx_peek(&ctx->uart, &data);
    if (n == 0) {
      // nothing to do: sleep until the RX ISR notifies us
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    } else {
      ato_uart_rx_consume(&ctx->uart, line_editor(ctx, data, n));
    }
  }
  /* A task must never return; if it must end, call vTaskDelete(NULL). */
}

ato_cli_ctx_t *ato_cli_create_task(UART_HandleTypeDef *huart,
                                   const char *task_name) {
  ato_cli_ctx_t *ctx = pvPortMalloc(sizeof(*ctx));
  if (ctx == NULL) {
    fatalError("Allocation of context failed for task %s", task_name);
  }
  memset(ctx, 0, sizeof(*ctx));

  ato_uart_init(&ctx->uart, huart);
  ato_cli_core_init(&ctx->cli, ctx, uart_write, &ctx->uart);
  // list items live in the context: no extra allocation that could fail
  for (size_t i = 0; i < COUNT_OF(cli_commands); i++) {
    ato_cli_core_register(&ctx->cli, &cli_commands[i], &ctx->cmd_items[i]);
  }

  if (xTaskCreate(cli_task, task_name, CLI_STACK_WORDS, ctx, CLI_PRIORITY,
                  NULL) != pdPASS) {
    fatalError("Task creation failed for task %s", task_name);
  }
  return ctx;
}

void ato_cli_register_command(ato_cli_ctx_t *ctx, const ato_cli_command_t *def,
                              ato_cli_list_item_t *item) {
  configASSERT(ctx != NULL);
  ato_cli_core_register(&ctx->cli, def, item);
}
