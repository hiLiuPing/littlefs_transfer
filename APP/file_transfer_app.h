#ifndef __FILE_TRANSFER_APP_H__
#define __FILE_TRANSFER_APP_H__

#include "main.h"
#include "ymodem.h"
#include <stdint.h>
#include <stdbool.h>
#include "lfs_port.h"
#include "uart_dma.h"
#include "ymodem_storage.h"
#include "log.h"
/* Version */

#define TRANSFER_VERSION "1.0.0"


extern SPI_HandleTypeDef hspi2; // 假设你使用的是 hspi2，根据实际修改
extern spi_flash_t flash_32mb; // Flash 设备句柄
extern uart_dma_t uart1_admin; // 唯一实体的定义

typedef struct {
    uint32_t flash_addr;
    uint32_t total_written;
    uint32_t last_percent;
} transfer_context_t;



/* transfer 核心接口 */
void transfer_init(void);



/* Y-modem 接收 */
int transfer_receive_file(void);

/* 外部 UART */

extern ymodem_storage_ops_t lfs_storage_ops; // 外部声明
#endif /* __FILE_TRANSFER_APP_H__ */