#include "usart.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
/**
 * @brief  Retargets the C library printf function to the USART.
 */
int __io_putchar(int ch) {
  /* Place your implementation of fputc here */
  /* e.g. write a character to the LPUART1 and Loop until the end of
   * transmission */
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)&ch, 1, 0xFFFF);

  return ch;
}

void fatalError(const char *fmt, ...) {
  __disable_irq();
  printf("!!!! FATAL ERROR !!!!\r\n\r\n");
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
  printf("\r\n\r\n");
  while (1) {
  }
}