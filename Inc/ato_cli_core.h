/*
 * Reentrant command line interpreter.
 *
 * Forked from FreeRTOS+CLI V202212.00 (FreeRTOS_CLI.h).
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Modified: all mutable state moved into a per-instance context
 * (ato_cli_core_t), so that several interpreters can run concurrently;
 * command handlers write their output through a callback instead of filling
 * a buffer over several calls; typed parameters, parsed and checked before
 * the handler is called; multi-word commands grouped in menus; generated
 * help.
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

/*
 * Syntax and menus
 * ----------------
 * A line is a list of words separated by spaces. A command name is made of
 * one or more words ("ps", "gen sine"); its parameters follow it.
 *
 * Menus are implicit: the words shared by several command names form a menu
 * ("gen" for "gen sine" and "gen step"). Entering only a menu name enters the
 * menu, which is shown in the prompt ("gen> "). In the menu M, the input
 * "X ..." is looked up as "M X ..." first, then as "X ..." from the root: the
 * root commands and the full command names work from any menu.
 *   ".."  goes up one level, "/" goes back to the root.
 *
 * Output modes
 * ------------
 * "mode plain|json" selects the output of this interpreter:
 * - plain (default): echo and prompt; replies "ok key=value ..." or
 *   "error: message", tables as aligned text.
 * - json, for scripts: no echo, no prompt; each command line gets exactly
 *   one JSON object, on one line. Its last member is "ok": when it is not
 *   true, the rest of the object is invalid, and "error" holds the message:
 *     {"raw":1234,"mv":995,"ok":true}
 *     {"error":"<fs>: out of range 10..200000","ok":false}
 *
 * Handlers write flat replies with the ato_cli_reply_xxx() functions, which
 * produce both formats; for nested data (tables), they test
 * ato_cli_core_mode() and use the ato_cli_json_xxx() functions in json mode.
 */

#ifndef ATO_CLI_CORE_H
#define ATO_CLI_CORE_H

#include "FreeRTOS.h"
#include "ato_utils.h" // COUNT_OF
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ATO_CLI_PRINTF_SIZE
#define ATO_CLI_PRINTF_SIZE 128 // longest line of ato_cli_core_printf()
#endif

#ifndef ATO_CLI_MAX_WORDS
#define ATO_CLI_MAX_WORDS 10 // words per line, command name included
#endif

typedef struct ato_cli_core ato_cli_core_t;

typedef enum {
  ATO_CLI_MODE_PLAIN, // for humans
  ATO_CLI_MODE_JSON,  // for scripts
} ato_cli_mode_t;

/**
 * Output sink, provided by the transport. Must write all `len` bytes,
 * blocking if needed (back-pressure); it has no error to report.
 */
typedef void (*ato_cli_write_fn)(void *arg, const char *data, size_t len);

/* Parameters --------------------------------------------------------------- */

typedef enum {
  ATO_CLI_PARAM_INT,    // decimal or 0x hexadecimal, k and M suffixes
  ATO_CLI_PARAM_ENUM,   // one word of a list
  ATO_CLI_PARAM_STRING, // any word
} ato_cli_param_type_t;

/** Description of a command parameter, meant to be a const global. */
typedef struct {
  const char *name; // short, shown in the usage: "<name>"
  const char *help; // one line, shown by "help <command>"
  ato_cli_param_type_t type;
  bool optional; // optional parameters must be the last ones
  union {
    struct {
      int32_t min, max;
    } i; // ATO_CLI_PARAM_INT: accepted range, bounds included
    struct {
      uint8_t count;
      const char *const *values;
    } e; // ATO_CLI_PARAM_ENUM: accepted words
  };
} ato_cli_param_t;

// Parameter descriptors, for a `static const ato_cli_param_t xxx[]` table.
#define ATO_CLI_INT(name_, help_, min_, max_)                                  \
  {                                                                            \
    .name = (name_), .help = (help_), .type = ATO_CLI_PARAM_INT, .i = {        \
      (min_),                                                                  \
      (max_)                                                                   \
    }                                                                          \
  }
