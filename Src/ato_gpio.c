#include "ato_cli.h"
#include "ato_cli_core.h"
#include "ato_error.h"
#include "ato_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "ato_gpio.h"
#include "ato_gpio_af.h"
#include "stm32g474xx.h"
#include "stm32g4xx.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_utils.h"

#define PIN(x) | (1U << (x))
#define PIN_LIST(list) (0 list)

#define GPIO_DEFINITION(name) {#name, name}

static inline uint32_t gpio_port_idx(GPIO_TypeDef *addr) {
  return ((uint32_t)addr - GPIOA_BASE) / 0x400;
}

static const struct gpio_definition {
  const char *name;
  GPIO_TypeDef *addr;
} GPIO_ADDRESSES[] = {
    GPIO_DEFINITION(GPIOA), GPIO_DEFINITION(GPIOB), GPIO_DEFINITION(GPIOC),
    GPIO_DEFINITION(GPIOD), GPIO_DEFINITION(GPIOE), GPIO_DEFINITION(GPIOF),
    GPIO_DEFINITION(GPIOG),
};

// !! in tables below, unless specified otherwise, indexes match hardware
// values. Do not change without checking chip datasheet

static const char *const GPIO_MODE_STR[] = {"IN", "OUT", "AF", "ANL"};

static const char *const nb[] = {"0", "1", "2",  "3",  "4",  "5",  "6",  "7",
                                 "8", "9", "10", "11", "12", "13", "14", "15"};

static const char *gpio_af_str(GPIO_TypeDef *addr, int pin, int af) {
  if (LL_GPIO_GetPinMode(addr, 1 << pin) != LL_GPIO_MODE_ALTERNATE)
    return "-";
  const ato_gpio_af_row_t *rows = ato_gpio_af_by_port[gpio_port_idx(addr)];
  const char *name = rows ? rows[pin][af] : NULL;
  return name ? name : nb[af];
}

static const char *const GPIO_PULL[] = {"NO", "UP", "DO", "??"};

static uint32_t get_available_pins(GPIO_TypeDef *addr) {
  switch (LL_GetPackageType()) {
  case LL_UTILS_PACKAGETYPE_LQFP64:
    switch ((uint32_t)addr) {
    case (uint32_t)GPIOA:
    case (uint32_t)GPIOB:
    case (uint32_t)GPIOC:
      return 0xffff;
    case (uint32_t)GPIOD:
      return PIN_LIST(PIN(2));
    case (uint32_t)GPIOE:
      return 0;
    case (uint32_t)GPIOF:
      return PIN_LIST(PIN(0) PIN(1));
    case (uint32_t)GPIOG:
      return PIN_LIST(PIN(10));
    default:
      fatalError("Invalid GPIO handle %p", addr);
    }
  default:
    fatalError("Package not implemented.");
  }
}

// plain: one line per pin.
// json: {"ports":[{"port":"GPIOA","clock":true,"pins":[{"pin":0,"mode":"ANL",
//        "pull":"NO","level":0,"af":"..."},...]},...]}
static void gshow_command(ato_cli_core_t *cli, void *user_data,
                          const ato_cli_arg_t *args, uint8_t argc) {
  (void)user_data;
  (void)args;
  (void)argc;
  bool json = ato_cli_core_mode(cli) == ATO_CLI_MODE_JSON;
  if (json) {
    ato_cli_reply_begin(cli);
    ato_cli_json_arr_begin(cli, "ports");
  } else {
    ato_cli_core_puts(cli, "GPIOx PIN MODE PUL LVL ALTERNATE_FUNC \r\n");
  }
  for (int i = 0; i < COUNT_OF(GPIO_ADDRESSES); i++) {
    const struct gpio_definition *gpio = &GPIO_ADDRESSES[i];
    bool clock = LL_AHB2_GRP1_IsEnabledClock(1U << i);
    if (json) {
      ato_cli_json_obj_begin(cli, NULL);
      ato_cli_reply_str(cli, "port", gpio->name);
      ato_cli_reply_bool(cli, "clock", clock);
      ato_cli_json_arr_begin(cli, "pins");
    } else if (!clock) {
      ato_cli_core_printf(cli, "%s : clock is not enabled\r\n", gpio->name);
    }
    if (clock) {
      uint32_t available_pins = get_available_pins(gpio->addr);
      for (int pin = 0; pin < 16; pin++) {
        if (available_pins & (1U << pin)) {
          uint32_t bsr = 1U << pin;
          uint32_t mode =
              LL_GPIO_GetPinMode(gpio->addr, bsr); // LL_GPIO_MODE_ALTERNATE...
          uint32_t af = pin < 8 ? LL_GPIO_GetAFPin_0_7(gpio->addr, bsr)
                                : LL_GPIO_GetAFPin_8_15(gpio->addr, bsr);

          uint32_t pull = LL_GPIO_GetPinPull(gpio->addr, bsr);
          uint32_t lvl = LL_GPIO_IsInputPinSet(gpio->addr, bsr);
          if (json) {
            ato_cli_json_obj_begin(cli, NULL);
            ato_cli_reply_uint(cli, "pin", pin);
            ato_cli_reply_str(cli, "mode", GPIO_MODE_STR[mode]);
            ato_cli_reply_str(cli, "pull", GPIO_PULL[pull]);
            ato_cli_reply_uint(cli, "level", lvl);
            ato_cli_reply_str(cli, "af", gpio_af_str(gpio->addr, pin, af));
            ato_cli_json_obj_end(cli);
          } else {
            ato_cli_core_printf(cli, "%s %3d %4s %3s %3lx %s\r\n", gpio->name,
                                pin, GPIO_MODE_STR[mode], GPIO_PULL[pull],
                                (unsigned long)lvl,
                                gpio_af_str(gpio->addr, pin, af));
          }
        }
      }
    }
    if (json) {
      ato_cli_json_arr_end(cli);
      ato_cli_json_obj_end(cli);
    }
  }
  if (json) {
    ato_cli_json_arr_end(cli);
    ato_cli_reply_end(cli);
  }
}

