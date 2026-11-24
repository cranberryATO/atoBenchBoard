#ifndef ATO_GPIO_AF_H
#define ATO_GPIO_AF_H

/*
 * Names of the GPIO alternate functions, by port, pin and AF number.
 * The table is defined in a generated file (Src/ato_gpio_af_<family>.c),
 * see tools/gpio_af/gen_gpio_af.py.
 */

#define ATO_GPIO_AF_NB_PORTS 7 // GPIOA..GPIOG

/** Signal name of each AF number of one pin; NULL: no function. */
typedef const char *const ato_gpio_af_row_t[16];

/** [port index][pin] -> row; a NULL port has no pin in this family. */
extern const ato_gpio_af_row_t *const ato_gpio_af_by_port[ATO_GPIO_AF_NB_PORTS];

#endif // ATO_GPIO_AF_H
