#include "file_transfer_app.h"
#include <string.h>
#include "spi.h" // 确保能用到 hspi1 等句柄
#include "uart_dma.h"
/* 定义 Flash 设备句柄 */
static lfs_file_t g_ymodem_file;
static bool g_file_is_open = false;

uart_dma_t uart1_admin; // 唯一实体的定义

spi_flash_t flash_32mb = {0}; 

ymodem_file_info_t g_file_info; // 现在 ymodem.h 已定义此类型


static transfer_context_t ctx;


// lfs_file_t file; 
/* ================= 初始化 ================= */
void transfer_init(void)
{
    log_printf("\r\n================================\r\n");
    log_printf("     STM32 Y-MODEM Receiver     \r\n");
    log_printf("         Version %s", TRANSFER_VERSION);
    log_printf("\r\n================================\r\n");

    /* 3. 初始化 Flash 硬件接口 */
    // 请根据你的实际硬件连接修改 GPIOA 和 GPIO_PIN_4

   
       if (spi_flash_init(&flash_32mb, &hspi2, SPI2_CS_GPIO_Port, SPI2_CS_Pin) != 0) {
        log_printf("Flash Hardware Init Failed!\r\n");
        // vTaskDelete(NULL);
        return;
    }
    lfs_port_init(&flash_32mb);
    // lfs_t *lfs = lfs_port_get();

}





// 1. 打开文件回调
static int lfs_ymodem_open(const char *name, uint32_t size, void *user) {
    lfs_t *lfs = lfs_port_get();
    // 使用 LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC：如果存在则清空重新写
    int err = lfs_file_open(lfs, &g_ymodem_file, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err == LFS_ERR_OK) {
        g_file_is_open = true;
        // log_printf("[FS] File %s opened for writing, size: %lu\r\n", name, size);
    }
    return err;
}

// 2. 写入数据回调
static int lfs_ymodem_write(const uint8_t *data, uint32_t len, void *user) {
    lfs_t *lfs = lfs_port_get();
    if (!g_file_is_open) return -1;
    lfs_ssize_t written = lfs_file_write(lfs, &g_ymodem_file, data, len);
    return (written == len) ? 0 : -1;
}

// 3. 关闭文件回调
static int lfs_ymodem_close(void *user) {
    lfs_t *lfs = lfs_port_get();
    if (g_file_is_open) {
        lfs_file_close(lfs, &g_ymodem_file);
        g_file_is_open = false;
        // log_printf("[FS] File closed successfully.\r\n");
    }
    return 0;
}

// 定义存储接口实例
 ymodem_storage_ops_t lfs_storage_ops = {
    .open = lfs_ymodem_open,
    .write = lfs_ymodem_write,
    .close = lfs_ymodem_close,
    .remove = NULL // 可选
};
/* 数据包回调：在此处编写真正的 Flash 写入逻辑 */
static bool packet_callback(const uint8_t *data, uint16_t size, uint32_t packet_num, void *user_data) {
    transfer_context_t *c = (transfer_context_t *)user_data;
    if (size == 0) return true;

    // --- 核心修改：处理真实数据长度，剔除填充字节 ---
    // 计算剩余未写入的字节数
    uint32_t remain = g_file_info.file_size - c->total_written;
    // 如果这一包的大小超过了剩余长度，说明这包含有填充字节，只取有效部分
    uint32_t len_to_write = (size < remain) ? size : remain;

    if (len_to_write > 0) {
        // 调用你下面定义的 LittleFS 写入回调


        if (lfs_ymodem_write(data, len_to_write, NULL) != 0) {
            // log_printf("\r\n[Error] Flash Write Failed at packet %lu", packet_num);
            return false; // 返回 false 会通知 YModem 取消传输
        }


        c->total_written += len_to_write;
    }




    // 更新进度条
uint32_t percent = (c->total_written * 100) / g_file_info.file_size;
    // 修改：每 10% 打印一次，或者只在收完时打印
    if (percent >= c->last_percent + 10 || percent == 100) { 
        c->last_percent = percent;
        // log_printf("Progress: %lu%%", percent);
    }
    
    return true;
}










