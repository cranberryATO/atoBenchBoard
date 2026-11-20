#include "ato_adc.h"

#include "ato_error.h"
#include <string.h>

#define ATO_ADC_TIMEOUT_MS 10 // a conversion takes a few microseconds

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

  taskENTER_CRITICAL();
  configASSERT(s_by_periph[i] == NULL); // ADC already attached
  s_by_periph[i] = a;
  taskEXIT_CRITICAL();

  if (HAL_ADCEx_Calibration_Start(hadc, ADC_SINGLE_ENDED) != HAL_OK) {
    fatalError("ADC calibration failed");
  }
}

/* Conversion --------------------------------------------------------------- */

uint16_t ato_adc_read(ato_adc_t *a) {
  configASSERT(a != NULL);

  // must be set before the conversion starts: the EOC ISR notifies this task
  a->notify_task = xTaskGetCurrentTaskHandle();
  if (HAL_ADC_Start_IT(a->hadc) != HAL_OK) {
    fatalError("ADC start failed");
  }
  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ATO_ADC_TIMEOUT_MS)) == 0) {
    fatalError("ADC conversion timeout");
  }
  return (uint16_t)HAL_ADC_GetValue(a->hadc);
}

/* HAL callbacks (ISR context) ---------------------------------------------- */

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
  ato_adc_t *a = find_instance(hadc);
  if (a == NULL || a->notify_task == NULL) {
    return;
  }
  a->conversions++;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(a->notify_task, &woken);
  portYIELD_FROM_ISR(woken);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc) {
  ato_adc_t *a = find_instance(hadc);
  if (a != NULL) {
    a->errors++;
  }
}
