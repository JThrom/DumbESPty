#include "shell.hpp"
#include "ssh_client.hpp"
#include "terminal.hpp"
#include <cstring>
#include <cstdio>
#include "wifi_mgr.hpp"
#include "ssh_client.hpp"
#include <cstdlib>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "libssh2.h"

static const char *TAG = "shell";

#define CMD_MAX_ARGS 8
#define CMD_TABLE_SIZE 16
#define INPUT_BUF_SIZE 256
#define HISTORY_SIZE 16

typedef struct {
    const char *name;
    void (*handler)(int argc, char **argv);
} cmd_entry_t;

static cmd_entry_t cmd_table[CMD_TABLE_SIZE];
static int cmd_count = 0;

static terminal_t *term = NULL;

static char input_buf[INPUT_BUF_SIZE];
static int input_len = 0;
static int input_cursor = 0;

static char history[HISTORY_SIZE][INPUT_BUF_SIZE];
static int history_count = 0;
static int history_idx = -1;
static char history_draft[INPUT_BUF_SIZE];

static void (*input_callback)(const char *) = NULL;
static bool ssh_active = false;
static bool ssh_connect_pending = false;
static bool suppress_prompt_once = false;
static bool input_masked = false;

extern void cmd_help(int argc, char **argv);
extern void cmd_wifi(int argc, char **argv);
extern void cmd_ssh(int argc, char **argv);

void shell_print(const char *text) {
    if (term) terminal_write(term, text, -1);
}

void shell_register_cmd(const char *name, void (*handler)(int, char **)) {
    if (cmd_count >= CMD_TABLE_SIZE) {
        ESP_LOGE(TAG, "Command table full");
        return;
    }
    cmd_table[cmd_count].name = name;
    cmd_table[cmd_count].handler = handler;
    cmd_count++;
}

void shell_get_input(void (*callback)(const char *)) {
    input_callback = callback;
    input_masked = false;
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = '\0';
}

void shell_get_hidden_input(void (*callback)(const char *)) {
    input_callback = callback;
    input_masked = true;
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = '\0';
}

static void execute_command(const char *line) {
    char buf[INPUT_BUF_SIZE];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[CMD_MAX_ARGS];
    int argc = 0;
    char *token = strtok(buf, " ");
    while (token && argc < CMD_MAX_ARGS) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    if (argc == 0) return;

    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(argv[0], cmd_table[i].name) == 0) {
            cmd_table[i].handler(argc, argv);
            if (!input_callback && strcmp(argv[0], "ssh") != 0 && strcmp(argv[0], "clear") != 0) {
                shell_print("\r\n");
            }
            return;
        }
    }
    shell_print("\r\n  Unknown command: ");
    shell_print(argv[0]);
    shell_print("\r\n");
}

static void add_history(const char *line) {
    if (line[0] == '\0') return;
    if (history_count > 0 && strcmp(history[history_count - 1], line) == 0) return;
    if (history_count >= HISTORY_SIZE) {
        for (int i = 0; i < HISTORY_SIZE - 1; i++)
            strcpy(history[i], history[i + 1]);
        strcpy(history[HISTORY_SIZE - 1], line);
    } else {
        strcpy(history[history_count], line);
        history_count++;
    }
}

static void clear_screen_and_prompt(void) {
    terminal_write(term, "\033[0m\033[?25h\033[?2004l\033[2J\033[H$ ", -1);
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = '\0';
    history_idx = -1;
    input_masked = false;
}

static void prompt(bool leading_newline) {
    if (leading_newline) terminal_write(term, "\r\n", -1);
    terminal_write(term, "$ ", -1);
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = '\0';
    history_idx = -1;
    input_masked = false;
}

void shell_set_ssh_active(bool active) {
    ssh_active = active;
    if (!active) {
        clear_screen_and_prompt();
    }
}

bool shell_is_ssh_active(void) {
    return ssh_active;
}

