#ifndef __UART_DMA_H__
#define __UART_DMA_H__

#include "main.h"
#include "lwrb.h"
#include <stdint.h>

/* 定义最大支持的缓冲区大小，或者为每个实例单独分配 */
#define UART_DMA_RX_SIZE      1024
#define UART_LWRB_SIZE        4096

typedef struct {
    UART_HandleTypeDef* huart;      // 串口句柄 (huart1, huart3等)
    lwrb_t uart_rb;                // 环形缓冲区结构体
    uint8_t* dma_rx_buf;           // DMA 硬件接收缓冲指针
    uint32_t dma_rx_size;          // DMA 缓冲大小
    uint8_t* lwrb_buf;             // 环形缓冲内存指针
    uint32_t lwrb_size;            // 环形缓冲大小
    volatile uint32_t old_pos;     // 上次处理的位置
} uart_dma_t;

/* 函数接口改为传入指针 */
void uart_dma_init(uart_dma_t* ctrl, UART_HandleTypeDef* huart, uint8_t* dma_buf, uint32_t dma_size, uint8_t* rb_buf, uint32_t rb_size);
void uart_dma_rx_check(uart_dma_t* ctrl);
int uart_dma_read(uart_dma_t* ctrl, uint8_t *data, uint32_t len, uint32_t timeout);
int uart_dma_write(uart_dma_t* ctrl, const uint8_t *data, uint32_t len, uint32_t timeout);

#endif