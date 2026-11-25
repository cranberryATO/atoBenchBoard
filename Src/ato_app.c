#include "ato_app.h"

#include "FreeRTOS.h"
#include "task.h"

#include "ato_analog.h"
#include "ato_cli.h"
#include "ato_cli_test.h"
#include "ato_error.h"
#include "ato_gpio.h"
#include "ato_tim.h"
#include "ato_uart.h"
#include "ato_usb.h"
#include "ato_usb_cdc.h"
#include "dac.h"
#include "tim.h"
#include "usart.h"

ATO_CLI_COMMAND_FUNCTION(ps_command);
ATO_CLI_COMMAND_FUNCTION(so_command);
ATO_CLI_COMMAND_FUNCTION(stats_command);

#define APP_CLI_COMMANDS(X)                                                    \
  X("ps", "Show the FreeRTOS tasks", ps_command)                               \
  X("so", "Overflow the stack of this task (test)", so_command)                \
  X("stats", "Show the UART error and drop counters", stats_command)

ATO_CLI_TABLE(cli_commands, APP_CLI_COMMANDS);

typedef struct {
  ATO_CLI_ITEMS(APP_CLI_COMMANDS, cmd_items);
  ato_uart_t uart1_ctx;
  ato_cli_t cli1_ctx;
  ato_analog_ctx_t analog_ctx;
  ato_gpio_ctx_t gpio_ctx;
  ato_tim_ctx_t tim_ctx;
  ato_cli_test_ctx_t cli_test_ctx;
} ato_app_ctx_t;

static ato_app_ctx_t app_ctx;

__weak extern ADC_HandleTypeDef hadc1, hadc2, hadc3, hadc4, hadc5;

static void usb_echo_task(void *argument);

__weak void vApplicationStackOverflowHook(xTaskHandle xTask,
                                          signed char *pcTaskName) {
  /* Run time stack overflow checking is performed if
  configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
  called if a stack overflow is detected. */
  fatalError("Stack overflow in task %s", pcTaskName);
}

__weak void vApplicationMallocFailedHook(void) {
  /* vApplicationMallocFailedHook() will only be called if
  configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h. It is a hook
  function that will get called if a call to pvPortMalloc() fails.
  pvPortMalloc() is called internally by the kernel whenever a task, queue,
  timer or semaphore is created. It is also called by various parts of the
  demo application. If heap_1.c or heap_2.c are used, then the size of the
  heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
  FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
  to query the size of free heap space that remains (although it does not
  provide information on how the remaining heap might be fragmented). */
}

