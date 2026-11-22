/*
 * Reentrant command line interpreter.
 *
 * Forked from FreeRTOS+CLI V202212.00 (FreeRTOS_CLI.c).
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Modified: all mutable state (command list, list tail, output sink) moved
 * into a per-instance context (ato_cli_core_t), so that several interpreters
 * can run concurrently; command handlers write their output through a
 * callback instead of filling a buffer over several calls.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 */

#include "ato_cli_core.h"

#include "task.h" // taskENTER_CRITICAL(); configASSERT() uses taskDISABLE_INTERRUPTS()
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void help_command(ato_cli_core_t *cli, const char *cmd_string);
static int8_t get_number_of_parameters(const char *cmd_string);

/* The "help" command is always at the head of every command list. */
static const ato_cli_command_t help_command_def = {
    "help", "\r\nhelp:\r\n Lists all the registered commands\r\n\r\n",
    help_command, 0};

/*-----------------------------------------------------------*/

void ato_cli_core_init(ato_cli_core_t *cli, void *user, ato_cli_write_fn write,
                       void *write_arg) {
  configASSERT(cli != NULL);
  configASSERT(write != NULL);

  cli->commands.def = &help_command_def;
  cli->commands.next = NULL;
  cli->last = &cli->commands;
  cli->write = write;
  cli->write_arg = write_arg;
  cli->user = user;
}
/*-----------------------------------------------------------*/

void ato_cli_core_register(ato_cli_core_t *cli, const ato_cli_command_t *def,
                           ato_cli_list_item_t *item) {
  configASSERT(cli != NULL);
  configASSERT(def != NULL);
  configASSERT(item != NULL);

  /* Commands may be registered from any task, while the owner task walks the
   * list (process / help): the append must be atomic for that reader, and
   * protects `last` against concurrent registrations. */
  taskENTER_CRITICAL();
  {
    /* Reference the command being registered from the list item. */
    item->def = def;

    /* The new list item is appended to the end of the list. */
    item->next = NULL;
    cli->last->next = item;
    cli->last = item;
  }
  taskEXIT_CRITICAL();
}
/*-----------------------------------------------------------*/

void ato_cli_core_process(ato_cli_core_t *cli, const char *input) {
  configASSERT(cli != NULL);

  /* Search for the command string in the list of registered commands. */
  for (const ato_cli_list_item_t *item = &cli->commands; item != NULL;
       item = item->next) {
    const ato_cli_command_t *def = item->def;
    size_t len = strlen(def->command);

    /* To ensure the string lengths match exactly, so as not to pick up a
     * sub-string of a longer command, check the byte after the expected end
     * of the string is either the end of the string or a space before a
     * parameter. */
    if (strncmp(input, def->command, len) != 0 ||
        (input[len] != ' ' && input[len] != 0x00)) {
      continue;
    }

    /* The command has been found. Check it has the expected number of
     * parameters. If expected_params is -1, then there could be a variable
     * number of parameters and no check is made. */
    if (def->expected_params >= 0 &&
        get_number_of_parameters(input) != def->expected_params) {
      ato_cli_core_puts(cli, "Incorrect command parameter(s).  Enter \"help\" "
                             "to view a list of available commands.\r\n\r\n");
    } else {
      def->handler(cli, input);
    }
    return;
  }

  ato_cli_core_puts(cli, "Command not recognised.  Enter 'help' to view a "
                         "list of available commands.\r\n\r\n");
}
/*-----------------------------------------------------------*/

void ato_cli_core_write(ato_cli_core_t *cli, const char *data, size_t len) {
  if (len > 0) {
    cli->write(cli->write_arg, data, len);
  }
}
/*-----------------------------------------------------------*/

void ato_cli_core_puts(ato_cli_core_t *cli, const char *str) {
  ato_cli_core_write(cli, str, strlen(str));
}
/*-----------------------------------------------------------*/

void ato_cli_core_printf(ato_cli_core_t *cli, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(cli->printf_buf, sizeof(cli->printf_buf), fmt, args);
  va_end(args);

  if (n < 0) {
    return; // encoding error: nothing to print
  }
  // vsnprintf returns the untruncated length
  size_t len = (size_t)n < sizeof(cli->printf_buf)
                   ? (size_t)n
                   : sizeof(cli->printf_buf) - 1;
  ato_cli_core_write(cli, cli->printf_buf, len);
}
/*-----------------------------------------------------------*/

void *ato_cli_core_user(const ato_cli_core_t *cli) { return cli->user; }
/*-----------------------------------------------------------*/

const char *ato_cli_core_get_parameter(const char *cmd_string,
                                       UBaseType_t wanted,
                                       BaseType_t *param_len) {
  UBaseType_t found = 0;
  const char *ret = NULL;

  *param_len = 0;

  while (found < wanted) {
    /* Index the character pointer past the current word. If this is the start
     * of the command string then the first word is the command itself. */
    while (((*cmd_string) != 0x00) && ((*cmd_string) != ' ')) {
      cmd_string++;
    }

    /* Find the start of the next string. */
    while (((*cmd_string) != 0x00) && ((*cmd_string) == ' ')) {
      cmd_string++;
    }

    /* Was a string found? */
    if (*cmd_string != 0x00) {
      /* Is this the start of the required parameter? */
      found++;

      if (found == wanted) {
        /* How long is the parameter? */
        ret = cmd_string;

        while (((*cmd_string) != 0x00) && ((*cmd_string) != ' ')) {
          (*param_len)++;
          cmd_string++;
        }

        if (*param_len == 0) {
          ret = NULL;
        }
        break;
      }
    } else {
      break;
    }
  }

  return ret;
}
/*-----------------------------------------------------------*/

static void help_command(ato_cli_core_t *cli, const char *cmd_string) {
  (void)cmd_string;

  /* Commands may be appended meanwhile: each `next` is read once, and is
   * either NULL or a fully linked item (see ato_cli_core_register()). */
  for (const ato_cli_list_item_t *item = &cli->commands; item != NULL;
       item = item->next) {
    ato_cli_core_puts(cli, item->def->help);
  }
}
/*-----------------------------------------------------------*/

static int8_t get_number_of_parameters(const char *cmd_string) {
  int8_t params = 0;
  BaseType_t last_was_space = pdFALSE;

  /* Count the number of space delimited words in cmd_string. */
  while (*cmd_string != 0x00) {
    if ((*cmd_string) == ' ') {
      if (last_was_space != pdTRUE) {
        params++;
        last_was_space = pdTRUE;
      }
    } else {
      last_was_space = pdFALSE;
    }
    cmd_string++;
  }

  /* If the command string ended with spaces, then there will have been too
   * many parameters counted. */
  if (last_was_space == pdTRUE) {
    params--;
  }

  /* The value returned is one less than the number of space delimited words,
   * as the first word should be the command itself. */
  return params;
}
/*-----------------------------------------------------------*/
