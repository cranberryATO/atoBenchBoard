#ifndef ATO_CLI_H
#define ATO_CLI_H

#include "main.h"

/** CLI instance context (opaque). */
typedef struct ato_cli_ctx ato_cli_ctx_t;

/**
 * @brief  Create a CLI task on `huart` (FreeRTOS command interpreter with a
 *         line editor). The context is allocated on the FreeRTOS heap and
 *         passed to the task; each instance has its own command list.
 * @param  huart      UART with RX (circular) and TX DMA, see ato_uart.h.
 *                    A UART can only be used by one CLI.
 * @param  task_name  FreeRTOS task name (shown by the "ps" command).
 * @retval The new context. Never NULL, task creation failure will call fatalError().
 */
ato_cli_ctx_t *ato_cli_create_task(UART_HandleTypeDef *huart,
                                   const char *task_name);

#endif /* ATO_CLI_H */
