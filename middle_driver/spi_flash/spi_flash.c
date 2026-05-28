#include "spi_flash.h"
#include <string.h>
#include "log.h"

#define FLASH_USE_FREERTOS 1

/* ================= OS abstraction ================= */
#if FLASH_USE_FREERTOS

#define FLASH_DELAY(ms)      vTaskDelay(pdMS_TO_TICKS(ms))
#define FLASH_TICK()         xTaskGetTickCount()
#define FLASH_LOCK(f)        xSemaphoreTake((f)->lock, portMAX_DELAY)
#define FLASH_UNLOCK(f)      xSemaphoreGive((f)->lock)

#else

#define FLASH_DELAY(ms)      HAL_Delay(ms)
#define FLASH_TICK()         HAL_GetTick()
#define FLASH_LOCK(f)
#define FLASH_UNLOCK(f)

#endif

/* ================= CS ================= */
static inline void cs_low(spi_flash_t *f)
{
    HAL_GPIO_WritePin(f->cs_port, f->cs_pin, GPIO_PIN_RESET);
}

static inline void cs_high(spi_flash_t *f)
{
    HAL_GPIO_WritePin(f->cs_port, f->cs_pin, GPIO_PIN_SET);
}

/* ================= basic ================= */


static inline void flash_lock(spi_flash_t *f)
{
#if FLASH_USE_FREERTOS
    xSemaphoreTake(f->lock, portMAX_DELAY);
#else
    (void)f;
#endif
}

static inline void flash_unlock(spi_flash_t *f)
{
#if FLASH_USE_FREERTOS
    xSemaphoreGive(f->lock);
#else
    (void)f;
#endif
}



static uint8_t flash_rdsr(spi_flash_t *f)
{
    uint8_t cmd = FLASH_CMD_RDSR;
    uint8_t sr = 0;

    cs_low(f);
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);
    HAL_SPI_Receive(f->hspi, &sr, 1, HAL_MAX_DELAY);
    cs_high(f);

    return sr;
}
int spi_flash_sync(spi_flash_t *f)
{
    flash_lock(f);

    uint32_t start = HAL_GetTick();

    while (flash_rdsr(f) & FLASH_SR_BUSY)
    {
        if (HAL_GetTick() - start > 5000)
        {
            flash_unlock(f);
            return -1;
        }
    }

    flash_unlock(f);
    return 0;
}
static void flash_wren(spi_flash_t *f)
{
    uint8_t cmd = FLASH_CMD_WREN;
    cs_low(f);
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);
    cs_high(f);
}

/* ================= reset ================= */
static void flash_reset(spi_flash_t *f)
{
    uint8_t cmd;

    cs_low(f);
    cmd = FLASH_CMD_RESET_EN;
    HAL_SPI_Transmit(f->hspi, &cmd, 1, 100);
    cs_high(f);

    FLASH_DELAY(1);

    cs_low(f);
    cmd = FLASH_CMD_RESET;
    HAL_SPI_Transmit(f->hspi, &cmd, 1, 100);
    cs_high(f);

    FLASH_DELAY(5);
}

/* ================= wait ================= */
static int flash_wait_ready(spi_flash_t *f, flash_wait_type_t type)
{
    uint32_t timeout;

    switch (type)
    {
        case FLASH_T_PAGE:   timeout = 10; break;
        case FLASH_T_SECTOR: timeout = 500; break;
        case FLASH_T_BLOCK:  timeout = 4000; break;
        case FLASH_T_CHIP:   timeout = 20000; break;
        default: timeout = 1000; break;
    }

    uint32_t start = FLASH_TICK();
    uint32_t retry = 0;

    while (1)
    {
        uint8_t sr = flash_rdsr(f);

        /* ================= 关键修复1：非法值直接判失败 ================= */
        if (sr == 0xFF)
        {
            retry++;
            if (retry > 100)
            {
                log_printf("[FLASH] SR invalid 0xFF\r\n");
                return -2;
            }
            continue;
        }

        /* ================= BUSY清除 ================= */
        if (!(sr & FLASH_SR_BUSY))
        {
            return 0;
        }

        FLASH_DELAY(1);

        /* ================= timeout ================= */
        if (FLASH_TICK() - start > timeout)
        {
            log_printf("[FLASH] wait timeout SR=0x%02X\r\n", sr);

            flash_reset(f);

            return -1;
        }
    }
}

