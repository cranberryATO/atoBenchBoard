#ifndef ATO_ANALOG_H
#define ATO_ANALOG_H

/*
 * Analog test task: owns one ADC and one DAC channel, and registers the CLI
 * commands "adc" (read the ADC) and "dac <mV>" (set the DAC output).
 * Test setup: wire the DAC output pin to the ADC input pin.
 *
 * Single instance: the CLI command handlers only get the CLI context, so
 * they reach this module through its static context.
 */

#include "ato_cli.h"
#include "main.h"
#include <stdint.h>

/**
 * @brief  Create the analog task and register its commands in `cli`.
 *         Must be called only once. Failures call fatalError().
 * @param  hadc         ADC, see ato_adc.h for the requirements.
 * @param  hdac         DAC.
 * @param  dac_channel  DAC channel, e.g. DAC_CHANNEL_1.
 * @param  cli          CLI where the commands are registered.
 */
void ato_analog_create_task(ADC_HandleTypeDef *hadc, DAC_HandleTypeDef *hdac,
                            uint32_t dac_channel, ato_cli_ctx_t *cli);

#endif /* ATO_ANALOG_H */
