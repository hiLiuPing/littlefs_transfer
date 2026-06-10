#include "spi_flash.h"
#include "log.h"
#include <string.h>

#define FLASH_DELAY(ms)      HAL_Delay(ms)
#define FLASH_TICK()         HAL_GetTick()

static inline void cs_low(spi_flash_t *f)
{
    HAL_GPIO_WritePin(f->cs_port, f->cs_pin, GPIO_PIN_RESET);
}

static inline void cs_high(spi_flash_t *f)
{
    HAL_GPIO_WritePin(f->cs_port, f->cs_pin, GPIO_PIN_SET);
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

static void flash_wren(spi_flash_t *f)
{
    uint8_t cmd = FLASH_CMD_WREN;

    cs_low(f);
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);
    cs_high(f);
}

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

static int flash_wait_ready(spi_flash_t *f, flash_wait_type_t type)
{
    uint32_t timeout;
    uint32_t start;
    uint32_t retry = 0;

    switch (type)
    {
        case FLASH_T_PAGE:   timeout = 10; break;
        case FLASH_T_SECTOR: timeout = 500; break;
        case FLASH_T_BLOCK:  timeout = 4000; break;
        case FLASH_T_CHIP:   timeout = 20000; break;
        default:             timeout = 1000; break;
    }

    start = FLASH_TICK();

    while (1)
    {
        uint8_t sr = flash_rdsr(f);

        if (sr == 0xFF) {
            retry++;
            if (retry > 100U) {
                log_printf("[FLASH] SR invalid 0xFF\r\n");
                return -2;
            }
            continue;
        }

        if ((sr & FLASH_SR_BUSY) == 0U) {
            return 0;
        }

        FLASH_DELAY(1);

        if ((FLASH_TICK() - start) > timeout) {
            log_printf("[FLASH] wait timeout SR=0x%02X\r\n", sr);
            flash_reset(f);
            return -1;
        }
    }
}

static inline void pack_addr(spi_flash_t *f, uint32_t addr, uint8_t *b)
{
    if (f->addr_len == 3U) {
        b[0] = (uint8_t)(addr >> 16);
        b[1] = (uint8_t)(addr >> 8);
        b[2] = (uint8_t)addr;
    } else {
        b[0] = (uint8_t)(addr >> 24);
        b[1] = (uint8_t)(addr >> 16);
        b[2] = (uint8_t)(addr >> 8);
        b[3] = (uint8_t)addr;
    }
}

int spi_flash_sync(spi_flash_t *f)
{
    uint32_t start = HAL_GetTick();

    while ((flash_rdsr(f) & FLASH_SR_BUSY) != 0U)
    {
        if ((HAL_GetTick() - start) > 5000U) {
            return -1;
        }
    }

    return 0;
}

int spi_flash_init(spi_flash_t *f,
                   SPI_HandleTypeDef *hspi,
                   GPIO_TypeDef *cs_port,
                   uint16_t cs_pin)
{
    uint32_t id;

    memset(f, 0, sizeof(*f));

    f->hspi = hspi;
    f->cs_port = cs_port;
    f->cs_pin = cs_pin;
    f->page_size = 256;
    f->sector_size = 4096;
    f->block_size = 65536;
    f->addr_len = 3;
    f->type = FLASH_TYPE_SPI;

    flash_reset(f);

    id = spi_flash_read_id(f);
    log_printf("Flash ID: 0x%06X\r\n", id);

    if ((id >> 16) == 0xEFU) {
        uint8_t dev = (uint8_t)(id & 0xFFU);

        switch (dev)
        {
            case 0x17:
                f->flash_size = 8U * 1024U * 1024U;
                break;
            case 0x18:
                f->flash_size = 16U * 1024U * 1024U;
                break;
            case 0x19:
                f->flash_size = 32U * 1024U * 1024U;
                f->addr_len = 4;
                break;
            default:
                return -1;
        }
    } else {
        return -2;
    }

    if (f->addr_len == 4U) {
        uint8_t cmd = FLASH_CMD_ENTER_4B;
        cs_low(f);
        HAL_SPI_Transmit(hspi, &cmd, 1, HAL_MAX_DELAY);
        cs_high(f);
    }

    return 0;
}

