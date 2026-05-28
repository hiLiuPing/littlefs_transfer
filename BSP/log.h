#ifndef __LOG_H__
#define __LOG_H__

#include "main.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#define LOG_ENABLE   1   // 日志总开关：0 关闭，1 打开
#define LOG_AUTO_NEWLINE    0   // 自动换行
#define LOG_PRINT_PREFIX    0   // 打印文件/行号

/* 日志总开关：0 关闭，1 打开 */
#ifndef LOG_ENABLE
#define LOG_ENABLE    1
#endif

/* 日志默认行为宏（防止未定义报错） */
#ifndef LOG_AUTO_NEWLINE
#define LOG_AUTO_NEWLINE    1   // 自动换行
#endif

#ifndef LOG_PRINT_PREFIX
#define LOG_PRINT_PREFIX    1   // 打印文件/行号
#endif

/* 初始化日志串口 */
void log_init(UART_HandleTypeDef *huart);

/* 内部实现函数 */
void log_printf_impl(const char *file,
                     int line,
                     const char *func,
                     uint8_t auto_newline,
                     uint8_t print_prefix,
                     const char *fmt, ...);

/* 逐字发送单字符，用于 printf 重定向 */
void log_putc(char ch);

/* -------------------
 * 对外接口宏
 * ------------------- */
#if LOG_ENABLE

#define log_printf(fmt, ...) \
    log_printf_impl(__FILE__, __LINE__, __func__, \
                    LOG_AUTO_NEWLINE, \
                    LOG_PRINT_PREFIX, \
                    fmt, ##__VA_ARGS__)

/* 可选：直接用 printf 也输出到 UART */
#define printf(...) do { \
    char _buf[100]; \
    int _len = snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
    for (int _i = 0; _i < _len; _i++) log_putc(_buf[_i]); \
} while(0)

#else

#define log_printf(fmt, ...)
#define printf(...)

#endif

#endif /* __LOG_H__ */