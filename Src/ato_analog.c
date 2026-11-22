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
#include <stdlib.h>
#include <string.h>

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

static void adc_command(ato_cli_core_t *cli, const char *cmd_string);
static void dac_command(ato_cli_core_t *cli, const char *cmd_string);
static void gen_command(ato_cli_core_t *cli, const char *cmd_string);
static void acq_command(ato_cli_core_t *cli, const char *cmd_string);
static void dump_command(ato_cli_core_t *cli, const char *cmd_string);

static const ato_cli_command_t analog_commands[] = {
    {"adc", "\r\nadc <n> <ch>:\r\n Read ADCn channel ch (raw and mV)\r\n\r\n",
     adc_command, 2},
    {"dac",
     "\r\ndac <n> <ch> <mV>:\r\n Set DACn channel ch (0..3300 mV)\r\n\r\n",
     dac_command, 3},
    {"gen",
     "\r\ngen sine|square <Hz> <mVpp> | gen step <mVpp>:\r\n Waveform of the "
     "next acquisition, centered on 1650 mV\r\n\r\n",
     gen_command, -1},
    {"acq",
     "\r\nacq <fs> <n>:\r\n Generate and acquire n points at fs Hz\r\n\r\n",
     acq_command, 2},
    {"dump", "\r\ndump:\r\n Dump the last acquisition (hex CSV in,out)\r\n\r\n",
     dump_command, 0}};

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

// Parse parameter `index` as an unsigned decimal number.
static bool param_uint(const char *cmd_string, UBaseType_t index,
                       uint32_t *value) {
  BaseType_t len;
  const char *p = ato_cli_core_get_parameter(cmd_string, index, &len);
  if (p == NULL) {
    return false;
  }
  char *end;
  unsigned long v = strtoul(p, &end, 10);
  if (end != p + len) {
    return false;
  }
  *value = v;
  return true;
}

static bool param_is(const char *cmd_string, UBaseType_t index,
                     const char *word) {
  BaseType_t len;
  const char *p = ato_cli_core_get_parameter(cmd_string, index, &len);
  return p != NULL && (size_t)len == strlen(word) && strncmp(p, word, len) == 0;
}

static bool dac_channel_from_number(uint32_t ch, uint32_t *channel) {
  if (ch == 1) {
    *channel = DAC_CHANNEL_1;
  } else if (ch == 2) {
    *channel = DAC_CHANNEL_2;
  } else {
    return false;
  }
  return true;
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

static void adc_command(ato_cli_core_t *cli, const char *cmd_string) {
  uint32_t n, ch;
  if (!param_uint(cmd_string, 1, &n) || !param_uint(cmd_string, 2, &ch)) {
    ato_cli_core_printf(cli, "error: invalid parameter\r\n");
    return;
  }
  if (n < 1 || n > ATO_ANALOG_NB_ADC || s_ctx.cfg->adc[n - 1] == NULL) {
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

static void dac_command(ato_cli_core_t *cli, const char *cmd_string) {
  uint32_t n, ch, mv, channel;
  if (!param_uint(cmd_string, 1, &n) || !param_uint(cmd_string, 2, &ch) ||
      !param_uint(cmd_string, 3, &mv)) {
    ato_cli_core_printf(cli, "error: invalid parameter\r\n");
    return;
  }
  if (n < 1 || n > ATO_ANALOG_NB_DAC || s_ctx.cfg->dac[n - 1] == NULL) {
    ato_cli_core_printf(cli, "error: no DAC%lu\r\n", (unsigned long)n);
    return;
  }
  if (!dac_channel_from_number(ch, &channel)) {
    ato_cli_core_printf(cli, "error: invalid channel %lu\r\n",
                        (unsigned long)ch);
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

static void gen_command(ato_cli_core_t *cli, const char *cmd_string) {
  shape_t shape;
  uint32_t freq = 0, vpp;
  bool ok;

  if (param_is(cmd_string, 1, "step")) {
    shape = SHAPE_STEP;
    ok = param_uint(cmd_string, 2, &vpp) &&
         ato_cli_core_get_parameter(cmd_string, 3, &(BaseType_t){0}) == NULL;
  } else {
    shape = param_is(cmd_string, 1, "sine") ? SHAPE_SINE : SHAPE_SQUARE;
    ok = (param_is(cmd_string, 1, "sine") ||
          param_is(cmd_string, 1, "square")) &&
         param_uint(cmd_string, 2, &freq) && freq > 0 &&
         param_uint(cmd_string, 3, &vpp) &&
         ato_cli_core_get_parameter(cmd_string, 4, &(BaseType_t){0}) == NULL;
  }
  if (!ok) {
    ato_cli_core_printf(cli, "error: usage: gen sine|square <Hz> <mVpp> | "
                             "gen step <mVpp>\r\n");
    return;
  }
  if (vpp > VREF_MV) {
    vpp = VREF_MV;
  }

  xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
  s_ctx.shape = shape;
  s_ctx.freq_hz = freq;
  s_ctx.vpp_mv = vpp;
  xSemaphoreGive(s_ctx.lock);

  ato_cli_core_printf(cli, "ok shape=%s f=%lu vpp=%lu\r\n", shape_names[shape],
                      (unsigned long)freq, (unsigned long)vpp);
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

static void acq_command(ato_cli_core_t *cli, const char *cmd_string) {
  const ato_analog_config_t *cfg = s_ctx.cfg;
  uint32_t fs, n;
  if (!param_uint(cmd_string, 1, &fs) || !param_uint(cmd_string, 2, &n) ||
      fs < SCOPE_MIN_FS || fs > SCOPE_MAX_FS || n < SCOPE_MIN_POINTS ||
      n > SCOPE_MAX_POINTS) {
    ato_cli_core_printf(cli, "error: usage: acq <fs %u..%u> <n %u..%u>\r\n",
                        SCOPE_MIN_FS, SCOPE_MAX_FS, SCOPE_MIN_POINTS,
                        SCOPE_MAX_POINTS);
    return;
  }

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

static void dump_command(ato_cli_core_t *cli, const char *cmd_string) {
  (void)cmd_string;
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
