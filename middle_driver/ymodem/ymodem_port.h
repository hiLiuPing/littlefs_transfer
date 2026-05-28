#ifndef __YMODEM_PORT_H__
#define __YMODEM_PORT_H__

#include <stdint.h>

/**
 * @brief 从串口读取数据
 * @param timeout 单位为 ms
 * @return 实际读取的字节数
 */
int ymodem_port_read(uint8_t *data, uint32_t len, uint32_t timeout);

/**
 * @brief 向串口发送数据
 */
int ymodem_port_write(const uint8_t *data, uint32_t len, uint32_t timeout);

#endif