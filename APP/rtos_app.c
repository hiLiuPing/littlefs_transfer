#include "rtos_app.h"
#include "log.h"
#include "main.h"
#include "ymodem_crc.h"
#include "file_transfer_app.h"
#include "lfs_port.h"
#include "ymodem.h"
#include "spi_flash.h"
#include "uart_dma.h"
#include <string.h>
#include <stdlib.h>

/* ===================== 内部配置宏 ===================== */
#define CLI_BUF_SIZE            128
#define Ymodem_Task_PRIORITY    (tskIDLE_PRIORITY + 3) // 接收任务优先级稍高
#define Ymodem_Task_STACK_SIZE  2048

/* ===================== 外部接口显式声明 ===================== */
extern lfs_t* lfs_port_get(void); 

/* ===================== 全局互斥标志位 ===================== */
// 告诉系统此时是不是正在传文件。如果是，全局 log_printf 必须闭嘴，严禁输出！
volatile bool g_ymodem_transfer_active = false; 

/* ===================== LittleFS 局部指针变量 ===================== */
static lfs_t *p_lfs = NULL; 

/* ===================== 串口 1 DMA 缓冲区定义 ===================== */
uint8_t u1_dma_buf[2048];
uint8_t u1_rb_buf[4096];

/* CLI 动态输入缓冲区与状态变量 */
char cli_buf[CLI_BUF_SIZE];
uint16_t cli_idx = 0;
static bool cli_last_was_cr = false;

#define CLI_DELETE_LIST_MAX 64
static char cli_delete_files[CLI_DELETE_LIST_MAX][LFS_NAME_MAX + 1];
static uint8_t cli_delete_file_count = 0;

typedef enum {
    CLI_MODE_COMMAND = 0,
    CLI_MODE_DELETE_SELECT,
} cli_mode_t;

static cli_mode_t cli_mode = CLI_MODE_COMMAND;

/* ===================== FreeRTOS 任务句柄 ===================== */
TaskHandle_t Ymodem_Task_Handle = NULL;
TaskHandle_t cli_task_handle = NULL;

/* ===================== 前向声明 ===================== */
void vYmodem_Task(void *pvParameters);
void cli_task(void *arg);
bool cli_execute(char *cmd);
static void cli_reset_line(void);
static void cli_prompt(void);
static void cli_handle_byte(uint8_t ch);
static char *cli_trim(char *s);
static void cli_enter_delete_mode(void);
static int cli_collect_deletable_files(void);
static bool cli_handle_delete_selection(char *cmd);

/* ===================== 统一的初始化入口 ===================== */
void myTask()
{
    /* 1. 初始化 Flash 和 LittleFS */
    transfer_init(); 

    /* 2. 动态获取 LittleFS 运行句柄 */
    p_lfs = lfs_port_get(); 
    if (p_lfs == NULL) {
        log_printf("[System] Error: LittleFS instance pointer is NULL!\r\n");
    }
    /* 3. 初始化串口 1 DMA 实例（用于接收一切数据） */
    uart_dma_init(&uart1_admin, &huart1, u1_dma_buf, sizeof(u1_dma_buf), u1_rb_buf, sizeof(u1_rb_buf));

    /* 4. 注册存储接口到 YModem 引擎 */
    ymodem_storage_register(&lfs_storage_ops, NULL);

    /* 5. 创建 YModem 传输任务 */
    xTaskCreate((TaskFunction_t)vYmodem_Task,
                "My_Ymodem_Task",
                Ymodem_Task_STACK_SIZE,
                NULL,
                Ymodem_Task_PRIORITY,
                &Ymodem_Task_Handle);

    /* 6. 创建 CLI 任务 */
    xTaskCreate(cli_task, 
                "CLI_Task", 
                1024, 
                NULL, 
                2, 
                &cli_task_handle);

    log_printf("[CLI] Ready. Type 'help' for commands.\r\n");
}

