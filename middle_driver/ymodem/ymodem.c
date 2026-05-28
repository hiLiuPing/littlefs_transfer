#include "ymodem.h"
#include "ymodem_port.h"

/**
 * @brief 发送单个控制字符 (如 ACK, NAK, C)
 */
int ymodem_send_response(uint8_t byte)
{
    return (ymodem_port_write(&byte, 1, 100) == 1) ? 0 : -1;
}

// 辅助快捷函数
int ymodem_send_ack(void) { return ymodem_send_response(YMODEM_ACK); }
int ymodem_send_nak(void) { return ymodem_send_response(YMODEM_NAK); }