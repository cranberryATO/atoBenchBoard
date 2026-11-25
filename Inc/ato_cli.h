#ifndef ATO_CLI_H
#define ATO_CLI_H

#include "ato_cli_core.h" // ato_cli_command_t
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

/**
 * @brief  Register an extra command in a CLI, running or not yet started.
 *         No allocation: the caller provides the list item. Callable from
 *         any task (not from an ISR).
 * @param  ctx   CLI returned by ato_cli_create_task().
 * @param  def   Command descriptor (typically a static const).
 * @param  item  List item storage (typically static), one per command and
 *               per CLI. Both must stay valid for the lifetime of the CLI.
 * @param  user_data  Given to the handler of the command (typically the
 *                    context of the module).
 */
void ato_cli_register_command(ato_cli_ctx_t *ctx, const ato_cli_command_t *def,
                              ato_cli_list_item_t *item, void *user_data);

void ato_cli_register_command_table(ato_cli_ctx_t *ctx, int nb_commands,
                                    const ato_cli_command_t *const commands,
                                    ato_cli_list_item_t *item,
                                    void *user_data);


#endif /* ATO_CLI_H */
