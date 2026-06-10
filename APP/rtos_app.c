#include "rtos_app.h"
#include "log.h"
#include "file_transfer_app.h"
#include "lfs_port.h"
#include "spi_flash.h"
#include "uart_dma.h"
#include "ymodem.h"
#include "ymodem_storage.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CLI_BUF_SIZE         128
#define CLI_DELETE_LIST_MAX  64

extern lfs_t* lfs_port_get(void);

volatile bool g_ymodem_transfer_active = false;

static lfs_t *p_lfs = NULL;

static uint8_t u1_dma_buf[2048];
static uint8_t u1_rb_buf[4096];
static char cli_buf[CLI_BUF_SIZE];
static uint16_t cli_idx = 0;
static bool cli_last_was_cr = false;

static char cli_delete_files[CLI_DELETE_LIST_MAX][LFS_NAME_MAX + 1];
static uint8_t cli_delete_file_count = 0;
static char cli_delete_target[LFS_NAME_MAX + 1];

typedef enum {
    APP_MODE_CLI = 0,
    APP_MODE_YMODEM,
} app_mode_t;

typedef enum {
    CLI_MODE_COMMAND = 0,
    CLI_MODE_DELETE_SELECT,
    CLI_MODE_DELETE_CONFIRM,
} cli_mode_t;

typedef enum {
    DELETE_CONFIRM_NONE = 0,
    DELETE_CONFIRM_ONE,
    DELETE_CONFIRM_ALL,
} delete_confirm_action_t;

static app_mode_t app_mode = APP_MODE_CLI;
static cli_mode_t cli_mode = CLI_MODE_COMMAND;
static delete_confirm_action_t cli_delete_confirm_action = DELETE_CONFIRM_NONE;

static void cli_reset_line(void);
static void cli_prompt(void);
static void cli_handle_byte(uint8_t ch);
static bool cli_execute(char *cmd);
static char *cli_trim(char *s);
static void cli_print_size_kb(uint32_t size_bytes);
static void cli_print_fs_kb(uint32_t size_bytes);
static void cli_print_type(uint8_t type);
static void cli_clear_delete_state(void);
static void cli_print_delete_prompt(void);
static void cli_begin_delete_one(const char *path);
static void cli_begin_delete_all(void);
static int cli_collect_deletable_files(void);
static void cli_enter_delete_mode(void);
static bool cli_handle_delete_selection(char *cmd);
static bool cli_handle_delete_confirm(char *cmd);
static void app_run_transfer(void);

void app_init(void)
{
    transfer_init();

    p_lfs = lfs_port_get();
    if (p_lfs == NULL) {
        log_printf("[System] Error: LittleFS instance pointer is NULL!\r\n");
    }

    uart_dma_init(&uart1_admin, &huart1, u1_dma_buf, sizeof(u1_dma_buf), u1_rb_buf, sizeof(u1_rb_buf));
    ymodem_storage_register(&lfs_storage_ops, NULL);

    cli_reset_line();
    log_printf("[CLI] Ready. Type 'help' for commands.\r\n");
    cli_prompt();
}

void app_poll(void)
{
    if (app_mode == APP_MODE_YMODEM) {
        app_run_transfer();
        return;
    }

    while (1) {
        uint8_t ch;
        if (lwrb_read(&uart1_admin.uart_rb, &ch, 1) == 0) {
            break;
        }
        cli_handle_byte(ch);
        if (app_mode == APP_MODE_YMODEM) {
            break;
        }
    }
}

static void app_run_transfer(void)
{
    int result;

    g_ymodem_transfer_active = true;
    uart_dma_clear(&uart1_admin);
    HAL_Delay(20);

    result = transfer_receive_file();

    HAL_Delay(100);
    uart_dma_clear(&uart1_admin);
    g_ymodem_transfer_active = false;
    app_mode = APP_MODE_CLI;

    if (result == 0) {
        log_printf("[Transfer] Success.\r\n");
    } else {
        log_printf("[Transfer] Failed or timeout. Error: %d\r\n", result);
    }

    log_printf("[System] Back to CLI mode.\r\n");
    cli_reset_line();
    cli_prompt();
}

