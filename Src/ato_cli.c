/*
 * Reentrant command line interpreter.
 *
 * Forked from FreeRTOS+CLI V202212.00 (FreeRTOS_CLI.c).
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Modified: all mutable state (command list, list tail, output sink) moved
 * into a per-instance context (ato_cli_t), so that several interpreters
 * can run concurrently; command handlers write their output through a
 * callback instead of filling a buffer over several calls; typed
 * parameters, menus and generated help; line editor and task (see
 * ato_cli.h).
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

#include "ato_cli.h"

#include "FreeRTOS.h"
#include "ato_error.h"
#include "task.h" // taskENTER_CRITICAL(); configASSERT() uses taskDISABLE_INTERRUPTS()
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYNOPSIS_SIZE 64    // "name <p1> [<p2>]" in the usage and the help
#define CLI_STACK_WORDS 512 // 2 KB: vsnprintf
#define CLI_PRIORITY (tskIDLE_PRIORITY + 1)

static void help_command(ato_cli_t *cli, void *user_data,
                         const ato_cli_arg_t *args, uint8_t argc);
static void mode_command(ato_cli_t *cli, void *user_data,
                         const ato_cli_arg_t *args, uint8_t argc);

static const ato_cli_param_t help_params[] = {
    {.name = "name",
     .help = "first word of a command or menu",
     .type = ATO_CLI_PARAM_STRING,
     .optional = true},
};

/* The "help" command is always at the head of every command list. */
static const ato_cli_command_t help_command_def = {
    "help", "List the commands, or describe the ones starting with <name>",
    help_command, help_params, COUNT_OF(help_params)};

static const char *const mode_names[] = {"plain", "json"}; // ato_cli_mode_t
static const ato_cli_param_t mode_params[] = {
    ATO_CLI_ENUM("mode", "plain: for humans, json: for scripts", mode_names),
};

/* "mode" follows "help" in every command list. */
static const ato_cli_command_t mode_command_def = {
    "mode", "Select the output: plain (default) or json (for scripts)",
    mode_command, mode_params, COUNT_OF(mode_params)};

/* Command lookup ----------------------------------------------------------- */

typedef enum {
  MATCH_NONE,
  MATCH_FULL,   // the words start with the whole command name
  MATCH_PREFIX, // the words are the beginning of the command name (a menu)
} match_t;

typedef struct {
  match_t match;
  const ato_cli_list_item_t *item;
  uint8_t used;   // MATCH_FULL: number of words of the command name
  size_t covered; // MATCH_PREFIX: length of the name covered by the words
} lookup_t;

/*
 * Compare `words` with the command `name`, below the menu made of the first
 * `prefix_len` chars of `prefix` (0: root). With no word, any command of the
 * menu is a MATCH_PREFIX.
 */
static match_t match(const char *name, const char *prefix, size_t prefix_len,
                     const char *const *words, uint8_t count, uint8_t *used,
                     size_t *covered) {
  const char *p = name;
  if (prefix_len > 0) {
    if (strncmp(name, prefix, prefix_len) != 0 || name[prefix_len] != ' ') {
      return MATCH_NONE;
    }
    p += prefix_len + 1;
  }
  for (uint8_t i = 0; i < count; i++) {
    size_t len = strlen(words[i]);
    if (strncmp(p, words[i], len) != 0 || (p[len] != ' ' && p[len] != '\0')) {
      return MATCH_NONE;
    }
    p += len;
    if (*p == '\0') {
      *used = i + 1;
      return MATCH_FULL;
    }
    p++; // the space before the next word of the name
  }
  *covered = count > 0 ? (size_t)(p - 1 - name) : prefix_len;
  return MATCH_PREFIX;
}

