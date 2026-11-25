#include "ato_analog.h"

#include "FreeRTOS.h"
#include "ato_adc.h"
#include "ato_cli.h"
#include "ato_error.h"
#include "ato_utils.h"
#include "semphr.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define VREF_MV 3300    // VDDA on the Nucleo board (assumed, not measured)
#define FULL_SCALE 4095 // 12 bits, for both ADC and DAC
#define CENTER_RAW 2048 // waveforms are centered on VREF/2

#define SCOPE_MAX_POINTS ATO_ANALOG_SCOPE_MAX_POINTS
#define SCOPE_MIN_POINTS 16
#define SCOPE_MIN_FS 10
#define SCOPE_MAX_FS 200000   // 2 conversions of ~1.4 us each, with margin
#define SCOPE_MIN_PERIOD 4    // DAC samples per waveform period
#define SCOPE_SETTLE_MS 100   // circuit settling on the first level
#define SCOPE_TIMEOUT_MS 1000 // margin on top of the acquisition duration

typedef enum { SHAPE_SINE, SHAPE_SQUARE, SHAPE_STEP } shape_t;
static const char *const shape_names[] = {"sine", "square", "step"};

ATO_CLI_COMMAND_FUNCTION(adc_command);
ATO_CLI_COMMAND_FUNCTION(adc_list_command);
ATO_CLI_COMMAND_FUNCTION(dac_command);
ATO_CLI_COMMAND_FUNCTION(dac_list_command);
ATO_CLI_COMMAND_FUNCTION(gen_sine_command);
ATO_CLI_COMMAND_FUNCTION(gen_square_command);
ATO_CLI_COMMAND_FUNCTION(gen_step_command);
ATO_CLI_COMMAND_FUNCTION(acq_command);
ATO_CLI_COMMAND_FUNCTION(dump_command);

static const ato_cli_param_t adc_params[] = {
    ATO_CLI_INT("n", "ADC number", 1, ATO_ANALOG_NB_ADC),
    ATO_CLI_INT("ch", "channel", 1, ATO_ADC_MAX_CHANNEL),
};
static const ato_cli_param_t dac_params[] = {
    ATO_CLI_INT("n", "DAC number", 1, ATO_ANALOG_NB_DAC),
    ATO_CLI_INT("ch", "channel", 1, 2),
    ATO_CLI_INT("mv", "output voltage (mV)", 0, VREF_MV),
};
static const ato_cli_param_t gen_periodic_params[] = {
    ATO_CLI_INT("f", "frequency (Hz)", 1, SCOPE_MAX_FS / SCOPE_MIN_PERIOD),
    ATO_CLI_INT("vpp", "peak to peak amplitude (mV)", 0, VREF_MV),
};
static const ato_cli_param_t gen_step_params[] = {
    ATO_CLI_INT("vpp", "step height (mV)", 0, VREF_MV),
};
static const ato_cli_param_t acq_params[] = {
    ATO_CLI_INT("fs", "sample rate (Hz)", SCOPE_MIN_FS, SCOPE_MAX_FS),
    ATO_CLI_INT("n", "number of points", SCOPE_MIN_POINTS, SCOPE_MAX_POINTS),
};

ATO_CLI_TABLE(analog_commands, ATO_ANALOG_COMMANDS);

/* Helpers ------------------------------------------------------------------ */

static uint32_t mv_to_raw(uint32_t mv) {
  if (mv > VREF_MV) {
    mv = VREF_MV;
  }
  return mv * FULL_SCALE / VREF_MV;
}

static uint32_t raw_to_mv(uint32_t raw) { return raw * VREF_MV / FULL_SCALE; }

static uint16_t clamp_raw(int32_t raw) {
  return raw < 0 ? 0 : raw > FULL_SCALE ? FULL_SCALE : (uint16_t)raw;
}

static void dac_set(DAC_HandleTypeDef *hdac, uint32_t channel, uint32_t raw) {
  if (HAL_DAC_SetValue(hdac, channel, DAC_ALIGN_12B_R, raw) != HAL_OK ||
      HAL_DAC_Start(hdac, channel) != HAL_OK) {
    fatalError("DAC set failed");
  }
}

// DAC channel trigger enable (TENx). Must be changed with the channel
// disabled; the trigger source (TSELx) comes from the CubeMX configuration.
static void dac_trigger_enable(DAC_HandleTypeDef *hdac, uint32_t channel,
                               bool enable) {
  uint32_t ten = DAC_CR_TEN1 << (channel & 0x10UL);
  if (enable) {
    SET_BIT(hdac->Instance->CR, ten);
  } else {
    CLEAR_BIT(hdac->Instance->CR, ten);
  }
}

