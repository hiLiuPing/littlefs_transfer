#ifndef __YMODEM_STORAGE_H__
#define __YMODEM_STORAGE_H__

#include <stdint.h>

typedef struct
{
    int (*open)(const char *name,
                uint32_t size,
                void *user);

    int (*write)(const uint8_t *data,
                 uint32_t len,
                 void *user);

    int (*close)(void *user);

    int (*remove)(const char *name,
                  void *user);

} ymodem_storage_ops_t;

/* ================= API ================= */
void ymodem_storage_register(ymodem_storage_ops_t *ops,
                            void *user);

ymodem_storage_ops_t *ymodem_storage_get(void);

void *ymodem_storage_get_user(void);

#endif