/* Best match in a menu: the longest command name, else a menu. */
static lookup_t lookup(const ato_cli_t *cli, const char *prefix,
                       size_t prefix_len, const char *const *words,
                       uint8_t count) {
  lookup_t best = {.match = MATCH_NONE};

  /* Commands may be appended meanwhile: each `next` is read once, and is
   * either NULL or a fully linked item (see ato_cli_register_command()). */
  for (const ato_cli_list_item_t *item = &cli->commands; item != NULL;
       item = item->next) {
    uint8_t used = 0;
    size_t covered = 0;
    match_t m = match(item->def->command, prefix, prefix_len, words, count,
                      &used, &covered);
    if (m == MATCH_FULL && (best.match != MATCH_FULL || used > best.used)) {
      best = (lookup_t){MATCH_FULL, item, used, 0};
    } else if (m == MATCH_PREFIX && best.match == MATCH_NONE) {
      best = (lookup_t){MATCH_PREFIX, item, 0, covered};
    }
  }
  return best;
}

/* Parameters --------------------------------------------------------------- */

/* "name <p1> [<p2>]", truncated to SYNOPSIS_SIZE - 1; returns its length. */
static int synopsis(char *buf, const char *name, const ato_cli_command_t *def) {
  int n = snprintf(buf, SYNOPSIS_SIZE, "%s", name);
  for (uint8_t i = 0; i < def->nb_params && n < SYNOPSIS_SIZE; i++) {
    const ato_cli_param_t *p = &def->params[i];
    n += snprintf(buf + n, SYNOPSIS_SIZE - n, p->optional ? " [<%s>]" : " <%s>",
                  p->name);
  }
  return n < SYNOPSIS_SIZE ? n : SYNOPSIS_SIZE - 1;
}

/* Decimal, or hexadecimal with 0x; decimal accepts a k or M suffix. */
static bool parse_int(const char *s, int32_t *value) {
  const char *digits = (s[0] == '-' || s[0] == '+') ? s + 1 : s;
  int base =
      digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X') ? 16 : 10;
  char *end;
  long long v = strtoll(s, &end, base); // saturates on overflow
  if (end == s) {
    return false;
  }
  long long mult = 1;
  if (base == 10 && *end == 'k') {
    mult = 1000;
    end++;
  } else if (base == 10 && *end == 'M') {
    mult = 1000000;
    end++;
  }
  if (*end != '\0' || v > INT32_MAX || v < INT32_MIN) {
    return false;
  }
  v *= mult; // no overflow: |v| <= 2^31
  if (v > INT32_MAX || v < INT32_MIN) {
    return false;
  }
  *value = (int32_t)v;
  return true;
}

/* "a|b|c" into buf (SYNOPSIS_SIZE), truncated; returns buf. */
static const char *enum_values(char *buf, const ato_cli_param_t *p) {
  int n = 0;
  buf[0] = '\0';
  for (uint8_t k = 0; k < p->e.count && n < SYNOPSIS_SIZE; k++) {
    n += snprintf(buf + n, SYNOPSIS_SIZE - n, k > 0 ? "|%s" : "%s",
                  p->e.values[k]);
  }
  return buf;
}

/* Convert `word` according to `p`; on error, prints it and returns false. */
static bool convert(ato_cli_t *cli, const ato_cli_param_t *p, const char *word,
                    ato_cli_arg_t *arg) {
  char buf[SYNOPSIS_SIZE];
  switch (p->type) {
  case ATO_CLI_PARAM_INT: {
    int32_t v;
    if (!parse_int(word, &v)) {
      ato_cli_reply_error(cli, "<%s>: invalid number '%s'", p->name, word);
      return false;
    }
    if (v < p->i.min || v > p->i.max) {
      ato_cli_reply_error(cli, "<%s>: out of range %ld..%ld", p->name,
                          (long)p->i.min, (long)p->i.max);
      return false;
    }
    arg->i = v;
    return true;
  }
  case ATO_CLI_PARAM_ENUM:
    for (uint8_t k = 0; k < p->e.count; k++) {
      if (strcmp(word, p->e.values[k]) == 0) {
        arg->e = k;
        return true;
      }
    }
    ato_cli_reply_error(cli, "<%s>: expected %s", p->name, enum_values(buf, p));
    return false;
  case ATO_CLI_PARAM_STRING:
    arg->s = word;
    return true;
  }
  return false;
}