static uint32_t tim_clock_hz(const TIM_HandleTypeDef *htim) {
  const TIM_TypeDef *t = htim->Instance;
  bool apb2 = t == TIM1 || t == TIM8 || t == TIM15 || t == TIM16 ||
              t == TIM17 || t == TIM20;
  // timer clock is twice the APB clock when the APB prescaler is not 1
  if (apb2) {
    uint32_t pclk = HAL_RCC_GetPCLK2Freq();
    return READ_BIT(RCC->CFGR, RCC_CFGR_PPRE2_2) ? 2 * pclk : pclk;
  }
  uint32_t pclk = HAL_RCC_GetPCLK1Freq();
  return READ_BIT(RCC->CFGR, RCC_CFGR_PPRE1_2) ? 2 * pclk : pclk;
}

// Program the timer for `fs` Hz; returns the actual rate in mHz.
static uint64_t tim_set_rate(TIM_HandleTypeDef *htim, uint32_t fs) {
  uint32_t clk = tim_clock_hz(htim);
  uint32_t psc = (clk / fs - 1) / 65536;
  uint32_t arr = clk / ((psc + 1) * fs) - 1;
  __HAL_TIM_SET_PRESCALER(htim, psc);
  __HAL_TIM_SET_AUTORELOAD(htim, arr);
  // load the prescaler now (it is buffered); the DAC and the ADC are not
  // armed yet, so the TRGO of this update event is ignored
  htim->Instance->EGR = TIM_EGR_UG;
  return (uint64_t)clk * 1000 / ((uint64_t)(psc + 1) * (arr + 1));
}

/* Unit commands ------------------------------------------------------------ */

ATO_CLI_COMMAND_FUNCTION(adc_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)argc;
  uint32_t n = args[0].i, ch = args[1].i;
  if (ctx->cfg->adc[n - 1] == NULL) {
    ato_cli_reply_error(cli, "no ADC%lu", (unsigned long)n);
    return;
  }
  ato_adc_t *a = &ctx->adc[n - 1];
  if (!ato_adc_is_software_triggered(a)) {
    ato_cli_reply_error(cli, "ADC%lu is timer triggered (scope)",
                        (unsigned long)n);
    return;
  }

  uint16_t raw;
  bool ok = ato_adc_read(a, ch, &raw);

  if (!ok) {
    ato_cli_reply_error(cli, "invalid channel %lu", (unsigned long)ch);
    return;
  }
  ato_cli_reply_begin(cli);
  ato_cli_reply_uint(cli, "raw", raw);
  ato_cli_reply_uint(cli, "mv", raw_to_mv(raw));
  ato_cli_reply_end(cli);
}

// plain: one line per ADC, "ADC3 software IN1"
// json: {"adcs":[{"adc":3,"trigger":"software","channels":[1]},...]}
ATO_CLI_COMMAND_FUNCTION(adc_list_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)args;
  (void)argc;
  bool json = ato_cli_mode(cli) == ATO_CLI_MODE_JSON;
  if (json) {
    ato_cli_reply_begin(cli);
    ato_cli_json_arr_begin(cli, "adcs");
  }
  for (uint32_t i = 0; i < ATO_ANALOG_NB_ADC; i++) {
    if (ctx->cfg->adc[i] == NULL) {
      continue;
    }
    const ato_adc_t *a = &ctx->adc[i];
    const char *trigger =
        ato_adc_is_software_triggered(a) ? "software" : "timer";
    uint32_t channels = ato_adc_channels(a);
    if (json) {
      ato_cli_json_obj_begin(cli, NULL);
      ato_cli_reply_uint(cli, "adc", i + 1);
      ato_cli_reply_str(cli, "trigger", trigger);
      ato_cli_json_arr_begin(cli, "channels");
    } else {
      ato_cli_printf(cli, "ADC%lu %-8s", (unsigned long)(i + 1), trigger);
    }
    for (uint32_t ch = 0; ch < 32; ch++) {
      if (channels & (1UL << ch)) {
        if (json) {
          ato_cli_reply_uint(cli, NULL, ch);
        } else {
          ato_cli_printf(cli, " IN%lu", (unsigned long)ch);
        }
      }
    }
    if (json) {
      ato_cli_json_arr_end(cli);
      ato_cli_json_obj_end(cli);
    } else {
      ato_cli_puts(cli, "\r\n");
    }
  }
  if (json) {
    ato_cli_json_arr_end(cli);
    ato_cli_reply_end(cli);
  }
}

