#ifndef __RTOS_APP_H__
#define __RTOS_APP_H__

// #include "stm32l4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "usart.h"

typedef struct {
    uint8_t *data;
    uint8_t len;
} keyMessage_t;

void myTask();



#endif /* __RTOS_APP_H__ */