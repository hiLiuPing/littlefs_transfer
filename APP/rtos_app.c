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

/* ===================== FreeRTOS 任务句柄 ===================== */
TaskHandle_t Ymodem_Task_Handle = NULL;
TaskHandle_t cli_task_handle = NULL;

/* ===================== 前向声明 ===================== */
void vYmodem_Task(void *pvParameters);
void cli_task(void *arg);
void cli_execute(char *cmd);

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
/* ---------------- 新增：启动时自动扫描并打印存储的文件内容 ---------------- */
    log_printf("\r\n--- [Storage Scan Start] ---\r\n");
    
    struct lfs_info info;
    lfs_dir_t dir;
    
    // 打开根目录
    if (lfs_dir_open(p_lfs, &dir, "/") == 0)
    {
        int file_count = 0;
        
        // 循环读取目录下的每一个条目
        while (lfs_dir_read(p_lfs, &dir, &info) > 0)
        {
            // 过滤掉当前目录 "." 和 上级目录 ".."
            if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
                continue;
            }
            
            file_count++;
            
            // 判断是文件还是目录
            if (info.type == LFS_TYPE_REG) 
            {
                log_printf("📄 File: %-20s | Size: %6d bytes\r\n", info.name, (int)info.size);
                
                // 尝试打开该文件并读取前一段内容
                lfs_file_t file_preview;
                if (lfs_file_open(p_lfs, &file_preview, info.name, LFS_O_RDONLY) == 0)
                {
                    char preview_buf[64]; // 在栈上开辟 64 字节缓冲区，安全稳妥
                    
                    // 读取前段内容（最多64字节）
                    int read_bytes = lfs_file_read(p_lfs, &file_preview, preview_buf, sizeof(preview_buf));
                    if (read_bytes > 0)
                    {
                        // 使用 %.*s 语法指定只打印读取到的 read_bytes 长度，绝对不会越界或打印出乱码
                        log_printf("   └── Preview: %.*s", read_bytes, preview_buf);
                        
                        // 如果文件没有以换行结尾，补一个换行让日志整洁
                        if (preview_buf[read_bytes - 1] != '\n') {
                            log_printf("\r\n");
                        }
                    }
                    else if (read_bytes == 0)
                    {
                        log_printf("   └── Preview: [Empty File]\r\n");
                    }
                    else
                    {
                        log_printf("   └── Preview: [Read Error: %d]\r\n", read_bytes);
                    }
                    
                    // 读取完一定要记得关闭文件
                    lfs_file_close(p_lfs, &file_preview);
                }
                else
                {
                    log_printf("   └── [Error] Could not open file for preview.\r\n");
                }
            }
            else if (info.type == LFS_TYPE_DIR)
            {
                log_printf("📁 Folder: %-18s |\r\n", info.name);
            }
        }
        
        if (file_count == 0) {
            log_printf("[Storage] LittleFS is empty. No files found.\r\n");
        }
        
        // 关闭目录句柄
        lfs_dir_close(p_lfs, &dir);
    }
    else
    {
        log_printf("[Storage] Error: Failed to open root directory '/'!\r\n");
    }
    log_printf("--- [Storage Scan End] ---\r\n\r\n");
    /* ----------------------------------------------------------------- */
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
}

/* ===================== 改进后的 CLI 任务 ===================== */
void cli_task(void *arg)
{
    cli_task_handle = xTaskGetCurrentTaskHandle();
    log_printf("[CLI] UART1 CLI Ready. Type 'help' for commands.\r\n");
    
    char ch;
    for (;;)
    {
        /* 从统一的 uart1 环形缓冲区中逐字节读取 */
        if (lwrb_read(&uart1_admin.uart_rb, &ch, 1) > 0)
        {
            if (ch == '\r' || ch == '\n')
            {
                // 修复误触核心：只要遇到换行，不管前面有没有数据，都必须对缓冲区切断处理
                cli_buf[cli_idx] = 0; 
                
                if (cli_idx > 0)
                {
                    log_printf("[CLI] CMD: %s\r\n", cli_buf);
                    cli_execute(cli_buf);
                    cli_idx = 0; // 执行完立刻清空
                }
            }
            else
            {
                // 正常字符压入缓冲区
                if (cli_idx < CLI_BUF_SIZE - 1)
                {
                    cli_buf[cli_idx++] = ch;
                }
                else
                {
                    // 缓冲区爆了，强制复位，防止内存越界破坏系统
                    cli_idx = 0;
                    memset(cli_buf, 0, sizeof(cli_buf));
                }
            }
        }
        else
        {
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
        /* 1. 阻塞等待 CLI 发送 download 指令来唤醒 */
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
    }
}

/* ===================== 命令执行器 ===================== */
void cli_execute(char *cmd)
{
    if (p_lfs == NULL) {
        log_printf("[CLI] Error: LittleFS not initialized properly.\r\n");
        return;
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
        return;
    }

    /* ===== 精准匹配：rm ===== */
    if (strncmp(cmd, "rm ", 3) == 0)
    {
        char *file = cmd + 3;
        int ret = lfs_remove(p_lfs, file);
        log_printf("[CLI] rm %s => %d\r\n", file, ret);
        return;
    }

    /* ===== 精准匹配：cat ===== */
    if (strncmp(cmd, "cat ", 4) == 0)
    {
        char *file = cmd + 4;
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
        return;
    }

    /* ===== 严格匹配：download ===== */
    if (strcmp(cmd, "download") == 0)
    {
        if (Ymodem_Task_Handle != NULL)
        {
            log_printf("[CLI] Switching to YModem mode... Please start YModem send within 10s.\r\n");
            xTaskNotifyGive(Ymodem_Task_Handle);
        }
        return;
    }

    /* ===== 精准匹配：help ===== */
    if (strcmp(cmd, "help") == 0)
    {
        log_printf("Commands:\r\n");
        log_printf("  ls             - List files\r\n");
        log_printf("  rm <file>      - Delete file\r\n");
        log_printf("  cat <file>     - View file content\r\n");
        log_printf("  download       - Start YModem file receive\r\n");
        return;
    }

    /* 安全防线：任何不认识的脏数据、乱码、空敲回车全部拦截在此处 */
    log_printf("[CLI] unknown cmd: '%s'\r\n", cmd);
}