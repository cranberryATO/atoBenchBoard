/*
 * Reentrant command line interpreter.
 *
 * Forked from FreeRTOS+CLI V202212.00 (FreeRTOS_CLI.c).
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Modified: all mutable state (command list, list tail, current command,
 * "help" cursor, output buffer) moved into a per-instance context
 * (ato_cli_core_t), so that several interpreters can run concurrently.
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

#include "task.h"
#include <string.h>

static BaseType_t help_command(ato_cli_core_t *cli, char *out, size_t out_len,
                               const char *cmd_string);
static int8_t get_number_of_parameters(const char *cmd_string);

/* The "help" command is always at the head of every command list. */
static const ato_cli_command_t help_command_def = {
    "help", "\r\nhelp:\r\n Lists all the registered commands\r\n\r\n",
    help_command, 0};

/*-----------------------------------------------------------*/

void ato_cli_core_init(ato_cli_core_t *cli, void *user) {
  configASSERT(cli != NULL);

  cli->commands.def = &help_command_def;
  cli->commands.next = NULL;
  cli->last = &cli->commands;
  cli->current = NULL;
  cli->help_next = NULL;
  cli->user = user;
  cli->output[0] = '\0';
}
/*-----------------------------------------------------------*/

void ato_cli_core_register_static(ato_cli_core_t *cli,
                                  const ato_cli_command_t *def,
                                  ato_cli_list_item_t *item) {
  configASSERT(cli != NULL);
  configASSERT(def != NULL);
  configASSERT(item != NULL);

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

BaseType_t ato_cli_core_register(ato_cli_core_t *cli,
                                 const ato_cli_command_t *def) {
  configASSERT(def != NULL);

  ato_cli_list_item_t *item = pvPortMalloc(sizeof(ato_cli_list_item_t));
  configASSERT(item != NULL);

  if (item == NULL) {
    return pdFAIL;
  }
  ato_cli_core_register_static(cli, def, item);
  return pdPASS;
}
/*-----------------------------------------------------------*/

BaseType_t ato_cli_core_process(ato_cli_core_t *cli, const char *input,
                                char *out, size_t out_len) {
  BaseType_t ret = pdTRUE;

  configASSERT(cli != NULL);

  if (cli->current == NULL) {
    /* Search for the command string in the list of registered commands. */
    for (cli->current = &cli->commands; cli->current != NULL;
         cli->current = cli->current->next) {
      const char *registered = cli->current->def->command;
      size_t len = strlen(registered);

      /* To ensure the string lengths match exactly, so as not to pick up
       * a sub-string of a longer command, check the byte after the expected
       * end of the string is either the end of the string or a space before
       * a parameter. */
      if (strncmp(input, registered, len) == 0) {
        if ((input[len] == ' ') || (input[len] == 0x00)) {
          /* The command has been found. Check it has the expected number of
           * parameters. If expected_params is -1, then there could be a
           * variable number of parameters and no check is made. */
          if (cli->current->def->expected_params >= 0) {
            if (get_number_of_parameters(input) !=
                cli->current->def->expected_params) {
              ret = pdFALSE;
            }
          }
          break;
        }
      }
    }
  }

  if ((cli->current != NULL) && (ret == pdFALSE)) {
    /* The command was found, but the number of parameters with the command
     * was incorrect. */
    strncpy(out,
            "Incorrect command parameter(s).  Enter \"help\" to view a list "
            "of available commands.\r\n\r\n",
            out_len);
    out[out_len - 1] = '\0';
    cli->current = NULL;
  } else if (cli->current != NULL) {
    /* Call the callback function that is registered to this command. */
    ret = cli->current->def->handler(cli, out, out_len, input);

    /* If ret is pdFALSE, then no further strings will be returned after this
     * one, and current can be reset to NULL ready to search for the next
     * entered command. */
    if (ret == pdFALSE) {
      cli->current = NULL;
    }
  } else {
    /* current was NULL, the command was not found. */
    strncpy(out,
            "Command not recognised.  Enter 'help' to view a list of "
            "available commands.\r\n\r\n",
            out_len);
    out[out_len - 1] = '\0';
    ret = pdFALSE;
  }

  return ret;
}
/*-----------------------------------------------------------*/

char *ato_cli_core_output_buffer(ato_cli_core_t *cli) { return cli->output; }
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

static BaseType_t help_command(ato_cli_core_t *cli, char *out, size_t out_len,
                               const char *cmd_string) {
  BaseType_t ret;

  (void)cmd_string;

  if (cli->help_next == NULL) {
    /* Reset the cursor back to the start of the list. */
    cli->help_next = &cli->commands;
  }

  /* Return the next command help string, before moving the cursor on to the
   * next command in the list. */
  strncpy(out, cli->help_next->def->help, out_len);
  out[out_len - 1] = '\0';
  cli->help_next = cli->help_next->next;

  if (cli->help_next == NULL) {
    /* There are no more commands in the list, so there will be no more
     * strings to return after this one and pdFALSE should be returned. */
    ret = pdFALSE;
  } else {
    ret = pdTRUE;
  }

  return ret;
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
