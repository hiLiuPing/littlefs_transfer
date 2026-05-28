#include "uart_dma.h"
#include <string.h>

void uart_dma_init(uart_dma_t* ctrl, UART_HandleTypeDef* huart, uint8_t* dma_buf, uint32_t dma_size, uint8_t* rb_buf, uint32_t rb_size)
{
    ctrl->huart = huart;
    ctrl->dma_rx_buf = dma_buf;
    ctrl->dma_rx_size = dma_size;
    ctrl->lwrb_buf = rb_buf;
    ctrl->lwrb_size = rb_size;
    ctrl->old_pos = 0;

    /* 初始化环形缓冲区 */
    lwrb_init(&ctrl->uart_rb, ctrl->lwrb_buf, ctrl->lwrb_size);

    /* 开启 DMA 接收 */
    HAL_UART_Receive_DMA(ctrl->huart, ctrl->dma_rx_buf, ctrl->dma_rx_size);

    /* 开启空闲中断 */
    __HAL_UART_ENABLE_IT(ctrl->huart, UART_IT_IDLE);
}

void uart_dma_rx_check(uart_dma_t* ctrl)
{
    uint32_t pos;

    /* 获取当前 DMA 写入位置 */
    pos = ctrl->dma_rx_size - __HAL_DMA_GET_COUNTER(ctrl->huart->hdmarx);

    if (pos != ctrl->old_pos)
    {
        if (pos > ctrl->old_pos)
        {
            lwrb_write(&ctrl->uart_rb, &ctrl->dma_rx_buf[ctrl->old_pos], pos - ctrl->old_pos);
        }
        else
        {
            lwrb_write(&ctrl->uart_rb, &ctrl->dma_rx_buf[ctrl->old_pos], ctrl->dma_rx_size - ctrl->old_pos);
            lwrb_write(&ctrl->uart_rb, &ctrl->dma_rx_buf[0], pos);
        }
        ctrl->old_pos = pos;
    }
}

int uart_dma_read(uart_dma_t* ctrl, uint8_t *data, uint32_t len, uint32_t timeout)
{
    uint32_t tick = HAL_GetTick();
    uint32_t rec = 0;

    while (rec < len)
    {
        rec += lwrb_read(&ctrl->uart_rb, &data[rec], len - rec);

        if ((HAL_GetTick() - tick) > timeout) break;
    }
    return rec;
}

int uart_dma_write(uart_dma_t* ctrl, const uint8_t *data, uint32_t len, uint32_t timeout)
{
    if (HAL_UART_Transmit(ctrl->huart, (uint8_t *)data, len, timeout) == HAL_OK)
    {
        return len;
    }
    return 0;
}