static void run_command(ato_cli_t *cli, const ato_cli_list_item_t *item,
                        const char *const *words, uint8_t count) {
  const ato_cli_command_t *def = item->def;
  uint8_t required = 0;
  for (uint8_t i = 0; i < def->nb_params; i++) {
    if (!def->params[i].optional) {
      required = i + 1;
    }
  }
  if (count < required || count > def->nb_params) {
    char buf[SYNOPSIS_SIZE];
    synopsis(buf, def->command, def);
    ato_cli_reply_error(cli, "usage: %s", buf);
    return;
  }

  ato_cli_arg_t args[ATO_CLI_MAX_WORDS];
  for (uint8_t i = 0; i < count; i++) {
    if (!convert(cli, &def->params[i], words[i], &args[i])) {
      return;
    }
  }
  def->handler(cli, item->user_data, args, count);

  /* Safety net: a script must get one reply per line. */
  if (cli->mode == ATO_CLI_MODE_JSON && !cli->replied) {
    ato_cli_reply_error(cli, cli->depth > 0 ? "incomplete reply" : "no reply");
  }
}

/*-----------------------------------------------------------*/

/* Empty command list (except "help" and "mode"), root menu, plain mode. */
static void cli_init(ato_cli_t *cli, ato_cli_read_fn read_fn,
                     ato_cli_write_fn write_fn, void *io_arg) {
  configASSERT(cli != NULL);
  configASSERT(read_fn != NULL);
  configASSERT(write_fn != NULL);

  cli->commands.def = &help_command_def;
  cli->commands.user_data = NULL;
  cli->commands.next = &cli->mode_item;
  cli->mode_item.def = &mode_command_def;
  cli->mode_item.user_data = NULL;
  cli->mode_item.next = NULL;
  cli->last = &cli->mode_item;
  cli->write_fn = write_fn;
  cli->read_fn = read_fn;
  cli->io_arg = io_arg;
  cli->menu = NULL;
  cli->menu_len = 0;
  cli->mode = ATO_CLI_MODE_PLAIN;
  cli->replied = false;
  cli->line_open = false;
  cli->depth = 0;
  cli->cmd_idx = 0;
  cli->discard = false;
  cli->last_char_cr = false;
}
/*-----------------------------------------------------------*/

void ato_cli_register_command(ato_cli_t *cli, const ato_cli_command_t *def,
                              ato_cli_list_item_t *item, void *user_data) {
  configASSERT(cli != NULL);
  configASSERT(def != NULL);
  configASSERT(item != NULL);
  configASSERT(def->nb_params < ATO_CLI_MAX_WORDS);
  for (uint8_t i = 1; i < def->nb_params; i++) {
    // optional parameters must be the last ones
    configASSERT(def->params[i].optional || !def->params[i - 1].optional);
  }

  /* Commands may be registered from any task, while the owner task walks the
   * list (process / help): the append must be atomic for that reader, and
   * protects `last` against concurrent registrations. */
  taskENTER_CRITICAL();
  {
    /* Reference the command being registered from the list item. */
    item->def = def;
    item->user_data = user_data;

    /* The new list item is appended to the end of the list. */
    item->next = NULL;
    cli->last->next = item;
    cli->last = item;
  }
  taskEXIT_CRITICAL();
}
/*-----------------------------------------------------------*/

static void json_key(ato_cli_t *cli, const char *key);
static void json_str(ato_cli_t *cli, const char *str, size_t len);

/* json mode: reply to a menu change, {"menu":"gen","ok":true}. */
static void menu_reply(ato_cli_t *cli) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    ato_cli_reply_begin(cli);
    json_key(cli, "menu");
    json_str(cli, cli->menu, cli->menu_len);
    ato_cli_reply_end(cli);
  }
}

/*
 * Run the command line `line`; returns when the command is done and all its
 * output has been given to the write callback. `line` is modified (split
 * into words in place).
 */