// Channels of the DACs, enabled or not (EN bit, set by HAL_DAC_Start), and
// their output (DOR). Whether a channel is configured in CubeMX cannot be
// read back (its mode register is the reset value): the UI also checks that
// its pin is in analog mode.
// plain: one line per channel, "DAC1 OUT1 on raw=1240 mv=999"
// json: {"outputs":[{"dac":1,"ch":1,"enabled":true,"raw":1240,"mv":999},...]}
ATO_CLI_COMMAND_FUNCTION(dac_list_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)args;
  (void)argc;
  static const uint32_t channels[] = {DAC_CHANNEL_1, DAC_CHANNEL_2};
  static const uint32_t enable[] = {DAC_CR_EN1, DAC_CR_EN2};
  bool json = ato_cli_mode(cli) == ATO_CLI_MODE_JSON;
  if (json) {
    ato_cli_reply_begin(cli);
    ato_cli_json_arr_begin(cli, "outputs");
  }
  for (uint32_t i = 0; i < ATO_ANALOG_NB_DAC; i++) {
    DAC_HandleTypeDef *hdac = ctx->cfg->dac[i];
    if (hdac == NULL) {
      continue;
    }
    for (uint32_t c = 0; c < COUNT_OF(channels); c++) {
      bool enabled = READ_BIT(hdac->Instance->CR, enable[c]) != 0;
      uint32_t raw = HAL_DAC_GetValue(hdac, channels[c]);
      if (json) {
        ato_cli_json_obj_begin(cli, NULL);
        ato_cli_reply_uint(cli, "dac", i + 1);
        ato_cli_reply_uint(cli, "ch", c + 1);
        ato_cli_reply_bool(cli, "enabled", enabled);
        ato_cli_reply_uint(cli, "raw", raw);
        ato_cli_reply_uint(cli, "mv", raw_to_mv(raw));
        ato_cli_json_obj_end(cli);
      } else {
        ato_cli_printf(cli, "DAC%lu OUT%lu %-3s raw=%lu mv=%lu\r\n",
                       (unsigned long)(i + 1), (unsigned long)(c + 1),
                       enabled ? "on" : "off", (unsigned long)raw,
                       (unsigned long)raw_to_mv(raw));
      }
    }
  }
  if (json) {
    ato_cli_json_arr_end(cli);
    ato_cli_reply_end(cli);
  }
}

ATO_CLI_COMMAND_FUNCTION(dac_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)argc;
  uint32_t n = args[0].i, mv = args[2].i;
  uint32_t channel = args[1].i == 1 ? DAC_CHANNEL_1 : DAC_CHANNEL_2;
  if (ctx->cfg->dac[n - 1] == NULL) {
    ato_cli_reply_error(cli, "no DAC%lu", (unsigned long)n);
    return;
  }

  uint32_t raw = mv_to_raw(mv);
  // the scope channel is only triggered during "acq", which holds the lock
  xSemaphoreTake(ctx->lock, portMAX_DELAY);
  dac_set(ctx->cfg->dac[n - 1], channel, raw);
  xSemaphoreGive(ctx->lock);

  ato_cli_reply_begin(cli);
  ato_cli_reply_uint(cli, "raw", raw);
  ato_cli_reply_uint(cli, "mv", raw_to_mv(raw));
  ato_cli_reply_end(cli);
}

/* Scope commands ----------------------------------------------------------- */

static void gen_set(ato_cli_t *cli, ato_analog_ctx_t *ctx, shape_t shape,
                    uint32_t freq, uint32_t vpp) {
  xSemaphoreTake(ctx->lock, portMAX_DELAY);
  ctx->shape = shape;
  ctx->freq_hz = freq;
  ctx->vpp_mv = vpp;
  xSemaphoreGive(ctx->lock);

  ato_cli_reply_begin(cli);
  ato_cli_reply_str(cli, "shape", shape_names[shape]);
  ato_cli_reply_uint(cli, "f", freq);
  ato_cli_reply_uint(cli, "vpp", vpp);
  ato_cli_reply_end(cli);
}

ATO_CLI_COMMAND_FUNCTION(gen_sine_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)argc;
  gen_set(cli, ctx, SHAPE_SINE, args[0].i, args[1].i);
}

ATO_CLI_COMMAND_FUNCTION(gen_square_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)argc;
  gen_set(cli, ctx, SHAPE_SQUARE, args[0].i, args[1].i);
}

ATO_CLI_COMMAND_FUNCTION(gen_step_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)argc;
  gen_set(cli, ctx, SHAPE_STEP, 0, args[0].i);
}

