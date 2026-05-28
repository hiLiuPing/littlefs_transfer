#include "log.h"

static UART_HandleTypeDef *log_uart = NULL;

/* 初始化串口 */
void log_init(UART_HandleTypeDef *huart)
{
    log_uart = huart;
}

/* 提取文件名 */
static const char *log_get_filename(const char *path)
{
    const char *p = path;
    while (*path)
    {
        if (*path == '/' || *path == '\\') p = path + 1;
        path++;
    }
    return p;
}

/* 阻塞发送单字符 */
void log_putc(char ch)
{
#if LOG_ENABLE
    if (!log_uart) return;

    USART_TypeDef *uart = log_uart->Instance;
    // STM32L4 正确寄存器：ISR / TDR / USART_ISR_TXE
    while (!(uart->ISR & USART_ISR_TXE));
    uart->TDR = (uint8_t)ch;
#endif
}

/* 日志核心实现 */
void log_printf_impl(const char *file,
                     int line,
                     const char *func,
                     uint8_t auto_newline,
                     uint8_t print_prefix,
                     const char *fmt, ...)
{
#if LOG_ENABLE
    char buf[512];
    uint16_t len = 0;
    va_list args;

#if !LOG_PRINT_PREFIX
    (void)file;
    (void)line;
    (void)func;
    (void)print_prefix;
#endif

#if !LOG_AUTO_NEWLINE
    (void)auto_newline;
#endif

    /* 前缀 */
#if LOG_PRINT_PREFIX
    if (print_prefix)
    {
        len += snprintf(buf + len, sizeof(buf) - len,
                        "[%s:%d %s] ",
                        log_get_filename(file), line, func);
    }
#endif

    /* 格式化字符串 */
    va_start(args, fmt);
    len += vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
    va_end(args);

    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;

    /* 自动换行 */
#if LOG_AUTO_NEWLINE
    if (auto_newline && len < sizeof(buf) - 2)
    {
        buf[len++] = '\r';
        buf[len++] = '\n';
    }
#endif

    /* 发送 */
    for (uint16_t i = 0; i < len; i++)
        log_putc(buf[i]);
#endif
}

/* 重定向 printf 输出到 UART */
int fputc(int ch, FILE *f)
{
    log_putc((char)ch);
    return ch;
}