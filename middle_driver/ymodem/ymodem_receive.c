#include "ymodem.h"
#include "ymodem_crc.h"
#include "ymodem_port.h"
#include <stdlib.h>
#include <string.h>

static void parse_header_packet(uint8_t *buf, ymodem_file_info_t *info)
{
    strncpy(info->filename, (char *)buf, sizeof(info->filename) - 1U);
    info->filename[sizeof(info->filename) - 1U] = '\0';
    info->file_size = (uint32_t)atoi((char *)buf + strlen(info->filename) + 1U);
}

static int wait_final_empty_header(uint32_t timeout)
{
    uint8_t c;
    uint8_t buf[1024];
    uint8_t seq[2];
    uint8_t crc_bytes[2];
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout)
    {
        uint32_t elapsed = HAL_GetTick() - start;
        uint32_t remaining = (elapsed < timeout) ? (timeout - elapsed) : 0U;
        uint32_t step = (remaining > 1000U) ? 1000U : remaining;

        if (step == 0U) {
            break;
        }

        if (ymodem_port_read(&c, 1, step) <= 0) {
            continue;
        }

        if (c == YMODEM_SOH) {
            ymodem_file_info_t end_info = {0};

            if (ymodem_port_read(seq, 2, 500) < 2) {
                continue;
            }
            if (ymodem_port_read(buf, 128, 500) < 128) {
                continue;
            }
            if (ymodem_port_read(crc_bytes, 2, 500) < 2) {
                continue;
            }

            if (seq[0] == 0x00U && (uint8_t)(seq[0] + seq[1]) == 0xFFU) {
                parse_header_packet(buf, &end_info);
                if (end_info.filename[0] == '\0') {
                    ymodem_send_response(YMODEM_ACK);
                    return YMODEM_OK;
                }
            }
        } else if (c == YMODEM_CAN || c == 0x03U) {
            return YMODEM_ERROR;
        }
    }

    return YMODEM_TIMEOUT;
}

int ymodem_wait_receive_header(ymodem_file_info_t *info, uint32_t timeout)
{
    uint8_t c;
    uint8_t buf[1024];
    uint8_t seq[2];
    uint8_t crc_bytes[2];
    uint32_t retry = 0;
    const uint32_t max_retries = (timeout + 999U) / 1000U;
    const uint32_t loops = (max_retries == 0U) ? 1U : max_retries;

    while (retry < loops)
    {
        ymodem_send_response(YMODEM_C);

        if (ymodem_port_read(&c, 1, 1000) > 0) {
            if (c == YMODEM_SOH) {
                if (ymodem_port_read(seq, 2, 500) < 2) {
                    goto retry_wait;
                }
                if (ymodem_port_read(buf, 128, 500) < 128) {
                    goto retry_wait;
                }
                if (ymodem_port_read(crc_bytes, 2, 500) < 2) {
                    goto retry_wait;
                }

                if (seq[0] == 0x00U && (uint8_t)(seq[0] + seq[1]) == 0xFFU) {
                    parse_header_packet(buf, info);
                    ymodem_send_response(YMODEM_ACK);
                    HAL_Delay(10);
                    ymodem_send_response(YMODEM_C);
                    return 1;
                }

                ymodem_send_response(YMODEM_NAK);
            } else if (c == YMODEM_CAN || c == 0x03U) {
                return 0;
            }
        }

retry_wait:
        retry++;
    }

    return 0;
}

int ymodem_receive_file_with_callback(ymodem_file_info_t *info, ymodem_packet_cb cb, void *user_data)
{
    uint8_t c;
    uint8_t buf[1024];
    uint8_t seq_bytes[2];
    uint8_t crc_bytes[2];
    uint8_t expected_seq = 1;
    uint32_t errors = 0;

    (void)info;

    while (errors < 10U)
    {
        if (ymodem_port_read(&c, 1, 3000) <= 0) {
            errors++;
            continue;
        }

        if (c == YMODEM_EOT) {
            ymodem_send_response(YMODEM_ACK);
            ymodem_send_response(YMODEM_C);

            if (wait_final_empty_header(3000) == YMODEM_OK) {
                return YMODEM_OK;
            }
            return YMODEM_OK;
        }

        if (c == YMODEM_CAN) {
            return YMODEM_ERROR;
        }

        if (c == YMODEM_SOH || c == YMODEM_STX) {
            uint16_t size = (c == YMODEM_STX) ? 1024U : 128U;
            uint16_t remote_crc;

            if (ymodem_port_read(seq_bytes, 2, 1000) < 2) {
                goto nak_packet;
            }
            if (ymodem_port_read(buf, size, 1000) < size) {
                goto nak_packet;
            }
            if (ymodem_port_read(crc_bytes, 2, 1000) < 2) {
                goto nak_packet;
            }

            if ((uint8_t)(seq_bytes[0] + seq_bytes[1]) != 0xFFU) {
                goto nak_packet;
            }

            if (seq_bytes[0] == (uint8_t)(expected_seq - 1U)) {
                ymodem_send_response(YMODEM_ACK);
                continue;
            }

            if (seq_bytes[0] != expected_seq) {
                ymodem_send_response(YMODEM_CAN);
                return YMODEM_ERROR;
            }

            remote_crc = (uint16_t)((crc_bytes[0] << 8) | crc_bytes[1]);
            if (crc16_update(0, buf, size) != remote_crc) {
                goto nak_packet;
            }

            if (cb != NULL && !cb(buf, size, expected_seq, user_data)) {
                ymodem_send_response(YMODEM_CAN);
                return YMODEM_ERROR;
            }

            ymodem_send_response(YMODEM_ACK);
            expected_seq++;
            errors = 0;
            continue;
        }

nak_packet:
        errors++;
        ymodem_send_response(YMODEM_NAK);
    }

    return YMODEM_TIMEOUT;
}