// Fill dac_table for the current waveform; returns its length (0: the
// frequency does not fit with this sample rate).
static uint32_t build_table(ato_analog_ctx_t *ctx, uint32_t fs, uint32_t n) {
  int32_t half = (int32_t)(mv_to_raw(ctx->vpp_mv) / 2);
  uint16_t low = clamp_raw(CENTER_RAW - half);
  uint16_t high = clamp_raw(CENTER_RAW + half);

  if (ctx->shape == SHAPE_STEP) {
    // first level long enough to see the circuit at rest, then the step
    for (uint32_t i = 0; i < n; i++) {
      ctx->dac_table[i] = i < n / 8 ? low : high;
    }
    return n;
  }

  uint32_t len = (fs + ctx->freq_hz / 2) / ctx->freq_hz; // rounded
  if (len < SCOPE_MIN_PERIOD || len > SCOPE_MAX_POINTS) {
    return 0;
  }
  for (uint32_t i = 0; i < len; i++) {
    if (ctx->shape == SHAPE_SINE) {
      float s = sinf(2.0f * (float)M_PI * (float)i / (float)len);
      ctx->dac_table[i] =
          clamp_raw(CENTER_RAW + (int32_t)lrintf(s * (float)half));
    } else {
      ctx->dac_table[i] = i < len / 2 ? low : high;
    }
  }
  return len;
}

ATO_CLI_COMMAND_FUNCTION(acq_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)argc;
  const ato_analog_config_t *cfg = ctx->cfg;
  uint32_t fs = args[0].i, n = args[1].i;

  xSemaphoreTake(ctx->lock, portMAX_DELAY);

  uint32_t len = build_table(ctx, fs, n);
  if (len == 0) {
    xSemaphoreGive(ctx->lock);
    ato_cli_reply_error(cli, "f must be between fs/%u and fs/%u",
                        SCOPE_MAX_POINTS, SCOPE_MIN_PERIOD);
    return;
  }

  // let the circuit settle on the first level
  dac_set(cfg->scope_dac, cfg->scope_dac_channel, ctx->dac_table[0]);
  vTaskDelay(pdMS_TO_TICKS(SCOPE_SETTLE_MS));

  ctx->n = 0; // no valid data until the acquisition is complete
  ctx->fs_mhz = tim_set_rate(cfg->scope_tim, fs);
  ctx->f_mhz = ctx->shape == SHAPE_STEP ? 0 : ctx->fs_mhz / len;

  // DAC paced by the timer, table repeated by the circular DMA
  if (HAL_DAC_Stop(cfg->scope_dac, cfg->scope_dac_channel) != HAL_OK) {
    fatalError("DAC stop failed");
  }
  dac_trigger_enable(cfg->scope_dac, cfg->scope_dac_channel, true);
  if (HAL_DAC_Start_DMA(cfg->scope_dac, cfg->scope_dac_channel,
                        (uint32_t *)ctx->dac_table, len,
                        DAC_ALIGN_12B_R) != HAL_OK) {
    fatalError("DAC DMA start failed");
  }

  // ADC armed on the same trigger, then the timer starts both
  ato_adc_start_dma(ctx->scope_adc, ctx->samples, 2 * n);
  if (HAL_TIM_Base_Start(cfg->scope_tim) != HAL_OK) {
    fatalError("scope timer start failed");
  }
  ato_adc_wait_dma(ctx->scope_adc,
                   pdMS_TO_TICKS((uint64_t)n * 1000 / fs + SCOPE_TIMEOUT_MS));

  // back to idle: timer stopped, DAC static on the first level
  HAL_TIM_Base_Stop(cfg->scope_tim);
  if (HAL_DAC_Stop_DMA(cfg->scope_dac, cfg->scope_dac_channel) != HAL_OK) {
    fatalError("DAC DMA stop failed");
  }
  dac_trigger_enable(cfg->scope_dac, cfg->scope_dac_channel, false);
  dac_set(cfg->scope_dac, cfg->scope_dac_channel, ctx->dac_table[0]);

  ctx->n = n;
  uint64_t fs_mhz = ctx->fs_mhz, f_mhz = ctx->f_mhz;
  xSemaphoreGive(ctx->lock);

  ato_cli_reply_begin(cli);
  ato_cli_reply_milli(cli, "fs", fs_mhz);
  ato_cli_reply_milli(cli, "f", f_mhz);
  ato_cli_reply_uint(cli, "n", n);
  ato_cli_reply_end(cli);
}

