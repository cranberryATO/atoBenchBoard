/*
 * Reentrant command line interpreter.
 *
 * Forked from FreeRTOS+CLI V202212.00 (FreeRTOS_CLI.c).
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Modified: all mutable state (command list, list tail, output sink) moved
 * into a per-instance context (ato_cli_core_t), so that several interpreters
 * can run concurrently; command handlers write their output through a
 * callback instead of filling a buffer over several calls; typed
 * parameters, menus and generated help (see ato_cli_core.h).
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
#include <stdlib.h>
#include <string.h>

#define SYNOPSIS_SIZE 64 // "name <p1> [<p2>]" in the usage and the help

static void help_command(ato_cli_core_t *cli, const ato_cli_arg_t *args,
                         uint8_t argc);

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

/* Command lookup ----------------------------------------------------------- */

typedef enum {
  MATCH_NONE,
  MATCH_FULL,   // the words start with the whole command name
  MATCH_PREFIX, // the words are the beginning of the command name (a menu)
} match_t;

typedef struct {
  match_t match;
  const ato_cli_command_t *def;
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
static lookup_t lookup(const ato_cli_core_t *cli, const char *prefix,
                       size_t prefix_len, const char *const *words,
                       uint8_t count) {
  lookup_t best = {.match = MATCH_NONE};

  /* Commands may be appended meanwhile: each `next` is read once, and is
   * either NULL or a fully linked item (see ato_cli_core_register()). */
  for (const ato_cli_list_item_t *item = &cli->commands; item != NULL;
       item = item->next) {
    uint8_t used = 0;
    size_t covered = 0;
    match_t m = match(item->def->command, prefix, prefix_len, words, count,
                      &used, &covered);
    if (m == MATCH_FULL && (best.match != MATCH_FULL || used > best.used)) {
      best = (lookup_t){MATCH_FULL, item->def, used, 0};
    } else if (m == MATCH_PREFIX && best.match == MATCH_NONE) {
      best = (lookup_t){MATCH_PREFIX, item->def, 0, covered};
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

static void put_enum_values(ato_cli_core_t *cli, const ato_cli_param_t *p) {
  for (uint8_t k = 0; k < p->e.count; k++) {
    if (k > 0) {
      ato_cli_core_puts(cli, "|");
    }
    ato_cli_core_puts(cli, p->e.values[k]);
  }
}

/* Convert `word` according to `p`; on error, prints it and returns false. */
static bool convert(ato_cli_core_t *cli, const ato_cli_param_t *p,
                    const char *word, ato_cli_arg_t *arg) {
  switch (p->type) {
  case ATO_CLI_PARAM_INT: {
    int32_t v;
    if (!parse_int(word, &v)) {
      ato_cli_core_printf(cli, "error: <%s>: invalid number '%s'\r\n", p->name,
                          word);
      return false;
    }
    if (v < p->i.min || v > p->i.max) {
      ato_cli_core_printf(cli, "error: <%s>: out of range %ld..%ld\r\n",
                          p->name, (long)p->i.min, (long)p->i.max);
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
    ato_cli_core_printf(cli, "error: <%s>: expected ", p->name);
    put_enum_values(cli, p);
    ato_cli_core_puts(cli, "\r\n");
    return false;
  case ATO_CLI_PARAM_STRING:
    arg->s = word;
    return true;
  }
  return false;
}

static void run_command(ato_cli_core_t *cli, const ato_cli_command_t *def,
                        const char *const *words, uint8_t count) {
  uint8_t required = 0;
  for (uint8_t i = 0; i < def->nb_params; i++) {
    if (!def->params[i].optional) {
      required = i + 1;
    }
  }
  if (count < required || count > def->nb_params) {
    char buf[SYNOPSIS_SIZE];
    synopsis(buf, def->command, def);
    ato_cli_core_printf(cli, "error: usage: %s\r\n", buf);
    return;
  }

  ato_cli_arg_t args[ATO_CLI_MAX_WORDS];
  for (uint8_t i = 0; i < count; i++) {
    if (!convert(cli, &def->params[i], words[i], &args[i])) {
      return;
    }
  }
  def->handler(cli, args, count);
}

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
  cli->menu = NULL;
  cli->menu_len = 0;
}
/*-----------------------------------------------------------*/

void ato_cli_core_register(ato_cli_core_t *cli, const ato_cli_command_t *def,
                           ato_cli_list_item_t *item) {
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

    /* The new list item is appended to the end of the list. */
    item->next = NULL;
    cli->last->next = item;
    cli->last = item;
  }
  taskEXIT_CRITICAL();
}
/*-----------------------------------------------------------*/

void ato_cli_core_process(ato_cli_core_t *cli, char *line) {
  configASSERT(cli != NULL);

  /* Split the line into words, in place. */
  const char *words[ATO_CLI_MAX_WORDS];
  uint8_t count = 0;
  for (char *p = line; *p != '\0';) {
    if (*p == ' ') {
      *p++ = '\0';
      continue;
    }
    if (count == ATO_CLI_MAX_WORDS) {
      ato_cli_core_puts(cli, "error: too many words\r\n");
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

  /* Menu navigation. */
  if (count == 1 && strcmp(words[0], "/") == 0) {
    cli->menu_len = 0;
    return;
  }
  if (count == 1 && strcmp(words[0], "..") == 0) {
    size_t n = cli->menu_len;
    while (n > 0 && cli->menu[n - 1] != ' ') {
      n--;
    }
    cli->menu_len = n > 0 ? n - 1 : 0;
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
    run_command(cli, r.def, words + r.used, count - r.used);
    break;
  case MATCH_PREFIX: // enter the menu; its name is a part of the command name
    cli->menu = r.def->command;
    cli->menu_len = r.covered;
    break;
  case MATCH_NONE:
    ato_cli_core_printf(cli, "error: unknown command '%s', enter 'help'\r\n",
                        words[0]);
    break;
  }
}
/*-----------------------------------------------------------*/

void ato_cli_core_prompt(ato_cli_core_t *cli) {
  if (cli->menu_len > 0) {
    ato_cli_core_printf(cli, "%.*s> ", (int)cli->menu_len, cli->menu);
  } else {
    ato_cli_core_puts(cli, "> ");
  }
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

/* Help --------------------------------------------------------------------- */

static void help_params_detail(ato_cli_core_t *cli,
                               const ato_cli_command_t *def) {
  for (uint8_t i = 0; i < def->nb_params; i++) {
    const ato_cli_param_t *p = &def->params[i];
    ato_cli_core_printf(cli, "      <%s>  ", p->name);
    switch (p->type) {
    case ATO_CLI_PARAM_INT:
      ato_cli_core_printf(cli, "%ld..%ld  ", (long)p->i.min, (long)p->i.max);
      break;
    case ATO_CLI_PARAM_ENUM:
      put_enum_values(cli, p);
      ato_cli_core_puts(cli, "  ");
      break;
    case ATO_CLI_PARAM_STRING:
      break;
    }
    ato_cli_core_printf(cli, "%s%s\r\n", p->help,
                        p->optional ? " (optional)" : "");
  }
}

/*
 * Without parameter: one line per command of the current menu. With a word:
 * the commands starting with it (in the menu, else from the root), with the
 * description of their parameters.
 */
static void help_command(ato_cli_core_t *cli, const ato_cli_arg_t *args,
                         uint8_t argc) {
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
    ato_cli_core_printf(cli, "error: no command '%s'\r\n", args[0].s);
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
      ato_cli_core_printf(cli, "  %-*s  %s\r\n", width, buf, def->help);
      if (argc > 0) {
        help_params_detail(cli, def);
      }
    }
  }
  if (argc == 0 && prefix_len > 0) {
    ato_cli_core_puts(cli, "  (\"..\": up, \"/\": root; the root commands "
                           "also work here)\r\n");
  }
}
/*-----------------------------------------------------------*/
