#ifndef ATO_ADC_H
#define ATO_ADC_H

/*
 * Generic ADC layer over a HAL ADC: single reads of any channel, and DMA
 * acquisitions of the regular sequence configured in CubeMX.
 *
 * End of conversion / acquisition is signaled from the ISR with a binary
 * semaphore (not a task notification, so that the caller may be a task that
 * already uses its notification, such as a CLI task).
 *
 * The ADC is calibrated (single-ended) on first use, once the scheduler runs.
 *
 * Requirements on the ADC (CubeMX): ADC IRQ and DMA IRQ priorities >= 5
 * (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY).
 * - ato_adc_read(): software trigger, one conversion, no continuous mode.
 * - ato_adc_start_dma(): DMA in normal mode (half-word), any trigger.
 *
 * Threading: an instance must only be used by one task at a time (callers
 * provide their own locking).
 */

#include "FreeRTOS.h"
#include "main.h"
#include "semphr.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  ADC_HandleTypeDef *hadc;
  SemaphoreHandle_t done; // given by the ISR at end of conversion / DMA
  bool calibrated;
  volatile uint32_t errors; // written in ISR
} ato_adc_t;

/**
 * @brief  Reset the instance and attach it to `hadc`, so that the HAL ADC
 *         callbacks are routed to it. An ADC can only be attached once.
 *         Can be called before the scheduler starts.
 */
void ato_adc_init(ato_adc_t *a, ADC_HandleTypeDef *hadc);

/** True if the ADC is started by software (usable by ato_adc_read()). */
bool ato_adc_is_software_triggered(const ato_adc_t *a);

/**
 * @brief  Convert one channel (single-ended), blocking the calling task.
 * @param  channel  Channel number (1..18), as in ADCx_INy.
 * @retval false if the ADC is not software triggered or the channel is
 *         rejected by the HAL; hardware failures call fatalError().
 */
bool ato_adc_read(ato_adc_t *a, uint32_t channel, uint16_t *value);

/**
 * @brief  Start a DMA acquisition of `len` conversions of the regular
 *         sequence into `buf`. With an external trigger, the ADC then waits
 *         for its trigger. Failure calls fatalError().
 */
void ato_adc_start_dma(ato_adc_t *a, uint16_t *buf, uint32_t len);

/**
 * @brief  Wait for the end of the acquisition started by ato_adc_start_dma(),
 *         then stop the ADC. Timeout calls fatalError().
 */
void ato_adc_wait_dma(ato_adc_t *a, TickType_t timeout);

#endif /* ATO_ADC_H */