void ato_app_start(void) {
  ato_usb_start();
  // temporary: validation of the CDC, before the CLI is moved onto it
  if (xTaskCreate(usb_echo_task, "USB echo", 256, ato_usb_cdc_get(0),
                  tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
    fatalError("Task creation failed for task USB echo");
  }

  // Initialise the USART 1 and start receiving
  ato_uart_init(&app_ctx.uart1_ctx, &huart1);

  // Initialize the CLI and start receiving on the UART
  TaskHandle_t cliTaskHandle;
  ato_cli_t *cli1 = &app_ctx.cli1_ctx;
  ato_cli_create_task(cli1, "CLI1", ato_uart_read_with_notify,
                      ato_uart_write_wait, &app_ctx.uart1_ctx, &cliTaskHandle);

  if (ato_uart_start_rx(&app_ctx.uart1_ctx, cliTaskHandle) != HAL_OK) {
    fatalError("UART1: RX DMA start failed");
  }

  static const ato_analog_config_t analog_cfg = {
      .adc = {&hadc1, &hadc2, &hadc3, &hadc4, &hadc5},
      .dac = {&hdac1},
      .scope_adc = &hadc1, // rank 1 = IN1 (PA0), rank 2 = IN5 (PB14)
      .scope_dac = &hdac1, // OUT1 (PA4)
      .scope_dac_channel = DAC_CHANNEL_1,
      .scope_tim = &htim7,
      .scope_in_label = "adc1.1",
      .scope_out_label = "adc1.5",
  };
  ato_analog_init(&app_ctx.analog_ctx, &analog_cfg, cli1);

  ato_gpio_init(&app_ctx.gpio_ctx, cli1);

  ato_tim_init(&app_ctx.tim_ctx, cli1);
  ato_cli_test_init(&app_ctx.cli_test_ctx, cli1);
  ATO_CLI_REGISTER_TABLE(cli1, cli_commands, app_ctx.cmd_items, NULL);
}

// Temporary: sends back everything received on the CDC interface `argument`.
static void usb_echo_task(void *argument) {
  char buf[64];
  for (;;) {
    size_t n = ato_usb_cdc_read(argument, buf, sizeof(buf));
    ato_usb_cdc_write(argument, buf, n);
  }
}
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

// plain: same columns as vTaskList(), which needs an unbounded output buffer.
// json: {"tasks":[{"name":..,"state":"B","prio":..,"stack":..,"num":..}]}
ATO_CLI_COMMAND_FUNCTION(ps_command) {
  (void)user_data;
  (void)args;
  (void)argc;
  // margin for tasks created between the count and the snapshot
  UBaseType_t size = uxTaskGetNumberOfTasks() + 2;
  TaskStatus_t *tasks = pvPortMalloc(size * sizeof(*tasks));
  if (tasks == NULL) {
    fatalError("ps: allocation failed");
  }
  UBaseType_t count = uxTaskGetSystemState(tasks, size, NULL);

  if (ato_cli_mode(cli) == ATO_CLI_MODE_JSON) {
    ato_cli_reply_begin(cli);
    ato_cli_json_arr_begin(cli, "tasks");
    for (UBaseType_t i = 0; i < count; i++) {
      const TaskStatus_t *t = &tasks[i];
      char state[2] = {task_state_char(t->eCurrentState), '\0'};
      ato_cli_json_obj_begin(cli, NULL);
      ato_cli_reply_str(cli, "name", t->pcTaskName);
      ato_cli_reply_str(cli, "state", state);
      ato_cli_reply_uint(cli, "prio", t->uxCurrentPriority);
      ato_cli_reply_uint(cli, "stack", t->usStackHighWaterMark);
      ato_cli_reply_uint(cli, "num", t->xTaskNumber);
      ato_cli_json_obj_end(cli);
    }
    ato_cli_json_arr_end(cli);
    ato_cli_reply_end(cli);
  } else {
    ato_cli_printf(cli, "%-*s state prio stack num\r\n",
                   configMAX_TASK_NAME_LEN, "name");
    for (UBaseType_t i = 0; i < count; i++) {
      const TaskStatus_t *t = &tasks[i];
      ato_cli_printf(
          cli, "%-*s   %c   %4lu %5u %3lu\r\n", configMAX_TASK_NAME_LEN,
          t->pcTaskName, task_state_char(t->eCurrentState),
          (unsigned long)t->uxCurrentPriority,
          (unsigned)t->usStackHighWaterMark, (unsigned long)t->xTaskNumber);
    }
  }
  vPortFree(tasks);
}

ATO_CLI_COMMAND_FUNCTION(so_command) {
  (void)cli;
  (void)user_data;
  (void)args;
  (void)argc;
  volatile int onStack[512]; // 2 KB: the whole task stack
  for (int i = 0; i < 512; i++) {
    onStack[i] = 0xbaadfeed;
  }
  (void)onStack[0];
}

ATO_CLI_COMMAND_FUNCTION(stats_command) {
  (void)args;
  (void)argc; /*
   const ato_cli_t *ctx = user_data;
   const ato_uart_t *u = &ctx->uart;
   ato_cli_reply_begin(cli);
   ato_cli_reply_uint(cli, "tx_dropped_bytes", u->tx_dropped_bytes);
   ato_cli_reply_uint(cli, "rx_errors", u->rx_errors);
   ato_cli_reply_uint(cli, "tx_errors", u->tx_errors);
   ato_cli_reply_uint(cli, "rx_restart_failures", u->rx_restart_failures);
   ato_cli_reply_end(cli);
   */
}