// "PA5" (or "pa5") -> port and pin number; false if not a pin of the package.
static bool parse_pin(const char *name, const struct gpio_definition **gpio,
                      uint32_t *pin) {
  if (toupper((unsigned char)name[0]) != 'P') {
    return false;
  }
  int port = toupper((unsigned char)name[1]) - 'A';
  if (port < 0 || port >= (int)COUNT_OF(GPIO_ADDRESSES) ||
      !isdigit((unsigned char)name[2])) {
    return false;
  }
  char *end;
  unsigned long n = strtoul(&name[2], &end, 10);
  if (*end != '\0' || n > 15) {
    return false;
  }
  *gpio = &GPIO_ADDRESSES[port];
  *pin = n;
  return (get_available_pins((*gpio)->addr) & (1U << n)) != 0;
}

static const ato_cli_param_t gset_params[] = {
    ATO_CLI_STRING("pin", "pin name, e.g. PA5"),
    ATO_CLI_INT("level", "output level", 0, 1),
};

// Only on a pin configured as output (CubeMX): the configuration of the
// pins used by peripherals is never changed.
static void gset_command(ato_cli_core_t *cli, void *user_data,
                         const ato_cli_arg_t *args, uint8_t argc) {
  (void)user_data;
  (void)argc;
  const struct gpio_definition *gpio;
  uint32_t pin;
  if (!parse_pin(args[0].s, &gpio, &pin)) {
    ato_cli_reply_error(cli, "no pin '%s' in this package", args[0].s);
    return;
  }
  if (!LL_AHB2_GRP1_IsEnabledClock(1U << gpio_port_idx(gpio->addr))) {
    ato_cli_reply_error(cli, "%s: clock is not enabled", gpio->name);
    return;
  }
  uint32_t bsr = 1U << pin;
  if (LL_GPIO_GetPinMode(gpio->addr, bsr) != LL_GPIO_MODE_OUTPUT) {
    ato_cli_reply_error(cli, "%s is not an output", args[0].s);
    return;
  }
  if (args[1].i) {
    LL_GPIO_SetOutputPin(gpio->addr, bsr);
  } else {
    LL_GPIO_ResetOutputPin(gpio->addr, bsr);
  }
  char name[5];
  snprintf(name, sizeof(name), "P%c%lu", gpio->name[4], (unsigned long)pin);
  ato_cli_reply_begin(cli);
  ato_cli_reply_str(cli, "pin", name);
  ato_cli_reply_uint(cli, "level", LL_GPIO_IsInputPinSet(gpio->addr, bsr));
  ato_cli_reply_end(cli);
}

ATO_CLI_TABLE(gpio_commands, ATO_GPIO_COMMANDS);

void ato_gpio_init(ato_gpio_ctx_t *ctx, ato_cli_ctx_t *cli) {
  for (size_t i = 0; i < COUNT_OF(gpio_commands); i++) {
    ato_cli_register_command(cli, &gpio_commands[i], &ctx->cmd_items[i], ctx);
  }
}
