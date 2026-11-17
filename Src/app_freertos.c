/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : app_freertos.c
 * Description        : Code for freertos applications
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "main.h"
#include "task.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS_CLI.h"
#include "usart.h"
#include <stdio.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

u16 echo(const u8 *bytes, u16 size);
typedef void (*tx_bytes_fn_t)(const u8 *data, u16 len);
u32 app_rx_bytes(uint8_t *bytes, size_t size);
void tx_bytes(const uint8_t *data, uint16_t len);

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define APP_RX_SIZE 256
_Static_assert((APP_RX_SIZE & (APP_RX_SIZE - 1)) == 0,
               "APP_RX_SIZE must be a power of 2");

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
static inline u32 app_rx_mask(u32 x) { return x & (APP_RX_SIZE - 1); }
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
static uint8_t tx_buf[512];
static u32 app_tx_head = 0;
static u32 app_tx_tail = 0;
static volatile uint8_t tx_busy = 0;

static u8 app_rx_buf[APP_RX_SIZE];
static volatile u32 app_rx_head = 0;
static u32 app_rx_tail = 0;

/* USER CODE END Variables */
osThreadId defaultTaskHandle;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static void HelloTask(void *argument);

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void const *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);

/* USER CODE BEGIN 4 */
__weak void vApplicationStackOverflowHook(xTaskHandle xTask,
                                          signed char *pcTaskName) {
  /* Run time stack overflow checking is performed if
  configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
  called if a stack overflow is detected. */
}
/* USER CODE END 4 */

/**
 * @brief  FreeRTOS initialization
 * @param  None
 * @retval None
 */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  xTaskCreate(HelloTask, /* task function            */
              "hello",   /* name (debug only)        */
              256,       /* stack size in WORDS (1 KB), printf needs room */
              NULL,      /* argument passed to task  */
              tskIDLE_PRIORITY + 1, NULL); /* handle (not needed here) */
  /* USER CODE END RTOS_THREADS */
}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
 * @brief  Function implementing the defaultTask thread.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const *argument) {
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for (;;) {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
static void HelloTask(void *argument) {
  (void)argument;
  uint32_t count = 0;
  // Init USART1
  printf("hello %lu\r\n", (unsigned long)count++);
  HAL_UARTEx_ReceiveToIdle_DMA(&huart1, app_rx_buf, APP_RX_SIZE);

  for (;;) {
    // application : read buffer
    u32 head_snapshot =
        app_rx_head; // app_rx_head is modified by interrupt, work on copy
    if (head_snapshot != app_rx_tail) {
      if (head_snapshot < app_rx_tail) {
        // process from tail to end of buffer
        app_rx_tail +=
            app_rx_bytes(&app_rx_buf[app_rx_tail], APP_RX_SIZE - app_rx_tail);
      } else {
        app_rx_tail +=
            app_rx_bytes(&app_rx_buf[app_rx_tail], head_snapshot - app_rx_tail);
      }
      app_rx_tail = app_rx_mask(app_rx_tail);
    }
  }
  /* A task must never return; if it must end, call vTaskDelete(NULL). */
}

void tx_bytes(const uint8_t *data, uint16_t len) {
  if (HAL_UART_Transmit_DMA(&huart1, data, len) != HAL_OK) {
  }
}

u16 echo(const u8 *bytes, u16 size) {
  if (tx_busy) {
    return 0;
  }
  u32 ri = 0;
  u32 wi = 0;
  static int last_byte_was_cr = 0;
  int last_byte_cr = last_byte_was_cr;
  while (wi < sizeof(tx_buf) - 1 &&
         ri < size) // always keep space for '\n' in tx_buf
  {
    if (last_byte_cr && bytes[ri] == '\n') {
      last_byte_cr = 0;
    } else {
      tx_buf[wi] = bytes[ri];
      wi++;
      last_byte_cr = bytes[ri] == '\r';
      if (last_byte_cr) {
        tx_buf[wi] = '\n';
        wi++;
      }
    }
    ri++;
  }
  if (wi == 0) {
    last_byte_was_cr = last_byte_cr;
    return ri;
  }
  if (HAL_UART_Transmit_DMA(&huart1, tx_buf, wi) == HAL_OK) {
    tx_busy = 1;
    last_byte_was_cr = last_byte_cr;
    return ri;
  }
  return 0;
}

u32 app_rx_bytes(uint8_t *bytes, size_t size) {
  static char commandBuf[100];
  static char commandBufIdx = 0;
  static char outBuf[512];
  echo(bytes, size);
  for (int i = 0; i < size; i++) {
    if (commandBufIdx + i >= sizeof(commandBuf) - 1) {
      printf("Error : command too long.");
      commandBufIdx = 0;
    } else {
      if (bytes[i] == '\r') {
        commandBuf[commandBufIdx + i] = 0;
        FreeRTOS_CLIProcessCommand(commandBuf, outBuf, sizeof(outBuf));
        printf("%s", outBuf);
        commandBufIdx = 0;
      } else {
        commandBuf[commandBufIdx + i] = bytes[i];
        commandBufIdx++;
      }
    }
  }
  return size;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    tx_busy = 0;
  }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size) {
  if (huart->Instance == USART1) {
    app_rx_head = app_rx_mask(size); // size is the position of the next
                                     // write in range [1..buffer_size]
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  /* overrun, bruit, framing : sans relance, la réception reste bloquée */
  if (huart->Instance == USART1) {
    if (huart->RxState == HAL_UART_STATE_READY)
      HAL_UARTEx_ReceiveToIdle_DMA(&huart1, app_rx_buf, APP_RX_SIZE);
  }
}

/* USER CODE END Application */
