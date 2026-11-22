#ifndef ATO_ANALOG_H
#define ATO_ANALOG_H

/*
 * Analog CLI commands: single ADC reads / DAC writes, and a small
 * "oscilloscope": a DAC channel generates a waveform while an ADC samples
 * the input and the output of the circuit under test, both paced by the
 * same timer.
 *
 * Commands (every reply line starts with "ok" or "error:"):
 *   adc <n> <ch>               read ADCn channel ch (software-triggered ADC)
 *   dac <n> <ch> <mV>          set DACn channel ch
 *   gen sine|square <Hz> <mVpp>, gen step <mVpp>
 *                              waveform for the next acquisition, centered
 *                              on VREF/2
 *   acq <fs> <n>               acquire n points at fs Hz
 *   dump                       "#scope ..." header, "in,out" lines (3 hex
 *                              digits each), "#end"
 *
 * Scope requirements (CubeMX):
 * - timer: TRGO = update event; its prescaler / period are set here.
 * - ADC: scan of 2 ranks (rank 1 = circuit input, rank 2 = circuit output),
 *   external trigger = timer TRGO, DMA normal mode, half-word.
 * - DAC channel: trigger source = timer TRGO (enabled here only during an
 *   acquisition), DMA circular mode, half-word.
 *
 * Single instance. Commands run in the calling CLI task; a mutex serializes
 * the access to the analog hardware between several CLIs.
 */

#include "ato_cli.h"
#include "main.h"
#include <stdint.h>

#define ATO_ANALOG_NB_ADC 5
#define ATO_ANALOG_NB_DAC 4

typedef struct {
  ADC_HandleTypeDef *adc[ATO_ANALOG_NB_ADC]; // index = ADC number - 1, or NULL
  DAC_HandleTypeDef *dac[ATO_ANALOG_NB_DAC]; // index = DAC number - 1, or NULL

  ADC_HandleTypeDef *scope_adc; // must also be listed in adc[]
  DAC_HandleTypeDef *scope_dac;
  uint32_t scope_dac_channel; // DAC_CHANNEL_1 or DAC_CHANNEL_2
  TIM_HandleTypeDef *scope_tim;
  const char *scope_in_label;  // shown in the dump header, e.g. "adc1.1"
  const char *scope_out_label; // e.g. "adc1.5"
} ato_analog_config_t;

/**
 * @brief  Set up the analog hardware and register the commands in `cli`.
 *         Must be called only once, can be called before the scheduler
 *         starts. `cfg` must stay valid (typically static const).
 */
void ato_analog_init(const ato_analog_config_t *cfg, ato_cli_ctx_t *cli);

#endif /* ATO_ANALOG_H */
