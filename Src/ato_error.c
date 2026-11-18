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
  // BSOD :D blue background, bright white text, clear screen, cursor home.
  printf("\x1b[44m"); // background blue
  printf("\x1b[97m"); // text white
  // printf("\x1b[2J");  // clear screen
  // printf("\x1b[H"); // cursor on top left
  printf("\r\n  :(\r\n\r\n");
  printf("  Your MCU ran into a problem and needs to restart.\r\n\r\n");
  printf("  !!!! FATAL ERROR !!!!\r\n\r\n  ");
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
  printf("\r\n\r\n");
  fflush(stdout);
  while (1) {
  }
}