static void process_line(ato_cli_t *cli, char *line) {
  configASSERT(cli != NULL);
  cli->replied = false;
  cli->line_open = false;
  cli->depth = 0;

  /* Split the line into words, in place. */
  const char *words[ATO_CLI_MAX_WORDS];
  uint8_t count = 0;
  for (char *p = line; *p != '\0';) {
    if (*p == ' ') {
      *p++ = '\0';
      continue;
    }
    if (count == ATO_CLI_MAX_WORDS) {
      ato_cli_reply_error(cli, "too many words");
      return;
    }
    words[count++] = p;
    while (*p != '\0' && *p != ' ') {
      p++;
    }
  }
  if (count == 0) {
    return;
  }

  /* Menu navigation; silent in plain mode (the prompt shows the menu). */
  if (count == 1 && strcmp(words[0], "/") == 0) {
    cli->menu_len = 0;
    menu_reply(cli);
    return;
  }
  if (count == 1 && strcmp(words[0], "..") == 0) {
    size_t n = cli->menu_len;
    while (n > 0 && cli->menu[n - 1] != ' ') {
      n--;
    }
    cli->menu_len = n > 0 ? n - 1 : 0;
    menu_reply(cli);
    return;
  }

  /* In the current menu first, then from the root. */
  lookup_t r = {.match = MATCH_NONE};
  if (cli->menu_len > 0) {
    r = lookup(cli, cli->menu, cli->menu_len, words, count);
  }
  if (r.match == MATCH_NONE) {
    r = lookup(cli, NULL, 0, words, count);
  }

  switch (r.match) {
  case MATCH_FULL:
    run_command(cli, r.item, words + r.used, count - r.used);
    break;
  case MATCH_PREFIX: // enter the menu; its name is a part of the command name
    cli->menu = r.item->def->command;
    cli->menu_len = r.covered;
    menu_reply(cli);
    break;
  case MATCH_NONE:
    ato_cli_reply_error(cli, "unknown command '%s', enter 'help'", words[0]);
    break;
  }
}
/*-----------------------------------------------------------*/

/* The prompt: "> ", or "<menu>> " in a menu; nothing in json mode. */
static void prompt(ato_cli_t *cli) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    return;
  }
  if (cli->menu_len > 0) {
    ato_cli_printf(cli, "%.*s> ", (int)cli->menu_len, cli->menu);
  } else {
    ato_cli_puts(cli, "> ");
  }
}
/*-----------------------------------------------------------*/

void ato_cli_write(ato_cli_t *cli, const char *data, size_t len) {
  if (len > 0) {
    cli->write_fn(cli->io_arg, (const uint8_t *)data, len);
  }
}
/*-----------------------------------------------------------*/

void ato_cli_puts(ato_cli_t *cli, const char *str) {
  ato_cli_write(cli, str, strlen(str));
}
/*-----------------------------------------------------------*/

void ato_cli_printf(ato_cli_t *cli, const char *fmt, ...) {
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
  ato_cli_write(cli, cli->printf_buf, len);
}
/*-----------------------------------------------------------*/

ato_cli_mode_t ato_cli_mode(const ato_cli_t *cli) { return cli->mode; }
/*-----------------------------------------------------------*/

/* Replies ------------------------------------------------------------------ */

#define BIT(d) (1UL << (d))

/* json: separator before a new element of the current level, then its key. */
static void json_key(ato_cli_t *cli, const char *key) {
  if (cli->depth > 0) {
    if (cli->first & BIT(cli->depth)) {
      cli->first &= ~BIT(cli->depth);
    } else {
      ato_cli_write(cli, ",", 1);
    }
  }
  if (key != NULL) {
    json_str(cli, key, strlen(key));
    ato_cli_write(cli, ":", 1);
  }
}

/* json: quoted string, with the escapes needed. */
static void json_str(ato_cli_t *cli, const char *str, size_t len) {
  ato_cli_write(cli, "\"", 1);
  size_t start = 0; // first char not written yet
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (c == '"' || c == '\\' || c < 0x20) {
      ato_cli_write(cli, str + start, i - start);
      char esc[8];
      int n = c < 0x20 ? snprintf(esc, sizeof(esc), "\\u%04x", c)
                       : snprintf(esc, sizeof(esc), "\\%c", c);
      ato_cli_write(cli, esc, (size_t)n);
      start = i + 1;
    }
  }
  ato_cli_write(cli, str + start, len - start);
  ato_cli_write(cli, "\"", 1);
}

static void json_open(ato_cli_t *cli, const char *key, bool array) {
  json_key(cli, key);
  ato_cli_write(cli, array ? "[" : "{", 1);
  cli->depth++;
  configASSERT(cli->depth < 32);
  cli->first |= BIT(cli->depth);
  if (array) {
    cli->is_arr |= BIT(cli->depth);
  } else {
    cli->is_arr &= ~BIT(cli->depth);
  }
}

