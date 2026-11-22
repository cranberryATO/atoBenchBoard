#ifndef ATO_UART_H
#define ATO_UART_H

/*
 * Generic DMA byte stream over a HAL UART.
 *
 * - RX: circular DMA with ReceiveToIdle; the registered task is notified
 *   (xTaskNotifyGive) on every RX event and after an RX restart on error.
 * - TX: ring buffer drained by DMA chunks. ato_uart_write*() never block,
 *   bytes that do not fit are dropped and counted; ato_uart_write*_wait()
 *   block until everything is queued (the task is notified on TX complete).
 *
 * Requirements on the UART (CubeMX): RX DMA in circular mode, TX DMA in
 * normal mode, UART and DMA IRQ priorities >= 5
 * (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY) so that critical sections
 * mask them.
 *
 * Threading: one task owns an instance; ato_uart_write*() and
 * ato_uart_rx_*() must only be called from that task.
 */

#include "FreeRTOS.h"
#include "main.h"
#include "task.h"
#include <stdint.h>

#define ATO_UART_RX_SIZE 256  // must be a power of 2
#define ATO_UART_TX_SIZE 1024 // must be a power of 2; holds a CLI output + echo

typedef struct {
  UART_HandleTypeDef *huart;
  TaskHandle_t notify_task; // woken from ISR on RX event / RX restart

  uint8_t tx_buf[ATO_UART_TX_SIZE];
  volatile uint32_t tx_head;
  volatile uint32_t tx_tail;
  volatile uint32_t tx_inflight;
  uint32_t tx_dropped_bytes;
  volatile uint8_t tx_waiting; // the task waits for room in tx_buf

  uint8_t rx_buf[ATO_UART_RX_SIZE];
  volatile uint32_t rx_head;
  uint32_t rx_tail;
  volatile uint8_t rx_restarted; // set by ISR when RX DMA restarts at 0

  // error counters (written in ISR)
  volatile uint32_t rx_errors;
  volatile uint32_t tx_errors;
  volatile uint32_t rx_restart_failures;
} ato_uart_t;

/**
 * @brief  Reset the instance and attach it to `huart`, so that the HAL UART
 *         callbacks are routed to it. A UART can only be attached once.
 */
void ato_uart_init(ato_uart_t *u, UART_HandleTypeDef *huart);

/**
 * @brief  Detach the instance from its UART (aborts any DMA transfer).
 */
void ato_uart_deinit(ato_uart_t *u);

/**
 * @brief  Start DMA reception; `notify_task` is notified on received data.
 */
HAL_StatusTypeDef ato_uart_start_rx(ato_uart_t *u, TaskHandle_t notify_task);

/**
 * @brief  Get the contiguous block of received bytes not yet consumed.
 * @retval Number of bytes available at `*data` (0: nothing to read).
 */
uint32_t ato_uart_rx_peek(ato_uart_t *u, const uint8_t **data);

/**
 * @brief  Release `n` bytes previously returned by ato_uart_rx_peek().
 */
void ato_uart_rx_consume(ato_uart_t *u, uint32_t n);

/**
 * @brief  Queue bytes for transmission (non-blocking, drops on overflow).
 */
void ato_uart_write(ato_uart_t *u, const uint8_t *data, uint16_t len);

/**
 * @brief  Queue a null-terminated string for transmission.
 */
void ato_uart_write_string(ato_uart_t *u, const char *str);

/**
 * @brief  Queue bytes for transmission, waiting for room in the buffer when
 *         needed (nothing is dropped). Must be called from the notify task
 *         given to ato_uart_start_rx(): it sleeps on its task notification,
 *         which may consume an RX notification; the caller must check for
 *         received data (ato_uart_rx_peek) before sleeping again.
 */
void ato_uart_write_wait(ato_uart_t *u, const uint8_t *data, uint32_t len);

/**
 * @brief  Null-terminated string version of ato_uart_write_wait().
 */
void ato_uart_write_string_wait(ato_uart_t *u, const char *str);

#endif /* ATO_UART_H */
