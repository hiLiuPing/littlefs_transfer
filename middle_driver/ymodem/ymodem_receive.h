#ifndef __YMODEM_RECEIVE_H__
#define __YMODEM_RECEIVE_H__

#include <stdint.h>
#include "ymodem_storage.h"

#define YMODEM_FILE_NAME_MAX 128

typedef struct
{
    char name[YMODEM_FILE_NAME_MAX];
    uint32_t size;
    uint32_t received;
} ymodem_file_t;


#endif