static bool cli_execute(char *cmd)
{
    cmd = cli_trim(cmd);
    if (*cmd == '\0') {
        return true;
    }

    log_printf("[CLI] CMD: %s\r\n", cmd);

    if (cli_mode == CLI_MODE_DELETE_SELECT) {
        return cli_handle_delete_selection(cmd);
    }

    if (cli_mode == CLI_MODE_DELETE_CONFIRM) {
        return cli_handle_delete_confirm(cmd);
    }

    if (p_lfs == NULL) {
        log_printf("[CLI] Error: LittleFS not initialized properly.\r\n");
        return true;
    }

    if (strcmp(cmd, "ls") == 0) {
        struct lfs_info info;
        lfs_dir_t dir;

        if (lfs_dir_open(p_lfs, &dir, "/") == 0) {
            while (lfs_dir_read(p_lfs, &dir, &info) > 0) {
                log_printf("%-20s ", info.name);
                cli_print_size_kb((uint32_t)info.size);
                log_printf("\r\n");
            }
            lfs_dir_close(p_lfs, &dir);
        } else {
            log_printf("[CLI] Failed to open root directory.\r\n");
        }
        return true;
    }

    if (strncmp(cmd, "rm ", 3) == 0) {
        char *file = cli_trim(cmd + 3);
        if (*file == '\0') {
            log_printf("[CLI] usage: rm <file>\r\n");
            return true;
        }
        cli_begin_delete_one(file);
        return true;
    }

    if (strncmp(cmd, "cat ", 4) == 0) {
        char *file = cli_trim(cmd + 4);
        lfs_file_t f;
        char buf[64];

        if (*file == '\0') {
            log_printf("[CLI] usage: cat <file>\r\n");
            return true;
        }

        if (lfs_file_open(p_lfs, &f, file, LFS_O_RDONLY) == 0) {
            int n;
            while ((n = lfs_file_read(p_lfs, &f, buf, sizeof(buf))) > 0) {
                log_printf("%.*s", n, buf);
            }
            lfs_file_close(p_lfs, &f);
            log_printf("\r\n");
        } else {
            log_printf("[CLI] Failed to open file: %s\r\n", file);
        }
        return true;
    }

    if (strncmp(cmd, "stat ", 5) == 0) {
        char *path = cli_trim(cmd + 5);
        struct lfs_info info;
        int ret;

        if (*path == '\0') {
            log_printf("[CLI] usage: stat <file>\r\n");
            return true;
        }

        ret = lfs_stat(p_lfs, path, &info);
        if (ret != 0) {
            log_printf("[CLI] stat failed: %s, ret=%d\r\n", path, ret);
            return true;
        }

        log_printf("[CLI] name: %s\r\n", info.name);
        log_printf("[CLI] type: ");
        cli_print_type(info.type);
        log_printf("\r\n");
        log_printf("[CLI] size: ");
        cli_print_size_kb((uint32_t)info.size);
        log_printf("\r\n");
        return true;
    }

    if (strcmp(cmd, "free") == 0) {
        lfs_ssize_t used_blocks = lfs_fs_size(p_lfs);
        uint32_t total_blocks;
        uint32_t used;
        uint32_t free_blocks;

        if (used_blocks < 0) {
            log_printf("[CLI] free failed, ret=%ld\r\n", (long)used_blocks);
            return true;
        }

        total_blocks = (uint32_t)LFS_BLOCK_COUNT;
        used = (uint32_t)used_blocks;
        free_blocks = (used < total_blocks) ? (total_blocks - used) : 0U;

        log_printf("[CLI] filesystem:\r\n");
        log_printf("  total: ");
        cli_print_fs_kb(total_blocks * LFS_BLOCK_SIZE);
        log_printf("\r\n");
        log_printf("  used : ");
        cli_print_fs_kb(used * LFS_BLOCK_SIZE);
        log_printf("\r\n");
        log_printf("  free : ");
        cli_print_fs_kb(free_blocks * LFS_BLOCK_SIZE);
        log_printf("\r\n");
        return true;
    }

    if (strcmp(cmd, "down") == 0 || strcmp(cmd, "download") == 0) {
        log_printf("[CLI] Switching to YModem mode. Start send within 20s.\r\n");
        app_mode = APP_MODE_YMODEM;
        return false;
    }

    if (strcmp(cmd, "delete") == 0) {
        cli_enter_delete_mode();
        return true;
    }

    if (strcmp(cmd, "deleteall") == 0) {
        cli_begin_delete_all();
        return true;
    }

    if (strcmp(cmd, "help") == 0) {
        log_printf("Commands:\r\n");
        log_printf("  ls             - List files\r\n");
        log_printf("  rm <file>      - Delete one file with confirm\r\n");
        log_printf("  delete         - Interactive delete by index\r\n");
        log_printf("  deleteall      - Delete all files with confirm\r\n");
        log_printf("  cat <file>     - View file content\r\n");
        log_printf("  stat <file>    - Show file info\r\n");
        log_printf("  free           - Show filesystem usage\r\n");
        log_printf("  down           - Start YModem file receive\r\n");
        return true;
    }

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
    } else if (cli_mode == CLI_MODE_DELETE_CONFIRM) {
        log_printf("confirm> ");
    } else {
        log_printf("> ");
    }
}

