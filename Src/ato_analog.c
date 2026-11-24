#include "ato_analog.h"

#include "FreeRTOS.h"
#include "ato_adc.h"
#include "ato_cli_core.h"
#include "ato_error.h"
#include "ato_utils.h"
#include "semphr.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#define VREF_MV 3300    // VDDA on the Nucleo board (assumed, not measured)
#define FULL_SCALE 4095 // 12 bits, for both ADC and DAC
#define CENTER_RAW 2048 // waveforms are centered on VREF/2

#define SCOPE_MAX_POINTS 4096
#define SCOPE_MIN_POINTS 16
#define SCOPE_MIN_FS 10
#define SCOPE_MAX_FS 200000   // 2 conversions of ~1.4 us each, with margin
#define SCOPE_MIN_PERIOD 4    // DAC samples per waveform period
#define SCOPE_SETTLE_MS 100   // circuit settling on the first level
#define SCOPE_TIMEOUT_MS 1000 // margin on top of the acquisition duration

typedef enum { SHAPE_SINE, SHAPE_SQUARE, SHAPE_STEP } shape_t;
static const char *const shape_names[] = {"sine", "square", "step"};

#define ANALOG_HANDLER(fn)                                                     \
  static void fn(ato_cli_core_t *cli, const ato_cli_arg_t *args, uint8_t argc)
ANALOG_HANDLER(adc_command);
ANALOG_HANDLER(dac_command);
ANALOG_HANDLER(gen_sine_command);
ANALOG_HANDLER(gen_square_command);
ANALOG_HANDLER(gen_step_command);
ANALOG_HANDLER(acq_command);
ANALOG_HANDLER(dump_command);

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

#define ANALOG_COMMANDS(X)                                                     \
  X("adc", "Read an ADC channel (raw and mV)", adc_command, adc_params)        \
  X("dac", "Set a DAC channel", dac_command, dac_params)                       \
  X("gen sine", "Sine of the next acquisition, centered on 1650 mV",           \
    gen_sine_command, gen_periodic_params)                                     \
  X("gen square", "Square of the next acquisition, centered on 1650 mV",       \
    gen_square_command, gen_periodic_params)                                   \
  X("gen step", "Step of the next acquisition, centered on 1650 mV",           \
    gen_step_command, gen_step_params)                                         \
  X("acq", "Generate the waveform and acquire n points at fs Hz", acq_command, \
    acq_params)                                                                \
  X("dump", "Dump the last acquisition (hex CSV in,out)", dump_command)

ATO_CLI_TABLE(analog_commands, ANALOG_COMMANDS);

typedef struct {
  const ato_analog_config_t *cfg;
  ato_adc_t adc[ATO_ANALOG_NB_ADC];
  ato_adc_t *scope_adc;
  SemaphoreHandle_t lock; // analog hardware and scope data

  // waveform, set by "gen"
  shape_t shape;
  uint32_t freq_hz;
  uint32_t vpp_mv;

  // last acquisition
  uint32_t n;      // number of points, 0: no data
  uint64_t fs_mhz; // actual sample rate, in mHz
  uint64_t f_mhz;  // actual waveform frequency, in mHz (0 for step)

  ato_cli_list_item_t cmd_items[COUNT_OF(analog_commands)];
} analog_ctx_t;

static analog_ctx_t s_ctx;

static uint16_t dac_table[SCOPE_MAX_POINTS];
static uint16_t samples[2 * SCOPE_MAX_POINTS]; // in, out interleaved

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

ANALOG_HANDLER(adc_command) {
  (void)argc;
  uint32_t n = args[0].i, ch = args[1].i;
  if (s_ctx.cfg->adc[n - 1] == NULL) {
    ato_cli_core_printf(cli, "error: no ADC%lu\r\n", (unsigned long)n);
    return;
  }
  ato_adc_t *a = &s_ctx.adc[n - 1];
  if (!ato_adc_is_software_triggered(a)) {
    ato_cli_core_printf(cli, "error: ADC%lu is timer triggered (scope)\r\n",
                        (unsigned long)n);
    return;
  }

  uint16_t raw;
  xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
  bool ok = ato_adc_read(a, ch, &raw);
  xSemaphoreGive(s_ctx.lock);

  if (!ok) {
    ato_cli_core_printf(cli, "error: invalid channel %lu\r\n",
                        (unsigned long)ch);
  } else {
    ato_cli_core_printf(cli, "ok raw=%u mv=%lu\r\n", raw,
                        (unsigned long)raw_to_mv(raw));
  }
}

