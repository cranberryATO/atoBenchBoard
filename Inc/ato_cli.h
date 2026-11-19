#ifndef ATO_CLI_H
#define ATO_CLI_H

#include <stdint.h>

/**
 * @brief  CLI task entry point (FreeRTOS+CLI over USART1, DMA RX/TX).
 *         Create it with xTaskCreate(); a stack of 512 words is needed
 *         by vTaskList()/snprintf().
 */
void ato_cli_task(void *argument);

/**
 * @brief  Queue bytes for transmission on USART1 (non-blocking).
 *         Bytes that do not fit in the TX ring buffer are dropped and
 *         counted (see the "stats" command).
 */
void tx_bytes(const uint8_t *data, uint16_t len);

/**
 * @brief  Queue a null-terminated string for transmission on USART1.
 */
void tx_string(char *str);

#endif /* ATO_CLI_H */