int spi_flash_read(spi_flash_t *f, uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t cmd = (len > 512U) ? FLASH_CMD_FAST_READ : FLASH_CMD_READ;
    uint8_t a[4];

    cs_low(f);
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);

    pack_addr(f, addr, a);
    HAL_SPI_Transmit(f->hspi, a, f->addr_len, HAL_MAX_DELAY);

    if (cmd == FLASH_CMD_FAST_READ) {
        uint8_t dummy = 0x00;
        HAL_SPI_Transmit(f->hspi, &dummy, 1, HAL_MAX_DELAY);
    }

    HAL_SPI_Receive(f->hspi, buf, len, HAL_MAX_DELAY);
    cs_high(f);

    return 0;
}

int spi_flash_write(spi_flash_t *f, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len > 0U)
    {
        uint32_t off = addr % f->page_size;
        uint32_t chunk = f->page_size - off;
        uint8_t cmd = FLASH_CMD_PAGE_PROGRAM;
        uint8_t a[4];

        if (chunk > len) {
            chunk = len;
        }

        flash_wren(f);
        cs_low(f);
        HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);

        pack_addr(f, addr, a);
        HAL_SPI_Transmit(f->hspi, a, f->addr_len, HAL_MAX_DELAY);
        HAL_SPI_Transmit(f->hspi, (uint8_t *)buf, chunk, HAL_MAX_DELAY);
        cs_high(f);

        if (flash_wait_ready(f, FLASH_T_PAGE) != 0) {
            return -1;
        }

        addr += chunk;
        buf += chunk;
        len -= chunk;
    }

    return 0;
}

int spi_flash_erase(spi_flash_t *f, uint32_t addr, uint32_t len)
{
    uint32_t end;

    if (len == 0U) {
        return 0;
    }

    if ((addr + len) > f->flash_size) {
        log_printf("[FLASH] Erase out of range: 0x%08X + %lu\r\n", addr, len);
        return -1;
    }

    if ((addr % 4096U) != 0U || (len % 4096U) != 0U) {
        log_printf("[FLASH] Erase ALIGN ERROR! Addr: 0x%08X, Len: %lu\r\n", addr, len);
        return -2;
    }

    end = addr + len;

    while (addr < end)
    {
        uint8_t cmd;
        uint32_t erase_size;
        uint8_t a[4];

        flash_wren(f);
        cs_low(f);

        if ((addr % 65536U) == 0U && (end - addr) >= 65536U) {
            cmd = FLASH_CMD_BLOCK_ERASE;
            erase_size = 65536U;
        } else {
            cmd = FLASH_CMD_SECTOR_ERASE;
            erase_size = 4096U;
        }

        HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);
        pack_addr(f, addr, a);
        HAL_SPI_Transmit(f->hspi, a, f->addr_len, HAL_MAX_DELAY);
        cs_high(f);

        if (flash_wait_ready(f, (cmd == FLASH_CMD_BLOCK_ERASE) ? FLASH_T_BLOCK : FLASH_T_SECTOR) != 0) {
            return -3;
        }

        addr += erase_size;
    }

    return 0;
}

int spi_flash_chip_erase(spi_flash_t *f)
{
    uint8_t cmd = FLASH_CMD_CHIP_ERASE;

    flash_wren(f);
    cs_low(f);
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);
    cs_high(f);

    return flash_wait_ready(f, FLASH_T_CHIP);
}

uint32_t spi_flash_read_id(spi_flash_t *f)
{
    uint8_t cmd = FLASH_CMD_READ_ID;
    uint8_t id[3];

    cs_low(f);
    HAL_SPI_Transmit(f->hspi, &cmd, 1, HAL_MAX_DELAY);
    HAL_SPI_Receive(f->hspi, id, 3, HAL_MAX_DELAY);
    cs_high(f);

    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}
