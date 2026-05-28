#ifndef __SPI_FLASH_H__
#define __SPI_FLASH_H__

#include "main.h"
#include <stdint.h>
#include "spi.h"

/* ================= OS SWITCH ================= */
#define FLASH_USE_FREERTOS   1   // 1=RTOS 0=裸机

#if FLASH_USE_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#endif

/* ================= CMD ================= */
#define FLASH_CMD_READ_ID        0x9F
#define FLASH_CMD_READ           0x03
#define FLASH_CMD_FAST_READ      0x0B
#define FLASH_CMD_PAGE_PROGRAM   0x02
#define FLASH_CMD_SECTOR_ERASE   0x20
#define FLASH_CMD_BLOCK_ERASE    0xD8
#define FLASH_CMD_CHIP_ERASE     0xC7
#define FLASH_CMD_WREN           0x06
#define FLASH_CMD_RDSR           0x05
#define FLASH_CMD_RESET_EN       0x66
#define FLASH_CMD_RESET          0x99
#define FLASH_CMD_ENTER_4B       0xB7

#define FLASH_SR_BUSY            0x01


/* ================= timing ================= */

typedef enum {
    FLASH_T_PAGE = 0,
    FLASH_T_SECTOR,
    FLASH_T_BLOCK,  // 新增
    FLASH_T_CHIP
} flash_wait_type_t;
typedef enum
{
    FLASH_TYPE_SPI = 0,
    FLASH_TYPE_INTERNAL
} flash_type_t;

/* ================= struct ================= */
typedef struct
{
    SPI_HandleTypeDef *hspi;
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;

    uint32_t flash_size;

    /* geometry */
    uint32_t page_size;
    uint32_t sector_size;
    uint32_t block_size;

    uint8_t addr_len;

    /* flash type */
    flash_type_t type;

#if FLASH_USE_FREERTOS
    SemaphoreHandle_t lock;
#endif

} spi_flash_t;

/* ================= API ================= */
int spi_flash_init(spi_flash_t *f,
                   SPI_HandleTypeDef *hspi,
                   GPIO_TypeDef *cs_port,
                   uint16_t cs_pin);

int spi_flash_read(spi_flash_t *f, uint32_t addr, uint8_t *buf, uint32_t len);

int spi_flash_write(spi_flash_t *f, uint32_t addr, const uint8_t *buf, uint32_t len);

int spi_flash_erase(spi_flash_t *f, uint32_t addr, uint32_t len);

int spi_flash_chip_erase(spi_flash_t *f);

uint32_t spi_flash_read_id(spi_flash_t *f);
int spi_flash_sync(spi_flash_t *f);

// int flash_boot_write(uint32_t addr, uint8_t *buf, uint32_t len);
#endif