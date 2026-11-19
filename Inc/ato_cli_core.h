/*
 * Reentrant command line interpreter.
 *
 * Forked from FreeRTOS+CLI V202212.00 (FreeRTOS_CLI.h).
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Modified: all mutable state moved into a per-instance context
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

#ifndef ATO_CLI_CORE_H
#define ATO_CLI_CORE_H

#include "FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>

#ifndef configCOMMAND_INT_MAX_OUTPUT_SIZE
#define configCOMMAND_INT_MAX_OUTPUT_SIZE 512
#endif

typedef struct ato_cli_core ato_cli_core_t;

/**
 * Command handler. Writes its output into `out` (at most `out_len` bytes,
 * null-terminated). Returns pdTRUE if it has more output to produce (it will
 * be called again with the same command string), pdFALSE when done.
 * `cli` is the interpreter running the command; use ato_cli_core_user() to
 * reach the owner context.
 */
typedef BaseType_t (*ato_cli_command_fn)(ato_cli_core_t *cli, char *out,
                                         size_t out_len,
                                         const char *cmd_string);

/** Command descriptor, meant to be a const global. */
typedef struct {
  const char *const command; // must be all lower case, e.g. "help"
  const char *const help;    // should end with "\r\n"
  const ato_cli_command_fn handler;
  int8_t expected_params; // fixed number of parameters, or -1 for variable
} ato_cli_command_t;

/** Element of the per-interpreter list of registered commands. */
typedef struct ato_cli_list_item {
  const ato_cli_command_t *def;
  struct ato_cli_list_item *next;
} ato_cli_list_item_t;

/** Interpreter context. Public so it can be embedded in an owner struct. */
struct ato_cli_core {
  ato_cli_list_item_t commands;         // list head, always the "help" command
  ato_cli_list_item_t *last;            // list tail, for appending
  const ato_cli_list_item_t *current;   // command producing multi-call output
  const ato_cli_list_item_t *help_next; // cursor of the "help" command
  void *user;                           // owner context, for command handlers
  char output[configCOMMAND_INT_MAX_OUTPUT_SIZE]; // optional output buffer
};

/** Initialise an interpreter: empty command list (except "help"). */
void ato_cli_core_init(ato_cli_core_t *cli, void *user);

/*
 * Registration follows the same rule as ato_cli_core_process(): it must be
 * done by the task that owns the context, or before that task starts.
 */

/** Register a command; the list item is allocated with pvPortMalloc(). */
BaseType_t ato_cli_core_register(ato_cli_core_t *cli,
                                 const ato_cli_command_t *def);

/** Register a command using a caller-provided list item. */
void ato_cli_core_register_static(ato_cli_core_t *cli,
                                  const ato_cli_command_t *def,
                                  ato_cli_list_item_t *item);

/**
 * Run the command line `input`, writing output into `out` (`out_len` bytes).
 * Must be called repeatedly with the same input until it returns pdFALSE.
 * Reentrant: distinct `cli` contexts can be used concurrently; a given
 * context must only be used by one task at a time.
 */
BaseType_t ato_cli_core_process(ato_cli_core_t *cli, const char *input,
                                char *out, size_t out_len);

/** Output buffer of this interpreter (configCOMMAND_INT_MAX_OUTPUT_SIZE). */
char *ato_cli_core_output_buffer(ato_cli_core_t *cli);

/** Owner context given to ato_cli_core_init(). */
void *ato_cli_core_user(const ato_cli_core_t *cli);

/**
 * Return a pointer to the `wanted`'th word (1 = first parameter) of
 * `cmd_string`, and its length in `*param_len`; NULL if absent.
 * Pure function: it needs no interpreter context.
 */
const char *ato_cli_core_get_parameter(const char *cmd_string,
                                       UBaseType_t wanted,
                                       BaseType_t *param_len);

#endif /* ATO_CLI_CORE_H */
