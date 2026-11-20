#include "ato_analog.h"

#include "FreeRTOS.h"
#include "ato_adc.h"
#include "ato_cli_core.h"
#include "ato_error.h"
#include "ato_utils.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>

#define ANALOG_STACK_WORDS 256
#define ANALOG_PRIORITY (tskIDLE_PRIORITY + 1)
#define ANALOG_REPLY_TIMEOUT_MS 100

#define VREF_MV 3300   // VDDA on the Nucleo board (assumed, not measured)
#define FULL_SCALE 4095 // 12 bits, for both ADC and DAC

typedef enum { ANALOG_READ, ANALOG_SET_DAC } analog_op_t;

typedef struct {
  analog_op_t op;
  uint32_t arg; // ANALOG_SET_DAC: raw DAC value
} analog_request_t;

static BaseType_t adc_command(ato_cli_core_t *cli, char *out, size_t out_len,
                              const char *cmd_string);
static BaseType_t dac_command(ato_cli_core_t *cli, char *out, size_t out_len,
                              const char *cmd_string);

static const ato_cli_command_t analog_commands[] = {
    {"adc", "\r\nadc:\r\n Read the ADC (raw and mV)\r\n\r\n", adc_command, 0},
    {"dac", "\r\ndac <mV>:\r\n Set the DAC output (0..3300 mV)\r\n\r\n",
     dac_command, 1}};

typedef struct {
  ato_adc_t adc;
  DAC_HandleTypeDef *hdac;
  uint32_t dac_channel;

  QueueHandle_t requests;
  SemaphoreHandle_t lock;  // one request at a time (several CLIs may ask)
  SemaphoreHandle_t reply; // given by the task when a request is done
  uint32_t result;         // ANALOG_READ: raw ADC value

  ato_cli_list_item_t cmd_items[COUNT_OF(analog_commands)];
} analog_ctx_t;

static analog_ctx_t s_ctx;

/* Requests (called from the CLI tasks) ------------------------------------- */

static uint32_t analog_request(analog_op_t op, uint32_t arg) {
  const analog_request_t req = {op, arg};

  xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
  xQueueSend(s_ctx.requests, &req, portMAX_DELAY);
  if (xSemaphoreTake(s_ctx.reply, pdMS_TO_TICKS(ANALOG_REPLY_TIMEOUT_MS)) !=
      pdTRUE) {
    fatalError("analog task does not answer");
  }
  uint32_t result = s_ctx.result;
  xSemaphoreGive(s_ctx.lock);
  return result;
}

/* CLI commands ------------------------------------------------------------- */

static BaseType_t adc_command(ato_cli_core_t *cli, char *out, size_t out_len,
                              const char *cmd_string) {
  (void)cli;
  (void)cmd_string;
  uint32_t raw = analog_request(ANALOG_READ, 0);
  snprintf(out, out_len, "adc: raw %lu, %lu mV\r\n", (unsigned long)raw,
           (unsigned long)(raw * VREF_MV / FULL_SCALE));
  return pdFALSE;
}

static BaseType_t dac_command(ato_cli_core_t *cli, char *out, size_t out_len,
                              const char *cmd_string) {
  (void)cli;
  BaseType_t len;
  const char *param = ato_cli_core_get_parameter(cmd_string, 1, &len);
  char *end;
  unsigned long mv = strtoul(param, &end, 10);
  if (end != param + len) {
    snprintf(out, out_len, "dac: invalid value\r\n");
    return pdFALSE;
  }
  if (mv > VREF_MV) {
    mv = VREF_MV;
  }
  uint32_t raw = mv * FULL_SCALE / VREF_MV;
  analog_request(ANALOG_SET_DAC, raw);
  snprintf(out, out_len, "dac: raw %lu, %lu mV\r\n", (unsigned long)raw, mv);
  return pdFALSE;
}

/* Task --------------------------------------------------------------------- */

static void set_dac(uint32_t raw) {
  if (HAL_DAC_SetValue(s_ctx.hdac, s_ctx.dac_channel, DAC_ALIGN_12B_R, raw) !=
      HAL_OK) {
    fatalError("DAC set value failed");
  }
}

static void analog_task(void *argument) {
  ADC_HandleTypeDef *hadc = argument;

  // done here, once the scheduler runs (HAL tick needed); requests are only
  // read below, so no command can reach the ADC before its calibration
  ato_adc_init(&s_ctx.adc, hadc);
  set_dac(0);
  if (HAL_DAC_Start(s_ctx.hdac, s_ctx.dac_channel) != HAL_OK) {
    fatalError("DAC start failed");
  }

  for (;;) {
    analog_request_t req;
    xQueueReceive(s_ctx.requests, &req, portMAX_DELAY);
    switch (req.op) {
    case ANALOG_READ:
      s_ctx.result = ato_adc_read(&s_ctx.adc);
      break;
    case ANALOG_SET_DAC:
      set_dac(req.arg);
      break;
    }
    xSemaphoreGive(s_ctx.reply);
  }
}

void ato_analog_create_task(ADC_HandleTypeDef *hadc, DAC_HandleTypeDef *hdac,
                            uint32_t dac_channel, ato_cli_ctx_t *cli) {
  configASSERT(s_ctx.hdac == NULL); // single instance

  configASSERT(hadc != NULL);
  configASSERT(hdac != NULL);
  s_ctx.hdac = hdac;
  s_ctx.dac_channel = dac_channel;

  s_ctx.requests = xQueueCreate(1, sizeof(analog_request_t));
  s_ctx.lock = xSemaphoreCreateMutex();
  s_ctx.reply = xSemaphoreCreateBinary();
  if (s_ctx.requests == NULL || s_ctx.lock == NULL || s_ctx.reply == NULL) {
    fatalError("analog: allocation failed");
  }

  if (xTaskCreate(analog_task, "ANALOG", ANALOG_STACK_WORDS, hadc,
                  ANALOG_PRIORITY, NULL) != pdPASS) {
    fatalError("Task creation failed for task ANALOG");
  }

  for (size_t i = 0; i < COUNT_OF(analog_commands); i++) {
    ato_cli_register_command(cli, &analog_commands[i], &s_ctx.cmd_items[i]);
  }
}
