#include "ymodem.h"
#include "ymodem_port.h"
#include "ymodem_crc.h"
#include <string.h>
#include <stdlib.h>
#include "log.h"
/* 解析 Packet 0 (文件名和大小) */
static void parse_header_packet(uint8_t *buf, ymodem_file_info_t *info) {
    strncpy(info->filename, (char *)buf, 128);
    char *p = (char *)buf + strlen(info->filename) + 1;
    info->file_size = (uint32_t)atoi(p);
}



int ymodem_wait_receive_header(ymodem_file_info_t *info, uint32_t timeout) {
    uint8_t c, buf[1024], seq[2], crc_bytes[2];
    uint32_t retry = 0;
    const uint32_t max_retries = 20; 


    while (retry < max_retries) {
        // 1. 发送 'C' 启动传输
        ymodem_send_response(YMODEM_C);

        // 2. 尝试读取起始字节 (等待 1 秒)
        if (ymodem_port_read(&c, 1, 1000) > 0) {
            if (c == YMODEM_SOH) {
                /* --- 收到 SOH，开始解析包体 --- */
                // 注意：这里需要更严格的超时判断
                if (ymodem_port_read(seq, 2, 500) < 2) goto __retry; 
                if (ymodem_port_read(buf, 128, 500) < 128) goto __retry;
                if (ymodem_port_read(crc_bytes, 2, 500) < 2) goto __retry;

                // 校验序号 (Packet 0 的序号必须是 00 FF)
                if (seq[0] == 0x00 && (seq[0] + seq[1] == 0xFF)) {
                    // 解析文件名和大小
                    parse_header_packet(buf, info);
                    
                    // 只有成功解析才回复 ACK + C
                    ymodem_send_response(YMODEM_ACK);
                    // vTaskDelay(pdMS_TO_TICKS(100)); // 给上位机一点处理时间
                    HAL_Delay(10); // 给上位机一点处理时间，确保 ACK 发送完成
                    ymodem_send_response(YMODEM_C); 
                    return 1; // 成功退出
                } else {
                    // 序号不对，可能是乱码，发 NAK 触发重传
                    ymodem_send_response(YMODEM_NAK);
                }
            } else if (c == 0x18 || c == 0x03) { 
                return 0; // 用户取消
            }
            // 如果收到其他乱码字符，不做处理，继续循环
        }

__retry:
        retry++;
        // 只有真的在等待时才打印，避免刷屏
        // if (retry % 2 == 0) log_printf("Waiting for PC... %d\r\n", retry);
    }

    return 0; // 彻底超时退出
}






/* 循环接收数据包 (解决 Warning #223-D) */
int ymodem_receive_file_with_callback(ymodem_file_info_t *info, ymodem_packet_cb cb, void *user_data) {
    uint8_t c, buf[1024], seq_bytes[2], crc_bytes[2];
    uint8_t expected_seq = 1; // 数据从 1 号包开始
    uint32_t errors = 0;

    while (errors < 10) {
        if (ymodem_port_read(&c, 1, 3000) <= 0) {
            errors++;
            continue;
        }

        if (c == YMODEM_EOT) {
            // 收到结束标志
            ymodem_send_response(YMODEM_ACK);
            return YMODEM_OK;
        }

        if (c == YMODEM_CAN) {
            return YMODEM_ERROR;
        }

        if (c == YMODEM_SOH || c == YMODEM_STX) {
            uint16_t size = (c == YMODEM_STX) ? 1024 : 128;
            
            // 建议连续读取，减少读取次数
            if (ymodem_port_read(seq_bytes, 2, 1000) < 2) goto __nak;
            if (ymodem_port_read(buf, size, 1000) < size) goto __nak;
            if (ymodem_port_read(crc_bytes, 2, 1000) < 2) goto __nak;

            // 1. 验证序号完整性 (序号 + 反码 = 0xFF)
            if ((uint8_t)(seq_bytes[0] + seq_bytes[1]) != 0xFF) {
                goto __nak;
            }

            // 2. 处理重复包：如果是上一包的序号，说明之前的 ACK 丢了
            if (seq_bytes[0] == (uint8_t)(expected_seq - 1)) {
                ymodem_send_response(YMODEM_ACK);
                continue; 
            }

            // 3. 验证当前包序号
            if (seq_bytes[0] != expected_seq) {
                ymodem_send_response(YMODEM_CAN); // 序号严重错乱，取消
                return YMODEM_ERROR;
            }

            // 4. CRC16 校验
            uint16_t remote_crc = (crc_bytes[0] << 8) | crc_bytes[1];
            if (crc16_update(0, buf, size) != remote_crc) {
                goto __nak;
            }

            // 5. 写入回调
            if (cb && !cb(buf, size, expected_seq, user_data)) {
                ymodem_send_response(YMODEM_CAN);
                return YMODEM_ERROR;
            }

            // 成功收包
            ymodem_send_response(YMODEM_ACK);
            expected_seq++;
            errors = 0; // 重置错误计数
            continue;

        __nak:
            errors++;
            ymodem_send_response(YMODEM_NAK);
        }
    }
    return YMODEM_TIMEOUT;
}