/* ===================== 改进后的 CLI 任务 ===================== */
void cli_task(void *arg)
{
    cli_task_handle = xTaskGetCurrentTaskHandle();
    cli_reset_line();
    cli_prompt();

    for (;;)
    {
        uint8_t ch;
        if (lwrb_read(&uart1_admin.uart_rb, &ch, 1) > 0) {
            cli_handle_byte(ch);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* ===================== YModem 接收任务 ===================== */
void vYmodem_Task(void *pvParameters)
{
    log_printf("[System] File Transfer Task Ready.\r\n");

    for (;;)
    {
        /* 1. 阻塞等待 CLI 发送 down 指令来唤醒 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* 2. 激活全局传输锁：通知 log_printf 此时闭嘴 */
        g_ymodem_transfer_active = true;

        /* 3. 挂起 CLI 任务，防止抢夺 RingBuffer 字符 */
        if (cli_task_handle != NULL) {
            vTaskSuspend(cli_task_handle);
        }

        /* 4. 彻底冲洗清空缓冲区，憋死一切传输前残留的乱码或 CLI 杂质 */
        lwrb_reset(&uart1_admin.uart_rb);
        vTaskDelay(pdMS_TO_TICKS(50));

        /* 5. 执行 YModem 接收逻辑 */
        int result = transfer_receive_file();

        /* 6. 传输结束，强行延时 100ms 让上位机把最后的 ACK/EOT 接收干净 */
        vTaskDelay(pdMS_TO_TICKS(100));
        lwrb_reset(&uart1_admin.uart_rb);
        
        /* 7. 恢复 CLI 任务，释放全局日志锁 */
        if (cli_task_handle != NULL) {
            vTaskResume(cli_task_handle);
        }
        
        g_ymodem_transfer_active = false; // 解锁日志输出

        // 此时可以安全打印结果了
        if (result == 0) {
            log_printf("[Transfer] Success!\r\n");
        } else {
            log_printf("[Transfer] Failed or Timeout. Error: %d\r\n", result);
        }
        log_printf("[System] Back to CLI Mode.\r\n");  
        cli_prompt();
    }
}

/* ===================== 命令执行器 ===================== */
bool cli_execute(char *cmd)
{
    cmd = cli_trim(cmd);
    if (*cmd == '\0') {
        return true;
    }

    log_printf("[CLI] CMD: %s\r\n", cmd);

    if (cli_mode == CLI_MODE_DELETE_SELECT) {
        return cli_handle_delete_selection(cmd);
    }

    if (p_lfs == NULL) {
        log_printf("[CLI] Error: LittleFS not initialized properly.\r\n");
        return true;
    }

    /* ===== 精准匹配：ls ===== */
    if (strcmp(cmd, "ls") == 0)
    {
        struct lfs_info info;
        lfs_dir_t dir;
        if (lfs_dir_open(p_lfs, &dir, "/") == 0)
        {
            while (lfs_dir_read(p_lfs, &dir, &info) > 0)
            {
                log_printf("%-20s %6d bytes\r\n", info.name, (int)info.size);
            }
            lfs_dir_close(p_lfs, &dir);
        }
        else
        {
            log_printf("[CLI] Failed to open root directory.\r\n");
        }
        return true;
    }

    /* ===== 精准匹配：rm ===== */
    if (strncmp(cmd, "rm ", 3) == 0)
    {
        char *file = cli_trim(cmd + 3);
        if (*file == '\0') {
            log_printf("[CLI] usage: rm <file>\r\n");
            return true;
        }
        int ret = lfs_remove(p_lfs, file);
        if (ret == 0) {
            log_printf("[CLI] delete %s success.\r\n", file);
        } else {
            log_printf("[CLI] delete %s failed, ret=%d\r\n", file, ret);
        }
        return true;
    }

    /* ===== 精准匹配：cat ===== */
    if (strncmp(cmd, "cat ", 4) == 0)
    {
        char *file = cli_trim(cmd + 4);
        if (*file == '\0') {
            log_printf("[CLI] usage: cat <file>\r\n");
            return true;
        }
        lfs_file_t f;
        char buf[64];
        if (lfs_file_open(p_lfs, &f, file, LFS_O_RDONLY) == 0)
        {
            int n;
            while ((n = lfs_file_read(p_lfs, &f, buf, sizeof(buf))) > 0)
            {
                log_printf("%.*s", n, buf); 
            }
            lfs_file_close(p_lfs, &f);
            log_printf("\r\n");
        }
        else
        {
            log_printf("[CLI] Failed to open file: %s\r\n", file);
        }
        return true;
    }

    /* ===== 严格匹配：down ===== */
    if (strcmp(cmd, "down") == 0 || strcmp(cmd, "download") == 0)
    {
        if (Ymodem_Task_Handle != NULL)
        {
            log_printf("[CLI] Switching to YModem mode... Please start YModem send within 20s.\r\n");
            xTaskNotifyGive(Ymodem_Task_Handle);
            return false;
        }
        log_printf("[CLI] YModem task is not ready.\r\n");
        return true;
    }

    /* ===== delete: 进入交互式删除模式 ===== */
    if (strcmp(cmd, "delete") == 0)
    {
        cli_enter_delete_mode();
        return true;
    }

    /* ===== 精准匹配：help ===== */
    if (strcmp(cmd, "help") == 0)
    {
        log_printf("Commands:\r\n");
        log_printf("  ls             - List files\r\n");
        log_printf("  rm <file>      - Delete one file by name\r\n");
        log_printf("  delete         - Interactive delete by index\r\n");
        log_printf("  cat <file>     - View file content\r\n");
        log_printf("  down           - Start YModem file receive\r\n");
        return true;
    }

    /* 安全防线：任何不认识的脏数据、乱码、空敲回车全部拦截在此处 */
    log_printf("[CLI] unknown cmd: '%s'\r\n", cmd);
    return true;
}

static void cli_reset_line(void)
{
    memset(cli_buf, 0, sizeof(cli_buf));
    cli_idx = 0;
    cli_last_was_cr = false;
}

static void cli_prompt(void)
{
    if (cli_mode == CLI_MODE_DELETE_SELECT) {
        log_printf("delete> ");
    } else {
        log_printf("> ");
    }
}

static void cli_handle_byte(uint8_t ch)
{
    if (ch == '\r' || ch == '\n') {
        if (ch == '\n' && cli_last_was_cr) {
            cli_last_was_cr = false;
            return;
        }

        cli_last_was_cr = (ch == '\r');
        log_printf("\r\n");

        bool show_prompt = true;
        if (cli_idx > 0) {
            cli_buf[cli_idx] = '\0';
            show_prompt = cli_execute(cli_buf);
            cli_reset_line();
        }

        if (show_prompt) {
            cli_prompt();
        }
        return;
    }

    cli_last_was_cr = false;

    if (ch == 0x08 || ch == 0x7F) {
        if (cli_idx > 0) {
            cli_idx--;
            cli_buf[cli_idx] = '\0';
        }
        return;
    }

    if (ch >= 0x20 && ch <= 0x7E) {
        if (cli_idx < CLI_BUF_SIZE - 1) {
            cli_buf[cli_idx++] = (char)ch;
        }
    }
}

static char *cli_trim(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return start;
}

static int cli_collect_deletable_files(void)
{
    cli_delete_file_count = 0;

    if (p_lfs == NULL) {
        return -1;
    }

    struct lfs_info info;
    lfs_dir_t dir;

    if (lfs_dir_open(p_lfs, &dir, "/") != 0) {
        return -1;
    }

    while (lfs_dir_read(p_lfs, &dir, &info) > 0)
    {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        if (info.type != LFS_TYPE_REG) {
            continue;
        }

        if (cli_delete_file_count >= CLI_DELETE_LIST_MAX) {
            log_printf("[CLI] delete list truncated at %u files.\r\n", (unsigned)CLI_DELETE_LIST_MAX);
            break;
        }

        strncpy(cli_delete_files[cli_delete_file_count], info.name, LFS_NAME_MAX);
        cli_delete_files[cli_delete_file_count][LFS_NAME_MAX] = '\0';
        log_printf("  [%u] %-20s %6d bytes\r\n",
                   (unsigned)(cli_delete_file_count + 1),
                   info.name,
                   (int)info.size);
        cli_delete_file_count++;
    }

    lfs_dir_close(p_lfs, &dir);
    return (int)cli_delete_file_count;
}

static void cli_enter_delete_mode(void)
{
    int count = cli_collect_deletable_files();
    if (count < 0) {
        log_printf("[CLI] Failed to scan files.\r\n");
        cli_mode = CLI_MODE_COMMAND;
        return;
    }

    if (count == 0) {
        log_printf("[CLI] No files found.\r\n");
        cli_mode = CLI_MODE_COMMAND;
        return;
    }

    cli_mode = CLI_MODE_DELETE_SELECT;
    log_printf("[CLI] Input file index to delete, or 'q' to cancel.\r\n");
}

static bool cli_handle_delete_selection(char *cmd)
{
    cmd = cli_trim(cmd);
    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "cancel") == 0) {
        cli_mode = CLI_MODE_COMMAND;
        log_printf("[CLI] delete canceled.\r\n");
        return true;
    }

    char *end = NULL;
    long idx = strtol(cmd, &end, 10);
    if (end == cmd) {
        log_printf("[CLI] please input a valid index.\r\n");
        return true;
    }

    end = cli_trim(end);
    if (*end != '\0') {
        log_printf("[CLI] please input a valid index.\r\n");
        return true;
    }

    if (idx < 1 || idx > cli_delete_file_count) {
        log_printf("[CLI] index out of range (1-%u).\r\n", (unsigned)cli_delete_file_count);
        return true;
    }

    const char *file = cli_delete_files[idx - 1];
    int ret = lfs_remove(p_lfs, file);
    if (ret == 0) {
        log_printf("[CLI] delete %s success.\r\n", file);
    } else {
        log_printf("[CLI] delete %s failed, ret=%d\r\n", file, ret);
    }

    cli_mode = CLI_MODE_COMMAND;
    return true;
}