static void json_close(ato_cli_t *cli) {
  configASSERT(cli->depth > 0);
  ato_cli_write(cli, (cli->is_arr & BIT(cli->depth)) ? "]" : "}", 1);
  cli->depth--;
}

/* json: close the open levels but the reply object, then "ok". */
static void json_finish(ato_cli_t *cli, bool ok) {
  while (cli->depth > 1) {
    json_close(cli);
  }
  json_key(cli, "ok");
  ato_cli_puts(cli, ok ? "true}\r\n" : "false}\r\n");
  cli->depth = 0;
  cli->replied = true;
}

/* plain: " key=" before a value; " " alone without key. */
static void plain_key(ato_cli_t *cli, const char *key) {
  ato_cli_write(cli, " ", 1);
  if (key != NULL) {
    ato_cli_puts(cli, key);
    ato_cli_write(cli, "=", 1);
  }
}

void ato_cli_reply_begin(ato_cli_t *cli) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    cli->depth = 0;
    json_open(cli, NULL, false);
  } else {
    ato_cli_puts(cli, "ok");
    cli->line_open = true;
  }
}

void ato_cli_reply_int(ato_cli_t *cli, const char *key, int32_t value) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_key(cli, key);
  } else {
    plain_key(cli, key);
  }
  ato_cli_printf(cli, "%ld", (long)value);
}

void ato_cli_reply_uint(ato_cli_t *cli, const char *key, uint32_t value) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_key(cli, key);
  } else {
    plain_key(cli, key);
  }
  ato_cli_printf(cli, "%lu", (unsigned long)value);
}

void ato_cli_reply_milli(ato_cli_t *cli, const char *key, uint64_t milli) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_key(cli, key);
  } else {
    plain_key(cli, key);
  }
  // no 64-bit printf with newlib nano
  ato_cli_printf(cli, "%lu.%03lu", (unsigned long)(milli / 1000),
                 (unsigned long)(milli % 1000));
}

void ato_cli_reply_str(ato_cli_t *cli, const char *key, const char *str) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_key(cli, key);
    json_str(cli, str, strlen(str));
  } else {
    plain_key(cli, key);
    ato_cli_puts(cli, str);
  }
}

void ato_cli_reply_bool(ato_cli_t *cli, const char *key, bool value) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_key(cli, key);
  } else {
    plain_key(cli, key);
  }
  ato_cli_puts(cli, value ? "true" : "false");
}

void ato_cli_reply_end(ato_cli_t *cli) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    configASSERT(cli->depth > 0); // ato_cli_reply_begin() missing
    json_finish(cli, true);
  } else {
    ato_cli_puts(cli, "\r\n");
    cli->line_open = false;
    cli->replied = true;
  }
}

void ato_cli_reply_error(ato_cli_t *cli, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(cli->printf_buf, sizeof(cli->printf_buf), fmt, args);
  va_end(args);
  size_t len = n < 0 ? 0
               : (size_t)n < sizeof(cli->printf_buf)
                   ? (size_t)n
                   : sizeof(cli->printf_buf) - 1;

  if (cli->mode == ATO_CLI_MODE_JSON) {
    if (cli->depth == 0) {
      json_open(cli, NULL, false);
    }
    // the levels above the reply object are closed first, then the message
    // (json_str() does not use printf_buf)
    while (cli->depth > 1) {
      json_close(cli);
    }
    json_key(cli, "error");
    json_str(cli, cli->printf_buf, len);
    json_finish(cli, false);
  } else {
    if (cli->line_open) {
      ato_cli_puts(cli, "\r\n");
    }
    ato_cli_puts(cli, "error: ");
    ato_cli_write(cli, cli->printf_buf, len);
    ato_cli_puts(cli, "\r\n");
    cli->line_open = false;
    cli->replied = true;
  }
}

void ato_cli_json_obj_begin(ato_cli_t *cli, const char *key) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_open(cli, key, false);
  }
}

void ato_cli_json_obj_end(ato_cli_t *cli) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_close(cli);
  }
}

