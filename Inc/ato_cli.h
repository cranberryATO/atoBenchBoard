#ifndef ATO_CLI_H
#define ATO_CLI_H

#include "FreeRTOS.h"
#include "ato_cli_core.h" // ato_cli_command_t
#include "task.h"

#define ATO_CLI_CMD_SIZE 100    // max command line length, null included
#define ATO_CLI_RX_BUF_SIZE 64
typedef struct ato_cli_ctx {
  ato_cli_core_t cli; // command interpreter
  // line editor state
  char cmd_buf[ATO_CLI_CMD_SIZE];
  uint8_t rx_buf[ATO_CLI_RX_BUF_SIZE];
  size_t cmd_idx;
  uint8_t discard;      // line too long: ignore until end of line
  uint8_t last_char_cr; // swallow the \n of a \r\n pair
} ato_cli_ctx_t;

void ato_cli_create_task(ato_cli_ctx_t *ctx, const char *task_name,
                                   ato_cli_read_fn read_fn,
                                   ato_cli_write_fn write_fn, void *io_arg,
                                   TaskHandle_t *const pxCreatedTask);

void ato_cli_register_command(ato_cli_ctx_t *ctx, const ato_cli_command_t *def,
                              ato_cli_list_item_t *item, void *user_data);

void ato_cli_register_command_table(ato_cli_ctx_t *ctx, 
                                    const ato_cli_command_t *const commands, size_t nb_commands,
                                    ato_cli_list_item_t *item,
                                    void *user_data);


#endif /* ATO_CLI_H */