/* ================= addr ================= */
static inline void pack_addr(spi_flash_t *f, uint32_t addr, uint8_t *b)
{
    if (f->addr_len == 3)
    {
        b[0] = addr >> 16;
        b[1] = addr >> 8;
        b[2] = addr;
    }
    else
    {
        b[0] = addr >> 24;
        b[1] = addr >> 16;
        b[2] = addr >> 8;
        b[3] = addr;
    }
}

/* ================= init ================= */
int spi_flash_init(spi_flash_t *f,
                   SPI_HandleTypeDef *hspi,
                   GPIO_TypeDef *cs_port,
                   uint16_t cs_pin)
{
    memset(f, 0, sizeof(*f));

    f->hspi = hspi;
    f->cs_port = cs_port;
    f->cs_pin = cs_pin;

    /* ================= 默认Flash参数（SPI Flash）================= */
    f->page_size   = 256;
    f->sector_size = 4096;
    f->block_size  = 65536;   // ⭐关键补充（默认W25Q）

    f->addr_len = 3;
    f->type = FLASH_TYPE_SPI;

#if FLASH_USE_FREERTOS
    f->lock = xSemaphoreCreateMutex();
#endif

    /* RESET Flash（防止4Byte模式残留） */
    flash_reset(f);

    uint32_t id = spi_flash_read_id(f);
    log_printf("Flash ID: 0x%06X\r\n", id);

    /* ================= Winbond W25Q系列识别 ================= */
    if ((id >> 16) == 0xEF)
    {
        uint8_t dev = id & 0xFF;

        switch (dev)
        {
        case 0x17:  // 8M
            f->flash_size = 8 * 1024 * 1024;
            break;

        case 0x18:  // 16M
            f->flash_size = 16 * 1024 * 1024;
            break;

        case 0x19:  // 32M (W25Q256)
            f->flash_size = 32 * 1024 * 1024;
            f->addr_len = 4;
            break;

        default:
            return -1;
        }
    }
    else
    {
        return -2;
    }

    /* ================= 4Byte模式 ================= */
    if (f->addr_len == 4)
    {
        uint8_t cmd = FLASH_CMD_ENTER_4B;
        cs_low(f);
        HAL_SPI_Transmit(hspi, &cmd, 1, HAL_MAX_DELAY);
        cs_high(f);
    }

    return 0;
}
/* ================= read ================= */
int spi_flash_read(spi_flash_t *f, uint32_t addr, uint8_t *buf, uint32_t len)
{
    FLASH_LOCK(f);

    cs_low(f);

    uint8_t cmd = (len > 512) ? FLASH_CMD_FAST_READ : FLASH_CMD_READ;
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);

    uint8_t a[4];
    pack_addr(f, addr, a);
    HAL_SPI_Transmit(f->hspi, a, f->addr_len, HAL_MAX_DELAY);

    /* ================= FIX: 标准 Fast Read dummy ================= */
    if (cmd == FLASH_CMD_FAST_READ)
    {
        uint8_t dummy = 0x00;   // ✔ 固定1 byte dummy（安全标准写法）
        HAL_SPI_Transmit(f->hspi, &dummy, 1, HAL_MAX_DELAY);
    }

    HAL_SPI_Receive(f->hspi, buf, len, HAL_MAX_DELAY);

    cs_high(f);

    FLASH_UNLOCK(f);
    return 0;
}