void test_read_and_print_file(const char* filename) {
    lfs_t *lfs = lfs_port_get();
    lfs_file_t file;
    static uint8_t read_buf[256]; // 读取缓冲区
    int err;

    // log_printf("\r\n--- Reading File: %s ---\r\n", filename);

    // 1. 打开文件
    err = lfs_file_open(lfs, &file, filename, LFS_O_RDONLY);
    if (err < 0) {
        // log_printf("[Error] Failed to open file: %d\r\n", err);
        return;
    }

    // 2. 获取文件大小
    lfs_soff_t size = lfs_file_size(lfs, &file);
    // log_printf("Total size: %ld bytes\r\n", size);

    // 3. 循环读取内容
    lfs_ssize_t read_size;
    uint32_t total_read = 0;
    
    while ((read_size = lfs_file_read(lfs, &file, read_buf, sizeof(read_buf))) > 0) {
        // 打印到串口 (注意：如果是二进制文件，直接 %s 可能会乱码)
        // 这里假设是 txt 文件，我们逐字节打印或限制长度
        for (int i = 0; i < read_size; i++) {
            // 如果是不可见字符，打印点号，防止串口调试助手崩溃
            if (read_buf[i] >= 32 || read_buf[i] == '\n' || read_buf[i] == '\r' || read_buf[i] == '\t') {
                // HAL_UART_Transmit(&huart1, &read_buf[i], 1, 10); // 假设 log 用的是 huart1
                 log_printf("%c", read_buf[i]);
            } else {
                // HAL_UART_Transmit(&huart1, (uint8_t*)".", 1, 10);
               log_printf("."); // 非可见字符显示为点
            }
        }
        total_read += read_size;
    }

    if (read_size < 0) {
        log_printf("\r\n[Error] Read error: %d\r\n", (int)read_size);
    }

    // 4. 关闭文件
    lfs_file_close(lfs, &file);
    log_printf("\r\n--- Read Finished (%lu bytes) ---\r\n", total_read);
}
int transfer_receive_file(void) {
    memset(&g_file_info, 0, sizeof(g_file_info));
    memset(&ctx, 0, sizeof(ctx));

    // log_printf("\r\nWaiting for file via Y-Modem...\r\n");

    // 1. 等待并接收 Packet 0 (文件名和大小)
    if (!ymodem_wait_receive_header(&g_file_info, 10000)) {
        // log_printf("Timeout waiting for file header\r\n");
        return -1;
    }

    // log_printf("File name : %s\r\n", g_file_info.filename);
    // log_printf("File size : %lu bytes\r\n", g_file_info.file_size);

    // 2. 核心修改：根据接收到的文件名，打开 LittleFS 文件
    if (lfs_ymodem_open(g_file_info.filename, g_file_info.file_size, NULL) != 0) {
        // log_printf("Failed to open file in Flash!\r\n");
        ymodem_send_response(YMODEM_CAN); // 告诉上位机取消
        return -1;
    }
    // log_printf("Starting Y-Modem transfer...\r\n");

    
//    uart_dma_flush(&uart3_admin);
    // 3. 按照 YModem 协议，在 Header 处理完后，发送 'C' 启动数据包流
    ymodem_send_response(YMODEM_C);

    // 4. 开始循环接收数据包并触发 packet_callback
    int result = ymodem_receive_file_with_callback(&g_file_info, packet_callback, &ctx);

    // 5. 核心修改：无论成功失败，都必须关闭文件，确保存盘
    lfs_ymodem_close(NULL);

    if(result != YMODEM_OK) {
        // log_printf("Transfer Failed! Code: %d\r\n", result);
        return -2;
    }
    // log_printf("Transfer Success!\r\n");
    return 0;
}