// `values_`: a `static const char *const []` array.
#define ATO_CLI_ENUM(name_, help_, values_)                                    \
  {                                                                            \
    .name = (name_), .help = (help_), .type = ATO_CLI_PARAM_ENUM, .e = {       \
      COUNT_OF(values_),                                                       \
      (values_)                                                                \
    }                                                                          \
  }
#define ATO_CLI_STRING(name_, help_)                                           \
  {.name = (name_), .help = (help_), .type = ATO_CLI_PARAM_STRING}
// For an optional parameter, write the initializer with `.optional = true`.

/** Value of a parameter, converted according to its type. */
typedef union {
  int32_t i;     // ATO_CLI_PARAM_INT
  uint8_t e;     // ATO_CLI_PARAM_ENUM: index in `values`
  const char *s; // ATO_CLI_PARAM_STRING: the word, null-terminated
} ato_cli_arg_t;

/* Commands ----------------------------------------------------------------- */

/**
 * Command handler, called once the parameters have been checked.
 * `args[0..argc-1]`: the values of the parameters given (argc is less than
 * the number of parameters when optional ones are missing).
 * Writes its output with ato_cli_core_write(), _puts() or _printf(), as much
 * as it wants, and returns when the command is done. `cli` is the
 * interpreter running the command, `user_data` the pointer given when the
 * command was registered in it (typically the context of the module).
 */
typedef void (*ato_cli_command_fn)(ato_cli_core_t *cli, void *user_data,
                                   const ato_cli_arg_t *args, uint8_t argc);

/** Command descriptor, meant to be a const global. */
typedef struct {
  const char *command; // lower case words separated by one space, "gpio show"
  const char *help;    // one line, without "\r\n"
  ato_cli_command_fn handler;
  const ato_cli_param_t *params; // NULL if none
  uint8_t nb_params;
} ato_cli_command_t;

// X-macro helpers: X(name, help, handler) or X(name, help, handler, params),
// `params` being a `static const ato_cli_param_t []` array.
#define ATO_CLI_X_COUNT(...) +1
#define ATO_CLI_X_DEF(name, help, handler, ...)                                \
  {(name), (help), (handler)__VA_OPT__(, (__VA_ARGS__), COUNT_OF(__VA_ARGS__))},

// Context member declaration (use in the .h in the context struct)
#define ATO_CLI_ITEMS(list)                                                    \
  ato_cli_list_item_t cmd_items[0 list(ATO_CLI_X_COUNT)]
// Command table definition (use in the .c, after the parameter tables)
#define ATO_CLI_TABLE(table, list)                                             \
  static const ato_cli_command_t table[] = {list(ATO_CLI_X_DEF)}

/** Element of the per-interpreter list of registered commands. */
typedef struct ato_cli_list_item {
  const ato_cli_command_t *def;
  void *user_data; // given to the handler
  struct ato_cli_list_item *next;
} ato_cli_list_item_t;

/** Interpreter context. Public so it can be embedded in an owner struct. */
struct ato_cli_core {
  ato_cli_list_item_t commands;  // list head, always the "help" command
  ato_cli_list_item_t *last;     // list tail, for appending
  ato_cli_write_fn write;        // output sink
  void *write_arg;               // its argument (e.g. the UART instance)
  ato_cli_list_item_t mode_item; // the "mode" command
  const char *menu;              // current menu: the first `menu_len`
  size_t menu_len;               // chars of a command name; 0: root
  ato_cli_mode_t mode;           // output mode
  // reply in progress
  bool replied;    // the reply of the current line is complete
  bool line_open;  // plain: a reply line is started
  uint8_t depth;   // json: number of open objects / arrays (0: none)
  uint32_t first;  // json: bit d set: next element of level d is the first
  uint32_t is_arr; // json: bit d set: level d is an array
  char printf_buf[ATO_CLI_PRINTF_SIZE]; // formatting buffer of _printf()
};