void ato_cli_json_arr_begin(ato_cli_t *cli, const char *key) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_open(cli, key, true);
  }
}

void ato_cli_json_arr_end(ato_cli_t *cli) {
  if (cli->mode == ATO_CLI_MODE_JSON) {
    json_close(cli);
  }
}
/*-----------------------------------------------------------*/

/* Mode --------------------------------------------------------------------- */

static void mode_command(ato_cli_t *cli, void *user_data,
                         const ato_cli_arg_t *args, uint8_t argc) {
  (void)user_data;
  (void)argc;
  cli->mode = (ato_cli_mode_t)args[0].e;
  ato_cli_reply_begin(cli); // in the new mode
  ato_cli_reply_str(cli, "mode", mode_names[cli->mode]);
  ato_cli_reply_end(cli);
}
/*-----------------------------------------------------------*/

/* Help --------------------------------------------------------------------- */

static void help_params_detail(ato_cli_t *cli, const ato_cli_command_t *def) {
  for (uint8_t i = 0; i < def->nb_params; i++) {
    const ato_cli_param_t *p = &def->params[i];
    ato_cli_printf(cli, "      <%s>  ", p->name);
    switch (p->type) {
    case ATO_CLI_PARAM_INT:
      ato_cli_printf(cli, "%ld..%ld  ", (long)p->i.min, (long)p->i.max);
      break;
    case ATO_CLI_PARAM_ENUM: {
      char buf[SYNOPSIS_SIZE];
      ato_cli_printf(cli, "%s  ", enum_values(buf, p));
      break;
    }
    case ATO_CLI_PARAM_STRING:
      break;
    }
    ato_cli_printf(cli, "%s%s\r\n", p->help, p->optional ? " (optional)" : "");
  }
}

/*
 * Without parameter: one line per command of the current menu. With a word:
 * the commands starting with it (in the menu, else from the root), with the
 * description of their parameters.
 */
static void help_command(ato_cli_t *cli, void *user_data,
                         const ato_cli_arg_t *args, uint8_t argc) {
  (void)user_data;
  const char *const *words = argc > 0 ? &args[0].s : NULL;
  const char *prefix = cli->menu;
  size_t prefix_len = cli->menu_len;
  uint8_t used;
  size_t covered;

  if (argc > 0 && prefix_len > 0 &&
      lookup(cli, prefix, prefix_len, words, argc).match == MATCH_NONE) {
    prefix_len = 0;
  }

  /* Pass 1: width of the first column. */
  int width = 0;
  for (const ato_cli_list_item_t *item = &cli->commands; item != NULL;
       item = item->next) {
    const ato_cli_command_t *def = item->def;
    if (match(def->command, prefix, prefix_len, words, argc, &used, &covered) !=
        MATCH_NONE) {
      char buf[SYNOPSIS_SIZE];
      int n =
          synopsis(buf, def->command + (prefix_len ? prefix_len + 1 : 0), def);
      width = n > width ? n : width;
    }
  }
  if (width == 0) {
    ato_cli_reply_error(cli, "no command '%s'", args[0].s);
    return;
  }

  /* json: {"commands":[{"command":..,"usage":..,"help":..},...],"ok":true},
   * full command names, without the description of the parameters. */
  if (cli->mode == ATO_CLI_MODE_JSON) {
    ato_cli_reply_begin(cli);
    ato_cli_json_arr_begin(cli, "commands");
    for (const ato_cli_list_item_t *item = &cli->commands; item != NULL;
         item = item->next) {
      const ato_cli_command_t *def = item->def;
      if (match(def->command, prefix, prefix_len, words, argc, &used,
                &covered) != MATCH_NONE) {
        char buf[SYNOPSIS_SIZE];
        synopsis(buf, def->command, def);
        ato_cli_json_obj_begin(cli, NULL);
        ato_cli_reply_str(cli, "command", def->command);
        ato_cli_reply_str(cli, "usage", buf);
        ato_cli_reply_str(cli, "help", def->help);
        ato_cli_json_obj_end(cli);
      }
    }
    ato_cli_json_arr_end(cli);
    ato_cli_reply_end(cli);
    return;
  }

  /* Pass 2: the commands; names relative to the menu. */
  for (const ato_cli_list_item_t *item = &cli->commands; item != NULL;
       item = item->next) {
    const ato_cli_command_t *def = item->def;
    if (match(def->command, prefix, prefix_len, words, argc, &used, &covered) !=
        MATCH_NONE) {
      char buf[SYNOPSIS_SIZE];
      synopsis(buf, def->command + (prefix_len ? prefix_len + 1 : 0), def);
      ato_cli_printf(cli, "  %-*s  %s\r\n", width, buf, def->help);
      if (argc > 0) {
        help_params_detail(cli, def);
      }
    }
  }
  if (argc == 0 && prefix_len > 0) {
    ato_cli_puts(cli, "  (\"..\": up, \"/\": root; the root commands "
                      "also work here)\r\n");
  }
}
/*-----------------------------------------------------------*/

