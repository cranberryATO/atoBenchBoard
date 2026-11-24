#ifndef ATO_ANALOG_H
#define ATO_ANALOG_H

/*
 * Analog CLI commands: single ADC reads / DAC writes, and a small
 * "oscilloscope": a DAC channel generates a waveform while an ADC samples
 * the input and the output of the circuit under test, both paced by the
 * same timer.
 *
 * Commands (replies "ok ..." / "error: ..." in plain mode, one JSON object
 * in json mode, see ato_cli_core.h):
 *   adc <n> <ch>               read ADCn channel ch (software-triggered ADC)
 *   dac <n> <ch> <mV>          set DACn channel ch
 *   gen sine <Hz> <mVpp>, gen square <Hz> <mVpp>, gen step <mVpp>
 *                              waveform for the next acquisition, centered
 *                              on VREF/2
 *   acq <fs> <n>               acquire n points at fs Hz
 *   dump                       plain: "#scope ..." header, "in,out" lines
 *                              (3 hex digits each), "#end"; json: the same
 *                              header fields, "in" and "out" arrays
 *
 * Scope requirements (CubeMX):
 * - timer: TRGO = update event; its prescaler / period are set here.
 * - ADC: scan of 2 ranks (rank 1 = circuit input, rank 2 = circuit output),
 *   external trigger = timer TRGO, DMA normal mode, half-word.
 * - DAC channel: trigger source = timer TRGO (enabled here only during an
 *   acquisition), DMA circular mode, half-word.
 *
 * The context is provided by the caller; several instances can coexist
 * (on different ADC / DAC / timer), registered in different CLIs. Commands
 * run in the calling CLI task; a mutex per instance serializes the access to
 * its hardware between several CLIs.
 */

#include "FreeRTOS.h"
#include "ato_adc.h"
#include "ato_cli.h"
#include "main.h"
#include "semphr.h"
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

#define ATO_ANALOG_SCOPE_MAX_POINTS 4096

// Handlers and parameter tables are defined in ato_analog.c.
#define ATO_ANALOG_COMMANDS(X)                                                 \
  X("adc", "Read an ADC channel (raw and mV)", adc_command, adc_params)        \
  X("adc list", "List the ADCs and their channels configured in CubeMX",       \
    adc_list_command)                                                          \
  X("dac", "Set a DAC channel", dac_command, dac_params)                       \
  X("dac list", "List the DAC channels, enabled or not, and their output",     \
    dac_list_command)                                                          \
  X("gen sine", "Sine of the next acquisition, centered on 1650 mV",           \
    gen_sine_command, gen_periodic_params)                                     \
  X("gen square", "Square of the next acquisition, centered on 1650 mV",       \
    gen_square_command, gen_periodic_params)                                   \
  X("gen step", "Step of the next acquisition, centered on 1650 mV",           \
    gen_step_command, gen_step_params)                                         \
  X("acq", "Generate the waveform and acquire n points at fs Hz", acq_command, \
    acq_params)                                                                \
  X("dump", "Dump the last acquisition (hex CSV in,out)", dump_command)

/**
 * Context of an instance; its fields are private to ato_analog.c.
 * About 24 KB with the scope buffers: allocate it static, never on a task
 * stack.
 */
typedef struct ato_analog_ctx {
  const ato_analog_config_t *cfg;
  ato_adc_t adc[ATO_ANALOG_NB_ADC];
  ato_adc_t *scope_adc;
  SemaphoreHandle_t lock; // analog hardware and scope data

  // waveform, set by "gen"
  uint8_t shape; // shape_t
  uint32_t freq_hz;
  uint32_t vpp_mv;

  // last acquisition
  uint32_t n;      // number of points, 0: no data
  uint64_t fs_mhz; // actual sample rate, in mHz
  uint64_t f_mhz;  // actual waveform frequency, in mHz (0 for step)

  uint16_t dac_table[ATO_ANALOG_SCOPE_MAX_POINTS];
  uint16_t samples[2 * ATO_ANALOG_SCOPE_MAX_POINTS]; // in, out interleaved

  ATO_CLI_ITEMS(ATO_ANALOG_COMMANDS);
} ato_analog_ctx_t;

/**
 * @brief  Set up the analog hardware and register the commands in `cli`.
 *         Can be called before the scheduler starts. `ctx` and `cfg` must
 *         stay valid (typically static and static const).
 */
void ato_analog_init(ato_analog_ctx_t *ctx, const ato_analog_config_t *cfg,
                     ato_cli_ctx_t *cli);

#endif /* ATO_ANALOG_H */