/**
 * Initialise an interpreter: empty command list (except "help"), output
 * sent to `write(write_arg, ...)`, root menu.
 */
void ato_cli_core_init(ato_cli_core_t *cli, ato_cli_write_fn write,
                       void *write_arg);

/**
 * Register a command, using a caller-provided list item (no allocation).
 * Can be called from any task, at any time, even while the owner task is
 * running commands (not from an ISR). `def` and `item` must stay valid for
 * the lifetime of the interpreter (typically static / const). An item links
 * one command into one interpreter: use one item per (command, interpreter).
 * `user_data` is given to the handler of the command.
 */
void ato_cli_core_register(ato_cli_core_t *cli, const ato_cli_command_t *def,
                           ato_cli_list_item_t *item, void *user_data);

/**
 * Run the command line `line`; returns when the command is done and all its
 * output has been given to the write callback. `line` is modified (split
 * into words in place).
 * Reentrant: distinct `cli` contexts can be used concurrently; a given
 * context must only be used by one task at a time.
 */
void ato_cli_core_process(ato_cli_core_t *cli, char *line);

/** Write the prompt: "> ", or "<menu>> " in a menu; nothing in json mode. */
void ato_cli_core_prompt(ato_cli_core_t *cli);

/** Output of a command: `len` bytes. */
void ato_cli_core_write(ato_cli_core_t *cli, const char *data, size_t len);

/** Output of a command: null-terminated string, of any length. */
void ato_cli_core_puts(ato_cli_core_t *cli, const char *str);

/**
 * Output of a command: formatted text. The result is truncated to
 * ATO_CLI_PRINTF_SIZE - 1 characters: call it once per line, and use
 * ato_cli_core_puts() for long constant texts.
 */
void ato_cli_core_printf(ato_cli_core_t *cli, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/** Current output mode. */
ato_cli_mode_t ato_cli_core_mode(const ato_cli_core_t *cli);

/* Replies ------------------------------------------------------------------ */
/*
 * A reply is ato_cli_reply_begin(), values, then ato_cli_reply_end(); or
 * ato_cli_reply_error(), at any time (also after a begin, values written so
 * far are then invalid). One reply per command. `key` NULL: value only (an
 * element of a json array).
 *   plain: "ok key=1 key=2\r\n"       or "error: message\r\n"
 *   json:  {"key":1,"key":2,"ok":true} or {...,"error":"message","ok":false}
 */
void ato_cli_reply_begin(ato_cli_core_t *cli);
void ato_cli_reply_int(ato_cli_core_t *cli, const char *key, int32_t value);
void ato_cli_reply_uint(ato_cli_core_t *cli, const char *key, uint32_t value);
/** Value given in thousandths, shown with 3 decimals: 20000.000 */
void ato_cli_reply_milli(ato_cli_core_t *cli, const char *key, uint64_t milli);
void ato_cli_reply_str(ato_cli_core_t *cli, const char *key, const char *str);
void ato_cli_reply_bool(ato_cli_core_t *cli, const char *key, bool value);
void ato_cli_reply_end(ato_cli_core_t *cli);
/** Message without "error:" and without "\r\n". */
void ato_cli_reply_error(ato_cli_core_t *cli, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Nested json values, between ato_cli_reply_begin() and _end(); ignored in
 * plain mode. `key` NULL inside an array.
 */
void ato_cli_json_obj_begin(ato_cli_core_t *cli, const char *key);
void ato_cli_json_obj_end(ato_cli_core_t *cli);
void ato_cli_json_arr_begin(ato_cli_core_t *cli, const char *key);
void ato_cli_json_arr_end(ato_cli_core_t *cli);

#endif /* ATO_CLI_CORE_H */
