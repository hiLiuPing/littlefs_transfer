#ifndef __YMODEM_CRC_H__
#define __YMODEM_CRC_H__
#include <stdint.h>

uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint16_t length);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t length);
uint8_t sum_update(uint8_t checksum, const uint8_t *data, uint16_t length);
uint8_t stm32_calc_crc8(uint8_t *ptr, uint16_t len);

#endif /* __YMODEM_CRC_H__ */