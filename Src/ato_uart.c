#include "ato_uart.h"

#include <string.h>

_Static_assert((ATO_UART_RX_SIZE & (ATO_UART_RX_SIZE - 1)) == 0,
               "ATO_UART_RX_SIZE must be a power of 2");
_Static_assert((ATO_UART_TX_SIZE & (ATO_UART_TX_SIZE - 1)) == 0,
               "ATO_UART_TX_SIZE must be a power of 2");
_Static_assert(ATO_UART_TX_SIZE <= 65535,
               "a TX DMA chunk length must fit in uint16_t");

static inline uint32_t rx_mask(uint32_t x) {
  return x & (ATO_UART_RX_SIZE - 1);
}
static inline uint32_t tx_mask(uint32_t x) {
  return x & (ATO_UART_TX_SIZE - 1);
}

// Instances attached to a UART, indexed by UART peripheral (not by HAL handle
// name, so this module does not depend on CubeMX naming). The contexts are
// owned by the caller; this table only references them for the HAL callbacks.
#define ATO_UART_NB_PERIPH 6
static ato_uart_t *s_by_periph[ATO_UART_NB_PERIPH];

static int32_t periph_index(const USART_TypeDef *inst) {
  if (inst == LPUART1) return 0;
  if (inst == USART1) return 1;
  if (inst == USART2) return 2;
  if (inst == USART3) return 3;
  if (inst == UART4) return 4;
  if (inst == UART5) return 5;
  return -1;
}

static ato_uart_t *find_instance(const UART_HandleTypeDef *huart) {
  int32_t i = periph_index(huart->Instance);
  if (i < 0) {
    return NULL;
  }
  ato_uart_t *u = s_by_periph[i];
  return (u != NULL && u->huart == huart) ? u : NULL;
}

/* Setup -------------------------------------------------------------------- */

void ato_uart_init(ato_uart_t *u, UART_HandleTypeDef *huart) {
  configASSERT(u != NULL);
  configASSERT(huart != NULL);
  configASSERT(huart->hdmarx != NULL); // RX DMA must be configured in CubeMX
  configASSERT(huart->hdmatx != NULL); // TX DMA must be configured in CubeMX

  int32_t i = periph_index(huart->Instance);
  configASSERT(i >= 0); // unknown UART peripheral

  memset(u, 0, sizeof(*u));
  u->huart = huart;

  taskENTER_CRITICAL();
  configASSERT(s_by_periph[i] == NULL); // UART already attached
  s_by_periph[i] = u;
  taskEXIT_CRITICAL();
}

void ato_uart_deinit(ato_uart_t *u) {
  HAL_UART_Abort(u->huart);
  int32_t i = periph_index(u->huart->Instance);
  taskENTER_CRITICAL();
  if (i >= 0 && s_by_periph[i] == u) {
    s_by_periph[i] = NULL;
  }
  taskEXIT_CRITICAL();
}

HAL_StatusTypeDef ato_uart_start_rx(ato_uart_t *u, TaskHandle_t notify_task) {
  // must be set before RX DMA starts: the UART ISRs notify this task
  u->notify_task = notify_task;
  return HAL_UARTEx_ReceiveToIdle_DMA(u->huart, u->rx_buf, ATO_UART_RX_SIZE);
}

/* RX ----------------------------------------------------------------------- */

uint32_t ato_uart_rx_peek(ato_uart_t *u, const uint8_t **data) {
  // Snapshot head and handle an RX restart atomically w.r.t. the UART/DMA
  // ISRs (priority >= configMAX_SYSCALL_INTERRUPT_PRIORITY, so masked).
  taskENTER_CRITICAL();
  if (u->rx_restarted) {
    u->rx_restarted = 0;
    u->rx_tail = 0; // DMA restarted writing at index 0
  }
  uint32_t head = u->rx_head;
  taskEXIT_CRITICAL();

  uint32_t tail = u->rx_tail;
  *data = &u->rx_buf[tail];
  if (head == tail) {
    return 0;
  }
  // wrapped: return from tail to end of buffer, the rest on the next call
  return head < tail ? ATO_UART_RX_SIZE - tail : head - tail;
}

void ato_uart_rx_consume(ato_uart_t *u, uint32_t n) {
  u->rx_tail = rx_mask(u->rx_tail + n);
}

/* TX ----------------------------------------------------------------------- */

// called from the owner task and under interrupt
static void tx_next_chunk(ato_uart_t *u) {
  if (u->tx_inflight > 0 || u->tx_head == u->tx_tail) {
    return;
  }
  uint32_t head = u->tx_head;
  uint32_t tail = u->tx_tail;
  uint16_t len = head > tail ? head - tail : ATO_UART_TX_SIZE - tail;
  u->tx_inflight = len;
  if (HAL_UART_Transmit_DMA(u->huart, &u->tx_buf[tail], len) != HAL_OK) {
    u->tx_inflight = 0;
  }
}

static uint32_t tx_free(const ato_uart_t *u) { // always leave an empty byte
  return ATO_UART_TX_SIZE - 1 -
         ((u->tx_head - u->tx_tail) & (ATO_UART_TX_SIZE - 1));
}

void ato_uart_write(ato_uart_t *u, const uint8_t *data, uint16_t len) {
  uint32_t n = tx_free(u);
  if (n > len) {
    n = len;
  }
  u->tx_dropped_bytes += len - n;

  uint32_t head = u->tx_head; // local copy: only this function writes head
  for (uint32_t i = 0; i < n; i++) {
    u->tx_buf[tx_mask(head + i)] = data[i];
  }
  // make sure the data stores are done before the ISR can see the new head
  __COMPILER_BARRIER();
  u->tx_head = tx_mask(head + n);
  tx_next_chunk(u);
}

void ato_uart_write_string(ato_uart_t *u, const char *str) {
  ato_uart_write(u, (const uint8_t *)str, strlen(str));
}

/* HAL UART callbacks ------------------------------------------------------- */

static void notify_from_isr(ato_uart_t *u) {
  if (u->notify_task == NULL) {
    return;
  }
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(u->notify_task, &woken);
  portYIELD_FROM_ISR(woken);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
  ato_uart_t *u = find_instance(huart);
  if (u == NULL) {
    return;
  }
  u->tx_tail = tx_mask(u->tx_tail + u->tx_inflight);
  u->tx_inflight = 0;
  tx_next_chunk(u);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size) {
  ato_uart_t *u = find_instance(huart);
  if (u == NULL) {
    return;
  }
  u->rx_head = rx_mask(size); // size is the position of the next write,
                              // in range [1..buffer_size]
  notify_from_isr(u);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  /* overrun, noise, framing: without a restart, reception stays stuck */
  ato_uart_t *u = find_instance(huart);
  if (u == NULL) {
    return;
  }

  if (huart->gState == HAL_UART_STATE_READY && u->tx_inflight) {
    u->tx_errors++;
    u->tx_tail = tx_mask(u->tx_tail + u->tx_inflight); // drop current inflight
    u->tx_inflight = 0;
    tx_next_chunk(u);
  }

  if (huart->RxState == HAL_UART_STATE_READY) {
    u->rx_errors++;
    // DMA restarts writing at index 0: tell the task to resync its tail
    u->rx_head = 0;
    u->rx_restarted = 1;
    if (HAL_UARTEx_ReceiveToIdle_DMA(huart, u->rx_buf, ATO_UART_RX_SIZE) !=
        HAL_OK) {
      u->rx_restart_failures++;
    }
    notify_from_isr(u);
  }
}
