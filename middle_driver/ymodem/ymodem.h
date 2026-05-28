#ifndef __YMODEM_H__
#define __YMODEM_H__

#include <stdint.h>
#include <stdbool.h>

/* 控制字符 */
#define YMODEM_SOH   0x01
#define YMODEM_STX   0x02
#define YMODEM_EOT   0x04
#define YMODEM_ACK   0x06
#define YMODEM_NAK   0x15
#define YMODEM_CAN   0x18
#define YMODEM_C     0x43

#define YMODEM_OK        0
#define YMODEM_ERROR    -1
#define YMODEM_TIMEOUT  -2

/* 文件信息结构体 (解决 Error #20) */
typedef struct {
    char filename[128];
    uint32_t file_size;
} ymodem_file_info_t;

/* 数据包回调函数原型 */
typedef bool (*ymodem_packet_cb)(const uint8_t *data, uint16_t size, uint32_t packet_num, void *user_data);

/* 接口声明 */
int ymodem_send_response(uint8_t byte);
int ymodem_wait_receive_header(ymodem_file_info_t *info, uint32_t timeout);
int ymodem_receive_file_with_callback(ymodem_file_info_t *info, ymodem_packet_cb cb, void *user_data);

#endif