ANALOG_HANDLER(dac_command) {
  (void)argc;
  uint32_t n = args[0].i, mv = args[2].i;
  uint32_t channel = args[1].i == 1 ? DAC_CHANNEL_1 : DAC_CHANNEL_2;
  if (s_ctx.cfg->dac[n - 1] == NULL) {
    ato_cli_core_printf(cli, "error: no DAC%lu\r\n", (unsigned long)n);
    return;
  }

  uint32_t raw = mv_to_raw(mv);
  // the scope channel is only triggered during "acq", which holds the lock
  xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
  dac_set(s_ctx.cfg->dac[n - 1], channel, raw);
  xSemaphoreGive(s_ctx.lock);

  ato_cli_core_printf(cli, "ok raw=%lu mv=%lu\r\n", (unsigned long)raw,
                      (unsigned long)raw_to_mv(raw));
}

/* Scope commands ----------------------------------------------------------- */

static void gen_set(ato_cli_core_t *cli, shape_t shape, uint32_t freq,
                    uint32_t vpp) {
  xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
  s_ctx.shape = shape;
  s_ctx.freq_hz = freq;
  s_ctx.vpp_mv = vpp;
  xSemaphoreGive(s_ctx.lock);

  ato_cli_core_printf(cli, "ok shape=%s f=%lu vpp=%lu\r\n", shape_names[shape],
                      (unsigned long)freq, (unsigned long)vpp);
}

ANALOG_HANDLER(gen_sine_command) {
  (void)argc;
  gen_set(cli, SHAPE_SINE, args[0].i, args[1].i);
}

ANALOG_HANDLER(gen_square_command) {
  (void)argc;
  gen_set(cli, SHAPE_SQUARE, args[0].i, args[1].i);
}

ANALOG_HANDLER(gen_step_command) {
  (void)argc;
  gen_set(cli, SHAPE_STEP, 0, args[0].i);
}

// Fill dac_table for the current waveform; returns its length (0: the
// frequency does not fit with this sample rate).
static uint32_t build_table(uint32_t fs, uint32_t n) {
  int32_t half = (int32_t)(mv_to_raw(s_ctx.vpp_mv) / 2);
  uint16_t low = clamp_raw(CENTER_RAW - half);
  uint16_t high = clamp_raw(CENTER_RAW + half);

  if (s_ctx.shape == SHAPE_STEP) {
    // first level long enough to see the circuit at rest, then the step
    for (uint32_t i = 0; i < n; i++) {
      dac_table[i] = i < n / 8 ? low : high;
    }
    return n;
  }

  uint32_t len = (fs + s_ctx.freq_hz / 2) / s_ctx.freq_hz; // rounded
  if (len < SCOPE_MIN_PERIOD || len > SCOPE_MAX_POINTS) {
    return 0;
  }
  for (uint32_t i = 0; i < len; i++) {
    if (s_ctx.shape == SHAPE_SINE) {
      float s = sinf(2.0f * (float)M_PI * (float)i / (float)len);
      dac_table[i] = clamp_raw(CENTER_RAW + (int32_t)lrintf(s * (float)half));
    } else {
      dac_table[i] = i < len / 2 ? low : high;
    }
  }
  return len;
}

ANALOG_HANDLER(acq_command) {
  (void)argc;
  const ato_analog_config_t *cfg = s_ctx.cfg;
  uint32_t fs = args[0].i, n = args[1].i;

  xSemaphoreTake(s_ctx.lock, portMAX_DELAY);

  uint32_t len = build_table(fs, n);
  if (len == 0) {
    xSemaphoreGive(s_ctx.lock);
    ato_cli_core_printf(cli, "error: f must be between fs/%u and fs/%u\r\n",
                        SCOPE_MAX_POINTS, SCOPE_MIN_PERIOD);
    return;
  }

  // let the circuit settle on the first level
  dac_set(cfg->scope_dac, cfg->scope_dac_channel, dac_table[0]);
  vTaskDelay(pdMS_TO_TICKS(SCOPE_SETTLE_MS));

  s_ctx.n = 0; // no valid data until the acquisition is complete
  s_ctx.fs_mhz = tim_set_rate(cfg->scope_tim, fs);
  s_ctx.f_mhz = s_ctx.shape == SHAPE_STEP ? 0 : s_ctx.fs_mhz / len;

  // DAC paced by the timer, table repeated by the circular DMA
  if (HAL_DAC_Stop(cfg->scope_dac, cfg->scope_dac_channel) != HAL_OK) {
    fatalError("DAC stop failed");
  }
  dac_trigger_enable(cfg->scope_dac, cfg->scope_dac_channel, true);
  if (HAL_DAC_Start_DMA(cfg->scope_dac, cfg->scope_dac_channel,
                        (uint32_t *)dac_table, len,
                        DAC_ALIGN_12B_R) != HAL_OK) {
    fatalError("DAC DMA start failed");
  }

  // ADC armed on the same trigger, then the timer starts both
  ato_adc_start_dma(s_ctx.scope_adc, samples, 2 * n);
  if (HAL_TIM_Base_Start(cfg->scope_tim) != HAL_OK) {
    fatalError("scope timer start failed");
  }
  ato_adc_wait_dma(s_ctx.scope_adc,
                   pdMS_TO_TICKS((uint64_t)n * 1000 / fs + SCOPE_TIMEOUT_MS));

  // back to idle: timer stopped, DAC static on the first level
  HAL_TIM_Base_Stop(cfg->scope_tim);
  if (HAL_DAC_Stop_DMA(cfg->scope_dac, cfg->scope_dac_channel) != HAL_OK) {
    fatalError("DAC DMA stop failed");
  }
  dac_trigger_enable(cfg->scope_dac, cfg->scope_dac_channel, false);
  dac_set(cfg->scope_dac, cfg->scope_dac_channel, dac_table[0]);

  s_ctx.n = n;
  uint64_t fs_mhz = s_ctx.fs_mhz, f_mhz = s_ctx.f_mhz;
  xSemaphoreGive(s_ctx.lock);

  ato_cli_core_printf(cli, "ok fs=%lu.%03lu f=%lu.%03lu n=%lu\r\n",
                      (unsigned long)(fs_mhz / 1000),
                      (unsigned long)(fs_mhz % 1000),
                      (unsigned long)(f_mhz / 1000),
                      (unsigned long)(f_mhz % 1000), (unsigned long)n);
}

