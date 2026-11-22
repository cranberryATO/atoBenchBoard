
ATO_CLI_COMMAND_BEGIN(gpio_commands)
ATO_CLI_COMMAND_COMMAND("gshow",
                        "\r\ngshow:\r\n Show GPIOs configuration\r\n\r\n",
                        gshow_command, 0)
ATO_CLI_COMMAND_END()
