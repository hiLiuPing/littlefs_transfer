#include "lfs_port.h"
#include "log.h"
#include <string.h>

/* ================= 全局 ================= */
lfs_t g_lfs;
struct lfs_config g_cfg;

/* ================= buffer ================= */
static uint8_t read_buf[LFS_CACHE_SIZE];
static uint8_t prog_buf[LFS_CACHE_SIZE];
static uint8_t lookahead_buf[LFS_LOOKAHEAD_SIZE];

/* =========================================================
 * ⭐ 关键：统一地址映射（修复核心）
 * ========================================================= */
static inline uint32_t lfs_to_flash_addr(lfs_block_t block, lfs_off_t off)
{
    return LFS_FLASH_OFFSET + block * LFS_BLOCK_SIZE + off;
}

/* ================= READ ================= */
int lfs_read_cb(const struct lfs_config *c,
                lfs_block_t block,
                lfs_off_t off,
                void *buffer,
                lfs_size_t size)
{
    spi_flash_t *flash = (spi_flash_t *)c->context;

    uint32_t addr = lfs_to_flash_addr(block, off);

    return spi_flash_read(flash, addr, buffer, size);
}

/* ================= PROGRAM ================= */
int lfs_prog_cb(const struct lfs_config *c,
                lfs_block_t block,
                lfs_off_t off,
                const void *buffer,
                lfs_size_t size)
{
    spi_flash_t *flash = (spi_flash_t *)c->context;

    uint32_t addr = lfs_to_flash_addr(block, off);

    return spi_flash_write(flash, addr, buffer, size);
}

/* ================= ERASE ================= */
int lfs_erase_cb(const struct lfs_config *c,
                lfs_block_t block)
{
    spi_flash_t *flash = (spi_flash_t *)c->context;

    uint32_t addr = LFS_FLASH_OFFSET + block * LFS_BLOCK_SIZE;

    return spi_flash_erase(flash, addr, LFS_BLOCK_SIZE);
}

/* ================= SYNC ================= */
int lfs_sync_cb(const struct lfs_config *c)
{
    spi_flash_t *flash = (spi_flash_t *)c->context;

    return spi_flash_sync(flash);
}

/* ================= INIT ================= */
int lfs_port_init(spi_flash_t *flash)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    g_cfg.context = flash;

    g_cfg.read  = lfs_read_cb;
    g_cfg.prog  = lfs_prog_cb;
    g_cfg.erase = lfs_erase_cb;
    g_cfg.sync  = lfs_sync_cb;

    g_cfg.read_size = LFS_READ_SIZE;
    g_cfg.prog_size = LFS_PROG_SIZE;
    g_cfg.block_size = LFS_BLOCK_SIZE;
    g_cfg.block_count = LFS_BLOCK_COUNT;

    g_cfg.cache_size = LFS_CACHE_SIZE;
    g_cfg.lookahead_size = LFS_LOOKAHEAD_SIZE;
    g_cfg.block_cycles = 500;

    g_cfg.read_buffer = read_buf;
    g_cfg.prog_buffer = prog_buf;
    g_cfg.lookahead_buffer = lookahead_buf;

    int ret = lfs_mount(&g_lfs, &g_cfg);

    if (ret != 0)
    {
        log_printf("[LFS] format...\r\n");
        lfs_format(&g_lfs, &g_cfg);

        ret = lfs_mount(&g_lfs, &g_cfg);
    }

    if (ret == 0)
    {
        log_printf("[LFS] mount OK\r\n");
    }
    else
    {
        log_printf("[LFS] mount FAIL\r\n");
    }

    return ret;
}

/* ================= handle ================= */
lfs_t* lfs_port_get(void)
{
    return &g_lfs;
}