ANALOG_HANDLER(dump_command) {
  (void)args;
  (void)argc;
  const ato_analog_config_t *cfg = s_ctx.cfg;

  // held during the whole output: the data cannot change meanwhile
  xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
  if (s_ctx.n == 0) {
    xSemaphoreGive(s_ctx.lock);
    ato_cli_core_printf(cli, "error: no data, run acq first\r\n");
    return;
  }
  ato_cli_core_printf(
      cli,
      "#scope fs=%lu.%03lu n=%lu f=%lu.%03lu shape=%s vpp=%lu "
      "vref=%u full=%u in=%s out=%s\r\n",
      (unsigned long)(s_ctx.fs_mhz / 1000),
      (unsigned long)(s_ctx.fs_mhz % 1000), (unsigned long)s_ctx.n,
      (unsigned long)(s_ctx.f_mhz / 1000), (unsigned long)(s_ctx.f_mhz % 1000),
      shape_names[s_ctx.shape], (unsigned long)s_ctx.vpp_mv, VREF_MV,
      FULL_SCALE, cfg->scope_in_label, cfg->scope_out_label);
  for (uint32_t i = 0; i < s_ctx.n; i++) {
    const uint16_t *p = &samples[2 * i];
    ato_cli_core_printf(cli, "%03X,%03X\r\n", p[0], p[1]);
  }
  ato_cli_core_puts(cli, "#end\r\n");
  xSemaphoreGive(s_ctx.lock);
}

/* Setup -------------------------------------------------------------------- */

void ato_analog_init(const ato_analog_config_t *cfg, ato_cli_ctx_t *cli) {
  configASSERT(cfg != NULL);
  configASSERT(s_ctx.cfg == NULL); // single instance
  configASSERT(cfg->scope_dac != NULL);
  configASSERT(cfg->scope_tim != NULL);

  s_ctx.cfg = cfg;
  for (size_t i = 0; i < ATO_ANALOG_NB_ADC; i++) {
    if (cfg->adc[i] != NULL) {
      ato_adc_init(&s_ctx.adc[i], cfg->adc[i]);
      if (cfg->adc[i] == cfg->scope_adc) {
        s_ctx.scope_adc = &s_ctx.adc[i];
      }
    }
  }
  configASSERT(s_ctx.scope_adc != NULL); // scope_adc must be listed in adc[]

  s_ctx.lock = xSemaphoreCreateMutex();
  if (s_ctx.lock == NULL) {
    fatalError("analog: mutex allocation failed");
  }

  // default waveform
  s_ctx.shape = SHAPE_STEP;
  s_ctx.vpp_mv = 2000;

  // the scope DAC channel is only triggered during an acquisition; at rest
  // it is static, so that "dac" works on it
  dac_trigger_enable(cfg->scope_dac, cfg->scope_dac_channel, false);
  dac_set(cfg->scope_dac, cfg->scope_dac_channel, CENTER_RAW);

  for (size_t i = 0; i < COUNT_OF(analog_commands); i++) {
    ato_cli_register_command(cli, &analog_commands[i], &s_ctx.cmd_items[i]);
  }
}