void shell_handle_key(char c) {
    if (ssh_active) {
        if (c == 0x04) {
            ssh_disconnect();
            shell_set_ssh_active(false);
        } else {
            char buf[3];
            int n;
            if (c == 0x1C) { buf[0] = 0x1B; buf[1] = '['; buf[2] = 'D'; n = 3; }
            else if (c == 0x1D) { buf[0] = 0x1B; buf[1] = '['; buf[2] = 'C'; n = 3; }
            else if (c == 0x1E) { buf[0] = 0x1B; buf[1] = '['; buf[2] = 'A'; n = 3; }
            else if (c == 0x1F) { buf[0] = 0x1B; buf[1] = '['; buf[2] = 'B'; n = 3; }
            else { buf[0] = c; n = 1; }
            if ((uint8_t)c == 0x1B) {
                ESP_LOGI(TAG, "SSH key TX: ESC");
            }
            ssh_write(buf, n);
        }
        return;
    }

    if (c == '\b') {
        if (input_cursor > 0) {
            terminal_write(term, "\b \b", 3);
            memmove(input_buf + input_cursor - 1, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_len--;
            input_cursor--;
        }
    } else if (c == 0x1B) {
        input_callback = NULL;
        input_masked = false;
        input_len = 0;
        input_cursor = 0;
        input_buf[0] = '\0';
        prompt(true);
    } else if (c == '\n' || c == '\r') {
        input_buf[input_len] = '\0';
        terminal_write(term, "\r\n", 2);
        if (input_len > 0 || !input_callback) {
            char cmd[INPUT_BUF_SIZE];
            strcpy(cmd, input_buf);
            input_len = 0;
            input_cursor = 0;
            input_buf[0] = '\0';
            if (input_callback) {
                void (*cb)(const char *) = input_callback;
                input_callback = NULL;
                cb(cmd);
            } else if (cmd[0] != '\0') {
                add_history(cmd);
                execute_command(cmd);
            }
        }
        if (!input_callback && !ssh_active && !ssh_connect_pending) {
            if (suppress_prompt_once) suppress_prompt_once = false;
            else prompt(false);
        }
    } else if (c == 0x1E) {
        if (history_count == 0) return;
        if (history_idx == -1) {
            strcpy(history_draft, input_buf);
            history_idx = history_count - 1;
        } else if (history_idx > 0) {
            history_idx--;
        } else {
            return;
        }
        int new_len = strlen(history[history_idx]);
        int clear = input_len > new_len ? input_len : 0;
        if (clear > 0) {
            char *spaces = (char *)malloc(clear + 1);
            if (spaces) {
                memset(spaces, ' ', clear);
                spaces[clear] = '\0';
                terminal_write(term, "\r$ ", 3);
                terminal_write(term, spaces, clear);
                free(spaces);
            }
        }
        terminal_write(term, "\r$ ", 3);
        terminal_write(term, history[history_idx], new_len);
        strcpy(input_buf, history[history_idx]);
        input_len = new_len;
        input_cursor = new_len;
    } else if (c == 0x1F) {
        if (history_idx == -1) return;
        const char *new_text;
        int new_len;
        if (history_idx < history_count - 1) {
            history_idx++;
            new_text = history[history_idx];
        } else {
            history_idx = -1;
            new_text = history_draft;
        }
        new_len = strlen(new_text);
        int clear = input_len > new_len ? input_len : 0;
        if (clear > 0) {
            char *spaces = (char *)malloc(clear + 1);
            if (spaces) {
                memset(spaces, ' ', clear);
                spaces[clear] = '\0';
                terminal_write(term, "\r$ ", 3);
                terminal_write(term, spaces, clear);
                free(spaces);
            }
        }
        terminal_write(term, "\r$ ", 3);
        terminal_write(term, new_text, new_len);
        strcpy(input_buf, new_text);
        input_len = new_len;
        input_cursor = new_len;
    } else if (c == 0x1C) {
        if (input_cursor > 0) {
            terminal_write(term, "\b", 1);
            input_cursor--;
        }
    } else if (c == 0x1D) {
        if (input_cursor < input_len) {
            terminal_write(term, "\033[C", 3);
            input_cursor++;
        }
    } else {
        if (input_len < INPUT_BUF_SIZE - 1) {
            char ch = c;
            if (input_callback && input_masked) {
                char mask = '*';
                terminal_write(term, &mask, 1);
            } else {
                terminal_write(term, &ch, 1);
            }
            memmove(input_buf + input_cursor + 1, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_buf[input_cursor] = c;
            input_len++;
            input_cursor++;
        }
    }
}

static void cmd_help_handler(int argc, char **argv) {
    shell_print("\r\n  Available commands:");
    for (int i = 0; i < cmd_count; i++) {
        shell_print("\r\n    ");
        shell_print(cmd_table[i].name);
    }
    shell_print("\r\n");
}

static void cmd_clear_handler(int argc, char **argv) {
    (void)argc;
    (void)argv;
    suppress_prompt_once = true;
    clear_screen_and_prompt();
}

static void cmd_about_handler(int argc, char **argv) {
    (void)argc;
    (void)argv;

    shell_print("\033[2J\033[H");

    shell_print("\r\n\033[48;2;10;12;28m\033[38;2;255;170;95m");
    shell_print("  8888888b.                         888      8888888888 .d8888b.  8888888b.  888");
    shell_print("\r\n  888  \"Y88b                        888      888       d88P  Y88b 888   Y88b 888");
    shell_print("\r\n  888    888                        888      888       Y88b.      888    888 888");
    shell_print("\r\n  888    888 888  888 88888b.d88b.  88888b.  8888888    \"Y888b.   888   d88P 888888 888  888");
    shell_print("\r\n  888    888 888  888 888 \"888 \"88b 888 \"88b 888           \"Y88b. 8888888P\"  888    888  888");
    shell_print("\r\n  888    888 888  888 888  888  888 888  888 888             \"888 888        888    888  888");
    shell_print("\r\n  888  .d88P Y88b 888 888  888  888 888 d88P 888       Y88b  d88P 888        Y88b.  Y88b 888");
    shell_print("\r\n  8888888P\"   \"Y88888 888  888  888 88888P\"  8888888888 \"Y8888P\"  888         \"Y888  \"Y88888");
    shell_print("\r\n                                                                                         888");
    shell_print("\r\n                                                                                    Y8b d88P");
    shell_print("\r\n                                                                                     \"Y88P\"\033[0m");

    shell_print("\r\n\033[48;2;30;20;44m\033[38;2;255;212;120m  DEVICE\033[0m");
    const esp_app_desc_t *app_desc = esp_app_get_description();
    shell_print("\r\n\033[38;2;255;212;120m    Product:\033[0m Waveshare ESP32-S3-Touch-LCD-7");
    shell_print("\r\n\033[38;2;255;212;120m    DumbESPty version:\033[0m ");
    shell_print(app_desc->version);
    shell_print("\r\n\033[38;2;255;212;120m    Author:\033[0m Jason Throm");
    shell_print("\r\n\033[38;2;255;212;120m    GitHub:\033[0m https://github.com/JThrom/DumbESPty");
    shell_print("\r\n\033[38;2;255;212;120m    License:\033[0m GNU/GPL v2");
    shell_print("\r\n\033[38;2;255;212;120m    Display:\033[0m 7-inch RGB panel, terminal grid 100 x 32, cell 8 x 15");
    shell_print("\r\n\033[38;2;255;212;120m    Controller helper:\033[0m CH422G I2C expander init path enabled");

    shell_print("\r\n\r\n\033[48;2;16;36;72m\033[38;2;130;220;255m  MCU + SDK\033[0m");
    shell_print("\r\n\033[38;2;130;220;255m    MCU:\033[0m Espressif ESP32-S3 (dual-core Xtensa LX7, Wi-Fi + BLE)");
    shell_print("\r\n\033[38;2;130;220;255m    ESP-IDF:\033[0m ");
    shell_print(esp_get_idf_version());
    shell_print("\r\n\033[38;2;130;220;255m    FreeRTOS:\033[0m ");
    shell_print(tskKERNEL_VERSION_NUMBER);

    shell_print("\r\n\r\n\033[48;2;52;24;70m\033[38;2;255;170;230m  GRAPHICS + CONNECTIVITY\033[0m");
    shell_print("\r\n\033[38;2;255;170;230m    LVGL:\033[0m ");
    shell_print(LVGL_VERSION_MAJOR == 0 ? "unknown" : "v");
    if (LVGL_VERSION_MAJOR != 0) {
        char lvgl_ver[32];
        snprintf(lvgl_ver, sizeof(lvgl_ver), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
        shell_print(lvgl_ver);
    }
    shell_print("\r\n\033[38;2;255;170;230m    SSH transport:\033[0m libssh2 ");
    shell_print(LIBSSH2_VERSION);
    shell_print(" (non-blocking channel mode)");
    shell_print("\r\n\033[38;2;255;170;230m    Input paths:\033[0m BLE HID host + USB serial console\033[0m\r\n");
}

static char ssh_host[64];
static uint16_t ssh_port;
static char ssh_user[32];

typedef struct {
    char host[64];
    uint16_t port;
    char user[32];
    char pass[INPUT_BUF_SIZE];
} ssh_connect_ctx_t;

static void ssh_connect_task(void *param) {
    ssh_connect_ctx_t *ctx = (ssh_connect_ctx_t *)param;
    bool ok = ssh_connect(ctx->host, ctx->port, ctx->user, ctx->pass);
    if (ok) {
        shell_set_ssh_active(true);
    } else {
        shell_print("\r\n  SSH connection failed");
        if (!ssh_active) prompt(true);
    }
    ssh_connect_pending = false;
    free(ctx);
    vTaskDelete(NULL);
}

static void ssh_password_cb(const char *pass) {
    if (ssh_connect_pending) {
        shell_print("\r\n  SSH connect already in progress");
        return;
    }

    ssh_connect_ctx_t *ctx = (ssh_connect_ctx_t *)malloc(sizeof(ssh_connect_ctx_t));
    if (!ctx) {
        shell_print("\r\n  SSH connect alloc failed");
        return;
    }

    strncpy(ctx->host, ssh_host, sizeof(ctx->host) - 1);
    ctx->host[sizeof(ctx->host) - 1] = '\0';
    ctx->port = ssh_port;
    strncpy(ctx->user, ssh_user, sizeof(ctx->user) - 1);
    ctx->user[sizeof(ctx->user) - 1] = '\0';
    strncpy(ctx->pass, pass, sizeof(ctx->pass) - 1);
    ctx->pass[sizeof(ctx->pass) - 1] = '\0';

    ssh_connect_pending = true;
    shell_print("\r\n  Connecting...");

    BaseType_t ret = xTaskCreatePinnedToCore(ssh_connect_task, "ssh_connect", 12288, ctx, 5, NULL, 1);
    if (ret != pdPASS) {
        ssh_connect_pending = false;
        free(ctx);
        shell_print("\r\n  SSH connect task create failed");
    }
}

static void cmd_ssh_handler(int argc, char **argv) {
    if (argc < 2) {
        shell_print("\r\n  Usage: ssh user@host[:port]");
        return;
    }

    if (!wifi_mgr_is_connected()) {
        shell_print("\r\n  wifi disconnected");
        return;
    }

    const char *arg = argv[1];
    const char *at = strchr(arg, '@');
    if (!at) {
        shell_print("\r\n  Usage: ssh user@host[:port]");
        return;
    }

    int user_len = at - arg;
    if (user_len >= (int)sizeof(ssh_user)) user_len = sizeof(ssh_user) - 1;
    memcpy(ssh_user, arg, user_len);
    ssh_user[user_len] = '\0';

    const char *host_part = at + 1;
    const char *colon = strchr(host_part, ':');
    if (colon) {
        int host_len = colon - host_part;
        if (host_len >= (int)sizeof(ssh_host)) host_len = sizeof(ssh_host) - 1;
        memcpy(ssh_host, host_part, host_len);
        ssh_host[host_len] = '\0';
        ssh_port = atoi(colon + 1);
    } else {
        strncpy(ssh_host, host_part, sizeof(ssh_host) - 1);
        ssh_host[sizeof(ssh_host) - 1] = '\0';
        ssh_port = 22;
    }

    shell_print("\r\n  Password: ");
    shell_get_hidden_input(ssh_password_cb);
}

void shell_init(terminal_t *t) {
    term = t;

    input_buf[0] = '\0';
    input_len = 0;
    input_cursor = 0;
    history_count = 0;
    history_idx = -1;
    input_callback = NULL;

    shell_register_cmd("help", cmd_help_handler);
    shell_register_cmd("clear", cmd_clear_handler);
    shell_register_cmd("wifi", cmd_wifi);
    shell_register_cmd("ssh", cmd_ssh_handler);
    shell_register_cmd("about", cmd_about_handler);

    terminal_write(term, "$ ", -1);
}
