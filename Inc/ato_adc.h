#ifndef ATO_ADC_H
#define ATO_ADC_H

/*
 * Generic single-conversion ADC reader over a HAL ADC.
 *
 * A conversion is started in interrupt mode; the calling task sleeps until
 * the end-of-conversion ISR notifies it (xTaskNotifyGive).
 *
 * Requirements on the ADC (CubeMX): regular group, software trigger, one
 * conversion per start (no continuous mode), EOC selection = single
 * conversion, ADC IRQ enabled with priority >= 5
 * (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY).
 *
 * Threading: one task owns an instance; ato_adc_read() must only be called
 * from that task. The task notification of the caller is used.
 */

#include "FreeRTOS.h"
#include "main.h"
#include "task.h"
#include <stdint.h>

typedef struct {
  ADC_HandleTypeDef *hadc;
  TaskHandle_t notify_task; // task waiting for the end of conversion
  volatile uint32_t conversions;
  volatile uint32_t errors; // written in ISR
} ato_adc_t;

/**
 * @brief  Reset the instance, attach it to `hadc` (so that the HAL ADC
 *         callbacks are routed to it) and calibrate the ADC (single-ended).
 *         An ADC can only be attached once. Calibration failure calls
 *         fatalError().
 */
void ato_adc_init(ato_adc_t *a, ADC_HandleTypeDef *hadc);

/**
 * @brief  Run one conversion of the regular group and return the raw value.
 *         Blocks the calling task until the end of conversion. Start failure
 *         or timeout calls fatalError().
 */
uint16_t ato_adc_read(ato_adc_t *a);

#endif /* ATO_ADC_H */