void ato_cli_register_command_table(ato_cli_t *cli,
                                    const ato_cli_command_t *commands,
                                    size_t nb_commands,
                                    ato_cli_list_item_t *items,
                                    void *user_data) {
  for (size_t i = 0; i < nb_commands; i++) {
    ato_cli_register_command(cli, &commands[i], &items[i], user_data);
  }
}
/*-----------------------------------------------------------*/

/* Line editor -------------------------------------------------------------- */

// Echo, end of line (\r, \n or \r\n), backspace, overlong lines.
// No echo in json mode (scripts).
static void line_editor(ato_cli_t *cli, const uint8_t *bytes, size_t size) {
  for (size_t i = 0; i < size; i++) {
    char c = (char)bytes[i];
    // read for each byte: a command of the line may change the mode
    bool echo = cli->mode == ATO_CLI_MODE_PLAIN;

    if (c == '\r' || c == '\n') {
      bool is_lf_after_cr = (c == '\n') && cli->last_char_cr;
      cli->last_char_cr = (c == '\r');
      if (is_lf_after_cr) {
        continue; // second half of \r\n, line already handled
      }
      if (echo) {
        ato_cli_puts(cli, "\r\n");
      }
      if (!cli->discard && cli->cmd_idx > 0) {
        cli->cmd_buf[cli->cmd_idx] = '\0';
        process_line(cli, cli->cmd_buf);
      }
      cli->cmd_idx = 0;
      cli->discard = false;
      prompt(cli);
      continue;
    }
    cli->last_char_cr = false;

    if (cli->discard) {
      continue;
    }

    if (c == 0x7f || c == '\b') { // DEL (most terminals) or BS
      if (cli->cmd_idx > 0) {
        cli->cmd_idx--;
        if (echo) {
          ato_cli_puts(cli, "\b \b"); // erase the character on screen
        }
      }
      continue;
    }

    if ((unsigned char)c < 0x20) {
      continue; // ignore other control characters (no echo)
    }

    if (cli->cmd_idx >= sizeof(cli->cmd_buf) - 1) {
      if (echo) {
        ato_cli_puts(cli, "\r\n"); // the error after the echoed text
      }
      ato_cli_reply_error(cli, "command too long, line ignored");
      cli->discard = true;
      continue;
    }

    cli->cmd_buf[cli->cmd_idx++] = c;
    if (echo) {
      ato_cli_write(cli, &c, 1);
    }
  }
}

/* Task --------------------------------------------------------------------- */

static void cli_task(void *argument) {
  ato_cli_t *cli = argument;

  prompt(cli);
  for (;;) {
    size_t n = cli->read_fn(cli->io_arg, cli->rx_buf, sizeof(cli->rx_buf));
    line_editor(cli, cli->rx_buf, n);
  }
  /* A task must never return; if it must end, call vTaskDelete(NULL). */
}

void ato_cli_create_task(ato_cli_t *cli, const char *task_name,
                         ato_cli_read_fn read_fn, ato_cli_write_fn write_fn,
                         void *io_arg, TaskHandle_t *created_task) {
  cli_init(cli, read_fn, write_fn, io_arg);

  if (xTaskCreate(cli_task, task_name, CLI_STACK_WORDS, cli, CLI_PRIORITY,
                  created_task) != pdPASS) {
    fatalError("Task creation failed for task %s", task_name);
  }
}
/*-----------------------------------------------------------*/