/* ================= write ================= */
int spi_flash_write(spi_flash_t *f, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    FLASH_LOCK(f);

    while (len)
    {
        uint32_t off = addr % f->page_size;
        uint32_t chunk = f->page_size - off;

        if (chunk > len) chunk = len;

        flash_wren(f);

        cs_low(f);

        uint8_t cmd = FLASH_CMD_PAGE_PROGRAM;
        HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);

        uint8_t a[4];
        pack_addr(f, addr, a);
        HAL_SPI_Transmit(f->hspi, a, f->addr_len, HAL_MAX_DELAY);

        HAL_SPI_Transmit(f->hspi, (uint8_t*)buf, chunk, HAL_MAX_DELAY);

        cs_high(f);

       


        flash_wait_ready(f, FLASH_T_PAGE);

        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }

    FLASH_UNLOCK(f);
    return 0;
}

/* ================= erase ================= */
/**
 * @brief 擦除指定范围的 Flash 空间
 * @note  针对 LittleFS 优化：移除了自动对齐逻辑，改为严格对齐检查
 * @return 0:成功, -1:参数超限, -2:对齐错误, -3:等待超时
 */
int spi_flash_erase(spi_flash_t *f, uint32_t addr, uint32_t len)
{
    flash_lock(f);

    // 1. 基础合法性检查
    if (len == 0) {
        flash_unlock(f);
        return 0;
    }

    if (addr + len > f->flash_size) {
        log_printf("[FLASH] Erase out of range: 0x%08X + %lu\r\n", addr, len);
        flash_unlock(f);
        return -1;
    }

    /* ================= 核心修改：严格对齐校验 ================= */
    // W25Q256 最小物理擦除单位是 4096 字节 (Sector)
    // 如果不对齐，说明上层逻辑（如 LittleFS 配置）有误，必须报错而不是偷偷对齐
    if ((addr % 4096 != 0) || (len % 4096 != 0)) {
        log_printf("[FLASH] Erase ALIGN ERROR! Addr: 0x%08X, Len: %lu\r\n", addr, len);
        flash_unlock(f);
        return -2; 
    }

    uint32_t end = addr + len;

    while (addr < end) {
        flash_wren(f);

        cs_low(f);

        uint8_t cmd;
        uint32_t erase_size;

        /* ================= 智能擦除策略 ================= */
        // 如果当前地址 64KB 对齐，且剩余长度足够 64KB，优先使用 Block Erase 以提高速度
        if ((addr % 65536 == 0) && (end - addr >= 65536)) {
            cmd = FLASH_CMD_BLOCK_ERASE; // 0xD8
            erase_size = 65536;
        } else {
            cmd = FLASH_CMD_SECTOR_ERASE; // 0x20
            erase_size = 4096;
        }

        HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);

        uint8_t a[4];
        pack_addr(f, addr, a);
        HAL_SPI_Transmit(f->hspi, a, f->addr_len, HAL_MAX_DELAY);

        cs_high(f);

        /* ================= 关键修复：超时时间切换 ================= */
        // Block Erase 的等待时间（通常 2-3s）远长于 Sector Erase（通常 400ms）
        if (flash_wait_ready(f, (cmd == FLASH_CMD_BLOCK_ERASE) ? FLASH_T_BLOCK : FLASH_T_SECTOR) != 0) {
            flash_unlock(f);
            return -3;
        }

        addr += erase_size;
    }

    flash_unlock(f);
    return 0;
}

/* ================= chip erase ================= */
int spi_flash_chip_erase(spi_flash_t *f)
{
    FLASH_LOCK(f);

    flash_wren(f);

    uint8_t cmd = FLASH_CMD_CHIP_ERASE;

    cs_low(f);
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);
    cs_high(f);

    int ret = flash_wait_ready(f, FLASH_T_CHIP);

    FLASH_UNLOCK(f);
    return ret;
}

/* ================= ID ================= */
uint32_t spi_flash_read_id(spi_flash_t *f)
{
    uint8_t cmd = FLASH_CMD_READ_ID;
    uint8_t id[3];

    cs_low(f);
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);
    HAL_SPI_Receive(f->hspi, id, 3, HAL_MAX_DELAY);
    cs_high(f);

    return (id[0]<<16)|(id[1]<<8)|id[2];
}
/* spi_flash.c */

