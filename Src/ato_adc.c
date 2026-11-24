#include "ato_adc.h"

#include "ato_error.h"
#include "task.h"
#include <string.h>

#define ATO_ADC_TIMEOUT_MS 10 // a single conversion takes a few microseconds

// Instances attached to an ADC, indexed by ADC peripheral (not by HAL handle
// name, so this module does not depend on CubeMX naming). The contexts are
// owned by the caller; this table only references them for the HAL callbacks.
#define ATO_ADC_NB_PERIPH 5
static ato_adc_t *s_by_periph[ATO_ADC_NB_PERIPH];

static int32_t periph_index(const ADC_TypeDef *inst) {
  if (inst == ADC1) return 0;
  if (inst == ADC2) return 1;
  if (inst == ADC3) return 2;
  if (inst == ADC4) return 3;
  if (inst == ADC5) return 4;
  return -1;
}

static ato_adc_t *find_instance(const ADC_HandleTypeDef *hadc) {
  int32_t i = periph_index(hadc->Instance);
  if (i < 0) {
    return NULL;
  }
  ato_adc_t *a = s_by_periph[i];
  return (a != NULL && a->hadc == hadc) ? a : NULL;
}

/* Setup -------------------------------------------------------------------- */

void ato_adc_init(ato_adc_t *a, ADC_HandleTypeDef *hadc) {
  configASSERT(a != NULL);
  configASSERT(hadc != NULL);

  int32_t i = periph_index(hadc->Instance);
  configASSERT(i >= 0); // unknown ADC peripheral

  memset(a, 0, sizeof(*a));
  a->hadc = hadc;
  a->done = xSemaphoreCreateBinary();
  if (a->done == NULL) {
    fatalError("ADC: semaphore allocation failed");
  }

  taskENTER_CRITICAL();
  configASSERT(s_by_periph[i] == NULL); // ADC already attached
  s_by_periph[i] = a;
  taskEXIT_CRITICAL();
}

bool ato_adc_is_software_triggered(const ato_adc_t *a) {
  return a->hadc->Init.ExternalTrigConv == ADC_SOFTWARE_START;
}

// Calibration needs the HAL tick, so it is done on first use (scheduler
// running) rather than in ato_adc_init().
static void calibrate_once(ato_adc_t *a) {
  if (a->calibrated) {
    return;
  }
  if (HAL_ADCEx_Calibration_Start(a->hadc, ADC_SINGLE_ENDED) != HAL_OK) {
    fatalError("ADC calibration failed");
  }
  a->calibrated = true;
}

/* Single conversion -------------------------------------------------------- */

bool ato_adc_read(ato_adc_t *a, uint32_t channel, uint16_t *value) {
  configASSERT(a != NULL);
  if (!ato_adc_is_software_triggered(a) || channel < 1 ||
      channel > ATO_ADC_MAX_CHANNEL) {
    return false;
  }
  calibrate_once(a);

  ADC_ChannelConfTypeDef cfg = {
      .Channel = __LL_ADC_DECIMAL_NB_TO_CHANNEL(channel),
      .Rank = ADC_REGULAR_RANK_1,
      .SamplingTime = ADC_SAMPLETIME_247CYCLES_5, // unknown source impedance
      .SingleDiff = ADC_SINGLE_ENDED,
      .OffsetNumber = ADC_OFFSET_NONE,
      .Offset = 0,
  };
  if (HAL_ADC_ConfigChannel(a->hadc, &cfg) != HAL_OK) {
    return false;
  }

  xSemaphoreTake(a->done, 0); // drop a stale signal, if any
  if (HAL_ADC_Start_IT(a->hadc) != HAL_OK) {
    fatalError("ADC start failed");
  }
  if (xSemaphoreTake(a->done, pdMS_TO_TICKS(ATO_ADC_TIMEOUT_MS)) != pdTRUE) {
    fatalError("ADC conversion timeout");
  }
  *value = (uint16_t)HAL_ADC_GetValue(a->hadc);
  return true;
}

/* DMA acquisition ---------------------------------------------------------- */

void ato_adc_start_dma(ato_adc_t *a, uint16_t *buf, uint32_t len) {
  configASSERT(a != NULL);
  configASSERT(buf != NULL);
  calibrate_once(a);

  xSemaphoreTake(a->done, 0); // drop a stale signal, if any
  // the HAL takes a uint32_t pointer, but the transfer width is the DMA
  // configuration (half-word)
  if (HAL_ADC_Start_DMA(a->hadc, (uint32_t *)buf, len) != HAL_OK) {
    fatalError("ADC DMA start failed");
  }
}

void ato_adc_wait_dma(ato_adc_t *a, TickType_t timeout) {
  configASSERT(a != NULL);
  if (xSemaphoreTake(a->done, timeout) != pdTRUE) {
    fatalError("ADC acquisition timeout");
  }
  if (HAL_ADC_Stop_DMA(a->hadc) != HAL_OK) {
    fatalError("ADC DMA stop failed");
  }
}

/* HAL callbacks (ISR context) ---------------------------------------------- */

// End of a single conversion (IT) or of a DMA acquisition (DMA normal mode).
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
  ato_adc_t *a = find_instance(hadc);
  if (a == NULL) {
    return;
  }
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(a->done, &woken);
  portYIELD_FROM_ISR(woken);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc) {
  ato_adc_t *a = find_instance(hadc);
  if (a != NULL) {
    a->errors++;
  }
}