ATO_CLI_COMMAND_FUNCTION(dump_command) {
  ato_analog_ctx_t *ctx = user_data;
  (void)args;
  (void)argc;
  const ato_analog_config_t *cfg = ctx->cfg;

  // held during the whole output: the data cannot change meanwhile
  xSemaphoreTake(ctx->lock, portMAX_DELAY);
  if (ctx->n == 0) {
    xSemaphoreGive(ctx->lock);
    ato_cli_reply_error(cli, "no data, run acq first");
    return;
  }

  if (ato_cli_mode(cli) == ATO_CLI_MODE_JSON) {
    // {"fs":..,"n":..,...,"in":[...],"out":[...],"ok":true}
    ato_cli_reply_begin(cli);
    ato_cli_reply_milli(cli, "fs", ctx->fs_mhz);
    ato_cli_reply_uint(cli, "n", ctx->n);
    ato_cli_reply_milli(cli, "f", ctx->f_mhz);
    ato_cli_reply_str(cli, "shape", shape_names[ctx->shape]);
    ato_cli_reply_uint(cli, "vpp", ctx->vpp_mv);
    ato_cli_reply_uint(cli, "vref", VREF_MV);
    ato_cli_reply_uint(cli, "full", FULL_SCALE);
    ato_cli_reply_str(cli, "in_label", cfg->scope_in_label);
    ato_cli_reply_str(cli, "out_label", cfg->scope_out_label);
    for (uint32_t k = 0; k < 2; k++) { // k = 0: in, 1: out (interleaved)
      ato_cli_json_arr_begin(cli, k == 0 ? "in" : "out");
      for (uint32_t i = 0; i < ctx->n; i++) {
        ato_cli_reply_uint(cli, NULL, ctx->samples[2 * i + k]);
      }
      ato_cli_json_arr_end(cli);
    }
    ato_cli_reply_end(cli);
    xSemaphoreGive(ctx->lock);
    return;
  }

  ato_cli_printf(cli,
                 "#scope fs=%lu.%03lu n=%lu f=%lu.%03lu shape=%s vpp=%lu "
                 "vref=%u full=%u in=%s out=%s\r\n",
                 (unsigned long)(ctx->fs_mhz / 1000),
                 (unsigned long)(ctx->fs_mhz % 1000), (unsigned long)ctx->n,
                 (unsigned long)(ctx->f_mhz / 1000),
                 (unsigned long)(ctx->f_mhz % 1000), shape_names[ctx->shape],
                 (unsigned long)ctx->vpp_mv, VREF_MV, FULL_SCALE,
                 cfg->scope_in_label, cfg->scope_out_label);
  for (uint32_t i = 0; i < ctx->n; i++) {
    const uint16_t *p = &ctx->samples[2 * i];
    ato_cli_printf(cli, "%03X,%03X\r\n", p[0], p[1]);
  }
  ato_cli_puts(cli, "#end\r\n");
  xSemaphoreGive(ctx->lock);
}

/* Setup -------------------------------------------------------------------- */

void ato_analog_init(ato_analog_ctx_t *ctx, const ato_analog_config_t *cfg,
                     ato_cli_t *cli) {
  configASSERT(ctx != NULL);
  configASSERT(cfg != NULL);
  configASSERT(cfg->scope_dac != NULL);
  configASSERT(cfg->scope_tim != NULL);

  memset(ctx, 0, sizeof(*ctx));
  ctx->cfg = cfg;
  for (size_t i = 0; i < ATO_ANALOG_NB_ADC; i++) {
    if (cfg->adc[i] != NULL) {
      ato_adc_init(&ctx->adc[i], cfg->adc[i]);
      if (cfg->adc[i] == cfg->scope_adc) {
        ctx->scope_adc = &ctx->adc[i];
      }
    }
  }
  configASSERT(ctx->scope_adc != NULL); // scope_adc must be listed in adc[]

  ctx->lock = xSemaphoreCreateMutex();
  if (ctx->lock == NULL) {
    fatalError("analog: mutex allocation failed");
  }

  // default waveform
  ctx->shape = SHAPE_STEP;
  ctx->vpp_mv = 2000;

  // the scope DAC channel is only triggered during an acquisition; at rest
  // it is static, so that "dac" works on it
  dac_trigger_enable(cfg->scope_dac, cfg->scope_dac_channel, false);
  dac_set(cfg->scope_dac, cfg->scope_dac_channel, CENTER_RAW);

  for (size_t i = 0; i < COUNT_OF(analog_commands); i++) {
    ato_cli_register_command(cli, &analog_commands[i], &ctx->cmd_items[i], ctx);
  }
}
