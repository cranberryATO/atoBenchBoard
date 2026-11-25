#include "ato_cli.h"
#include "ato_cli_core.h"

#include "ato_cli_test.h"

ATO_CLI_COMMAND_FUNCTION(cli_test_1_command);
ATO_CLI_COMMAND_FUNCTION(cli_test_printf_command);
ATO_CLI_COMMAND_FUNCTION(cli_test_info_command);
ATO_CLI_COMMAND_FUNCTION(cli_test_mutex_command);

static const char *const abc[] = {"a", "b", "c"};

static const ato_cli_param_t cli_test_1_params[] = {
    ATO_CLI_INT("n", "some int", -10, 10),
    ATO_CLI_ENUM("e", "an enum", abc),
    ATO_CLI_STRING("s", "a string"),
    ATO_CLI_INT_OPTIONAL("no", "optional int", 0, 10000000),
    ATO_CLI_ENUM_OPTIONAL("eo", "optional enum", abc),
    ATO_CLI_STRING_OPTIONAL("so", "optional string"),
};

ATO_CLI_TABLE(cli_commands, ATO_CLI_TEST_COMMANDS);

void ato_cli_test_init(ato_cli_test_ctx_t *cli_test_ctx,
                       ato_cli_ctx_t *cli_ctx) {
  ATO_CLI_REGISTER_TABLE(cli_ctx, cli_commands, cli_test_ctx->cmd_items,
                         cli_test_ctx);
}

// This test function simply prints back the arguments
// it demonstrate how to test for optional arguments
// (test argc), how to use the automatic conversion
// (user still has to select the correct union that
// matches the parameter type)
// ato_cli_reply_* commands are useful for returning
// JSON formatted replies. If you only want a printf()
// formatted result look at cli_test_printf_command()
// below.
ATO_CLI_COMMAND_FUNCTION(cli_test_1_command) {
  ato_cli_reply_begin(cli);
  ato_cli_reply_int(cli, "n", args[0].i);
  ato_cli_reply_uint(cli, "e", args[1].e);
  ato_cli_reply_str(cli, "s", args[2].s);
  if (argc >= 4)
    ato_cli_reply_int(cli, "no", args[3].i);
  if (argc >= 5)
    ato_cli_reply_uint(cli, "eo", args[4].e);
  if (argc == 6)
    ato_cli_reply_str(cli, "so", args[5].s);
  ato_cli_reply_end(cli);
}

ATO_CLI_COMMAND_FUNCTION(cli_test_printf_command) {
  ato_cli_core_printf(cli, "n=%ld, e=%d, s=%s", args[0].i, args[1].e,
                      args[2].s);
  if (argc >= 4)
    ato_cli_core_printf(cli, ", no=%ld", args[3].i);
  if (argc >= 5)
    ato_cli_core_printf(cli, ", eo=%d", args[4].e);
  if (argc >= 6)
    ato_cli_core_printf(cli, ", so=%s", args[5].s);
  ato_cli_core_printf(cli, "\r\n");
}

ATO_CLI_COMMAND_FUNCTION(cli_test_info_command) {}
ATO_CLI_COMMAND_FUNCTION(cli_test_mutex_command) {}