static void cli_handle_byte(uint8_t ch)
{
    bool show_prompt = true;

    if (ch == '\r' || ch == '\n') {
        if (ch == '\n' && cli_last_was_cr) {
            cli_last_was_cr = false;
            return;
        }

        cli_last_was_cr = (ch == '\r');
        log_printf("\r\n");

        if (cli_idx > 0) {
            cli_buf[cli_idx] = '\0';
            show_prompt = cli_execute(cli_buf);
            cli_reset_line();
        }

        if (show_prompt && app_mode == APP_MODE_CLI) {
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
        if (cli_idx < (CLI_BUF_SIZE - 1U)) {
            cli_buf[cli_idx++] = (char)ch;
        }
    }
}

static char *cli_trim(char *s)
{
    char *start = s;
    char *end;

    while (*start == ' ' || *start == '\t') {
        start++;
    }

    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return start;
}

static void cli_print_size_kb(uint32_t size_bytes)
{
    uint32_t kb = size_bytes / 1024U;
    uint32_t tenth = (size_bytes % 1024U) * 10U / 1024U;
    log_printf("%lu.%lu KB", (unsigned long)kb, (unsigned long)tenth);
}

static void cli_print_fs_kb(uint32_t size_bytes)
{
    cli_print_size_kb(size_bytes);
}

static void cli_print_type(uint8_t type)
{
    if (type == LFS_TYPE_REG) {
        log_printf("file");
    } else if (type == LFS_TYPE_DIR) {
        log_printf("dir");
    } else {
        log_printf("unknown(0x%02X)", type);
    }
}

static void cli_clear_delete_state(void)
{
    cli_delete_confirm_action = DELETE_CONFIRM_NONE;
    cli_delete_target[0] = '\0';
    cli_delete_file_count = 0;
    cli_mode = CLI_MODE_COMMAND;
}

static void cli_print_delete_prompt(void)
{
    if (cli_delete_confirm_action == DELETE_CONFIRM_ONE) {
        log_printf("[CLI] Delete %s? (yes/no)\r\n", cli_delete_target);
    } else if (cli_delete_confirm_action == DELETE_CONFIRM_ALL) {
        log_printf("[CLI] Delete all files? (yes/no)\r\n");
    }
}

static void cli_begin_delete_one(const char *path)
{
    strncpy(cli_delete_target, path, LFS_NAME_MAX);
    cli_delete_target[LFS_NAME_MAX] = '\0';
    cli_delete_confirm_action = DELETE_CONFIRM_ONE;
    cli_mode = CLI_MODE_DELETE_CONFIRM;
    cli_print_delete_prompt();
}

static void cli_begin_delete_all(void)
{
    int count = cli_collect_deletable_files();

    if (count < 0) {
        log_printf("[CLI] Failed to scan files.\r\n");
        cli_clear_delete_state();
        return;
    }

    if (count == 0) {
        log_printf("[CLI] No files found.\r\n");
        cli_clear_delete_state();
        return;
    }

    cli_delete_confirm_action = DELETE_CONFIRM_ALL;
    cli_mode = CLI_MODE_DELETE_CONFIRM;
    cli_print_delete_prompt();
}

static int cli_collect_deletable_files(void)
{
    struct lfs_info info;
    lfs_dir_t dir;

    cli_delete_file_count = 0;

    if (p_lfs == NULL) {
        return -1;
    }

    if (lfs_dir_open(p_lfs, &dir, "/") != 0) {
        return -1;
    }

    while (lfs_dir_read(p_lfs, &dir, &info) > 0) {
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
        log_printf("  [%u] %-20s ", (unsigned)(cli_delete_file_count + 1U), info.name);
        cli_print_size_kb((uint32_t)info.size);
        log_printf("\r\n");
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
    char *end = NULL;
    long idx;

    cmd = cli_trim(cmd);
    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "cancel") == 0) {
        cli_clear_delete_state();
        log_printf("[CLI] delete canceled.\r\n");
        return true;
    }

    idx = strtol(cmd, &end, 10);
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

    cli_begin_delete_one(cli_delete_files[idx - 1]);
    return true;
}

static bool cli_handle_delete_confirm(char *cmd)
{
    cmd = cli_trim(cmd);

    if (strcmp(cmd, "yes") == 0 || strcmp(cmd, "y") == 0) {
        if (cli_delete_confirm_action == DELETE_CONFIRM_ONE) {
            int ret = lfs_remove(p_lfs, cli_delete_target);
            if (ret == 0) {
                log_printf("[CLI] delete %s success.\r\n", cli_delete_target);
            } else {
                log_printf("[CLI] delete %s failed, ret=%d\r\n", cli_delete_target, ret);
            }
        } else if (cli_delete_confirm_action == DELETE_CONFIRM_ALL) {
            uint8_t deleted = 0;
            uint8_t failed = 0;
            uint8_t i;

            for (i = 0; i < cli_delete_file_count; i++) {
                int ret = lfs_remove(p_lfs, cli_delete_files[i]);
                if (ret == 0) {
                    deleted++;
                } else {
                    failed++;
                }
            }

            log_printf("[CLI] delete all done: %u success, %u failed.\r\n",
                       (unsigned)deleted,
                       (unsigned)failed);
        }

        cli_clear_delete_state();
        return true;
    }

    if (strcmp(cmd, "no") == 0 || strcmp(cmd, "n") == 0 || strcmp(cmd, "cancel") == 0) {
        log_printf("[CLI] delete canceled.\r\n");
        cli_clear_delete_state();
        return true;
    }

    log_printf("[CLI] please type yes or no.\r\n");
    return true;
}
