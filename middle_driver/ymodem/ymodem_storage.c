#include "ymodem_storage.h"
#include <string.h>

/* ================= 默认空实现（防止崩溃） ================= */

static int storage_open_default(const char *name,
                                uint32_t size,
                                void *user)
{
    (void)name;
    (void)size;
    (void)user;
    return 0;
}

static int storage_write_default(const uint8_t *data,
                                uint32_t len,
                                void *user)
{
    (void)data;
    (void)len;
    (void)user;
    return 0;
}

static int storage_close_default(void *user)
{
    (void)user;
    return 0;
}

static int storage_remove_default(const char *name,
                                 void *user)
{
    (void)name;
    (void)user;
    return 0;
}

/* ================= 默认接口实例 ================= */

static ymodem_storage_ops_t default_ops =
{
    .open  = storage_open_default,
    .write = storage_write_default,
    .close = storage_close_default,
    .remove = storage_remove_default,
};

/* ================= 当前绑定接口 ================= */

static ymodem_storage_ops_t *g_ops = &default_ops;
static void *g_user = NULL;

/* ================= 对外API ================= */

void ymodem_storage_register(ymodem_storage_ops_t *ops,
                            void *user)
{
    if (ops)
        g_ops = ops;

    g_user = user;
}

ymodem_storage_ops_t *ymodem_storage_get(void)
{
    return g_ops;
}

void *ymodem_storage_get_user(void)
{
    return g_user;
}