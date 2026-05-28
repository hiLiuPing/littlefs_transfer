#include "ymodem_port.h"
#include "uart_dma.h" // 确保此头文件包含你的 UART DMA 驱动
// extern uart_dma_t uart1_admin; // 假设你使用 uart1_admin 来管理 UART1 的 DMA



// 引用在 file_transfer_app.c 中定义的串口3实例
extern uart_dma_t uart1_admin; 

int ymodem_port_read(uint8_t *data, uint32_t len, uint32_t timeout)
{
    // 读之前可以主动 check 一次 DMA，确保数据实时性
    // uart_dma_rx_check(&uart1_admin);
    return uart_dma_read(&uart1_admin, data, len, timeout);
}


int ymodem_port_write(const uint8_t *data, uint32_t len, uint32_t timeout)
{
    return uart_dma_write(&uart1_admin, data, len, timeout);
}