#ifndef ATO_CLI_TEST_H
#define ATO_CLI_TEST_H

#include "ato_cli.h"

// defining the commands with an X-macro will allow us to describe
// them here in one place, and then use this information
// both in the context below (to get the command count for the 
// command items table) and in the .c file for the command
// description table
#define ATO_CLI_TEST_COMMANDS(X) \
    X("cli test params", "test various params configurations", cli_test_1_command, cli_test_1_params) \
    X("cli test printf", "test printf", cli_test_printf_command, cli_test_1_params) \
    X("cli test mutex", "test protection of handlers on multiple CLIs", cli_test_mutex_command) \
    X("cli test info", "show cli info", cli_test_info_command) 

typedef struct {
    ATO_CLI_ITEMS(ATO_CLI_TEST_COMMANDS, cmd_items);
} ato_cli_test_ctx_t;

void ato_cli_test_init(ato_cli_test_ctx_t *cli_test_ctx,
                       ato_cli_t *cli_ctx);

#endif // ATO_CLI_TEST_H
