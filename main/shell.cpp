#include "shell.hpp"
#include "hostname_mgr.hpp"
#include "secret_vault.hpp"
#include "ssh_client.hpp"
#include "tailscale_mgr.hpp"
#include "terminal.hpp"
#include <cstring>
#include <cstdio>
#include "wifi_mgr.hpp"
#include <cstdlib>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lvgl.h"
#include "libssh2.h"

static const char *TAG = "shell";

#define CMD_MAX_ARGS 8
#define CMD_TABLE_SIZE 16
#define INPUT_BUF_SIZE 2048
#define HISTORY_SIZE 16
#define SSH_PASS_BUF_SIZE 256
#define SSH_KEY_MAX_LEN 4096

static constexpr const char *SSH_VAULT_KEY_PRIVATE = "ssh_privkey";
static constexpr const char *SSH_VAULT_KEY_PASSPHRASE = "ssh_keypass";

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
static int rendered_input_len = 0;

static char history[HISTORY_SIZE][INPUT_BUF_SIZE];
static int history_count = 0;
static int history_idx = -1;
static char history_draft[INPUT_BUF_SIZE];

static void (*input_callback)(const char *) = NULL;
static bool ssh_active = false;
static bool ssh_connect_pending = false;
static bool suppress_prompt_once = false;
static bool input_masked = false;

static int completion_count = 0;
static int completion_context_argc = 0;
static int completion_token_start = 0;
static int completion_token_len = 0;
static char completion_items[16][33];
static bool completion_tab_pending = false;
static char completion_tab_snapshot[INPUT_BUF_SIZE];
static int completion_tab_snapshot_len = 0;
static int completion_tab_snapshot_cursor = 0;

extern void cmd_help(int argc, char **argv);
extern void cmd_wifi(int argc, char **argv);
extern void cmd_ssh(int argc, char **argv);
extern void cmd_tailscale(int argc, char **argv);
extern void cmd_hostname(int argc, char **argv);
static void sshkey_reset_pending(void);

static void completion_reset(void) {
    completion_count = 0;
    completion_context_argc = 0;
    completion_token_start = 0;
    completion_token_len = 0;
    completion_tab_pending = false;
    completion_tab_snapshot[0] = '\0';
    completion_tab_snapshot_len = 0;
    completion_tab_snapshot_cursor = 0;
}

static void redraw_input_line(void) {
    char spaces[INPUT_BUF_SIZE + 1];
    memset(spaces, ' ', sizeof(spaces) - 1);
    spaces[sizeof(spaces) - 1] = '\0';
    const int clear_len = rendered_input_len > input_len ? rendered_input_len : input_len;
    terminal_write(term, "\r$ ", 3);
    if (clear_len > 0) terminal_write(term, spaces, clear_len);
    terminal_write(term, "\r$ ", 3);
    if (input_callback && input_masked) {
        if (input_len > 0) {
            memset(spaces, '*', input_len);
            terminal_write(term, spaces, input_len);
        }
    } else {
        terminal_write(term, input_buf, input_len);
    }
    if (input_cursor < input_len) {
        char seq[24];
        const int move_left = input_len - input_cursor;
        snprintf(seq, sizeof(seq), "\033[%dD", move_left);
        terminal_write(term, seq, -1);
    }
    rendered_input_len = input_len;
}

static bool completion_prefix_match(const char *candidate, const char *prefix) {
    if (!candidate || !prefix) return false;
    const size_t prefix_len = strlen(prefix);
    return strncmp(candidate, prefix, prefix_len) == 0;
}

static void completion_sort_items(char items[][33], int count) {
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(items[i], items[j]) > 0) {
                char tmp[33];
                memcpy(tmp, items[i], sizeof(tmp));
                memcpy(items[i], items[j], sizeof(tmp));
                memcpy(items[j], tmp, sizeof(tmp));
            }
        }
    }
}

static void completion_add_item(char out[][33], int max_entries, int *count, const char *candidate, const char *prefix) {
    if (!candidate || !count || *count >= max_entries) return;
    if (!completion_prefix_match(candidate, prefix)) return;
    const size_t nlen = strnlen(candidate, 32);
    memcpy(out[*count], candidate, nlen);
    out[*count][nlen] = '\0';
    (*count)++;
}

static void completion_collect_saved_ssids(char out[][33], int max_entries, int *count, const char *prefix) {
    char ssids[16][33];
    const int saved_count = wifi_mgr_get_saved_ssids(ssids, 16);
    for (int i = 0; i < saved_count && *count < max_entries; i++) {
        completion_add_item(out, max_entries, count, ssids[i], prefix);
    }
}

static int completion_collect_matches(char out[][33], int max_entries, const char *prefix, int context_argc, char **context_argv) {
    for (int i = 0; i < max_entries; i++) out[i][0] = '\0';
    int n = 0;

    if (context_argc == 0) {
        for (int i = 0; i < cmd_count && n < max_entries; i++) {
            completion_add_item(out, max_entries, &n, cmd_table[i].name, prefix);
        }
        completion_sort_items(out, n);
        return n;
    }

    const char *cmd = context_argv[0];
    if (strcmp(cmd, "wifi") == 0) {
        if (context_argc == 1) {
            static const char *wifi_subcmds[] = {
                "scan", "status", "list", "connect", "disconnect", "forget"
            };
            for (size_t i = 0; i < sizeof(wifi_subcmds) / sizeof(wifi_subcmds[0]) && n < max_entries; i++) {
                completion_add_item(out, max_entries, &n, wifi_subcmds[i], prefix);
            }
            completion_sort_items(out, n);
            return n;
        }
        if (context_argc == 2 &&
            (strcmp(context_argv[1], "connect") == 0 || strcmp(context_argv[1], "forget") == 0)) {
            completion_collect_saved_ssids(out, max_entries, &n, prefix);
            completion_sort_items(out, n);
            return n;
        }
        return 0;
    }

    if (strcmp(cmd, "sshkey") == 0 && context_argc == 1) {
        static const char *sshkey_subcmds[] = {
            "status", "import", "load", "clear", "erase"
        };
        for (size_t i = 0; i < sizeof(sshkey_subcmds) / sizeof(sshkey_subcmds[0]) && n < max_entries; i++) {
            completion_add_item(out, max_entries, &n, sshkey_subcmds[i], prefix);
        }
        completion_sort_items(out, n);
        return n;
    }

    if (strcmp(cmd, "sshknown") == 0 && context_argc == 1) {
        static const char *sshknown_subcmds[] = {
            "list", "remove", "trust", "clear"
        };
        for (size_t i = 0; i < sizeof(sshknown_subcmds) / sizeof(sshknown_subcmds[0]) && n < max_entries; i++) {
            completion_add_item(out, max_entries, &n, sshknown_subcmds[i], prefix);
        }
        completion_sort_items(out, n);
        return n;
    }

    if (strcmp(cmd, "tailscale") == 0) {
        if (context_argc == 1) {
            static const char *tailscale_subcmds[] = {
                "enable", "disable", "backend", "set", "show", "ping", "up", "down", "status", "devices", "clear"
            };
            for (size_t i = 0; i < sizeof(tailscale_subcmds) / sizeof(tailscale_subcmds[0]) && n < max_entries; i++) {
                completion_add_item(out, max_entries, &n, tailscale_subcmds[i], prefix);
            }
            completion_sort_items(out, n);
            return n;
        }
        if (context_argc == 2 && strcmp(context_argv[1], "backend") == 0) {
            static const char *backend_opts[] = {"saas", "headscale"};
            for (size_t i = 0; i < sizeof(backend_opts) / sizeof(backend_opts[0]) && n < max_entries; i++) {
                completion_add_item(out, max_entries, &n, backend_opts[i], prefix);
            }
            completion_sort_items(out, n);
            return n;
        }
        if (context_argc == 2 && strcmp(context_argv[1], "set") == 0) {
            static const char *set_opts[] = {"tailnet", "control-url", "authkey", "token"};
            for (size_t i = 0; i < sizeof(set_opts) / sizeof(set_opts[0]) && n < max_entries; i++) {
                completion_add_item(out, max_entries, &n, set_opts[i], prefix);
            }
            completion_sort_items(out, n);
            return n;
        }
        if (context_argc == 2 && strcmp(context_argv[1], "show") == 0) {
            static const char *show_opts[] = {"authkey", "token"};
            for (size_t i = 0; i < sizeof(show_opts) / sizeof(show_opts[0]) && n < max_entries; i++) {
                completion_add_item(out, max_entries, &n, show_opts[i], prefix);
            }
            completion_sort_items(out, n);
            return n;
        }
        return 0;
    }

    if (strcmp(cmd, "hostname") == 0 && context_argc == 1) {
        completion_add_item(out, max_entries, &n, "set", prefix);
        completion_sort_items(out, n);
        return n;
    }

    if (strcmp(cmd, "vault") == 0 && context_argc == 1) {
        static const char *vault_subcmds[] = {"status", "rekey"};
        for (size_t i = 0; i < sizeof(vault_subcmds) / sizeof(vault_subcmds[0]) && n < max_entries; i++) {
            completion_add_item(out, max_entries, &n, vault_subcmds[i], prefix);
        }
        completion_sort_items(out, n);
        return n;
    }

    if (strcmp(cmd, "mac") == 0 && context_argc == 1) {
        completion_add_item(out, max_entries, &n, "set", prefix);
        completion_sort_items(out, n);
        return n;
    }

    completion_sort_items(out, n);
    return n;
}

static bool completion_try_begin(bool prefer_next_level_exact) {
    if (input_cursor != input_len) return false;

    const bool trailing_space = (input_len > 0 && input_buf[input_len - 1] == ' ');

    int token_start = input_len;
    if (!trailing_space) {
        while (token_start > 0 && input_buf[token_start - 1] != ' ') {
            token_start--;
        }
    }

    int token_len = input_len - token_start;
    if (token_len < 0 || token_len > 32) return false;

    char prefix[33];
    memcpy(prefix, input_buf + token_start, token_len);
    prefix[token_len] = '\0';

    char context_buf[INPUT_BUF_SIZE];
    if (token_start > 0) {
        memcpy(context_buf, input_buf, token_start);
    }
    context_buf[token_start] = '\0';

    char *context_argv[CMD_MAX_ARGS];
    int context_argc = 0;
    char *tok = strtok(context_buf, " ");
    while (tok && context_argc < CMD_MAX_ARGS) {
        context_argv[context_argc++] = tok;
        tok = strtok(NULL, " ");
    }

    int match_count = completion_collect_matches(completion_items,
                                                 sizeof(completion_items) / sizeof(completion_items[0]),
                                                 prefix,
                                                 context_argc,
                                                 context_argv);

    if (prefer_next_level_exact && !trailing_space && match_count == 1 && strcmp(completion_items[0], prefix) == 0) {
        char next_context_buf[INPUT_BUF_SIZE];
        snprintf(next_context_buf, sizeof(next_context_buf), "%s", input_buf);

        char *next_context_argv[CMD_MAX_ARGS];
        int next_context_argc = 0;
        char *next_tok = strtok(next_context_buf, " ");
        while (next_tok && next_context_argc < CMD_MAX_ARGS) {
            next_context_argv[next_context_argc++] = next_tok;
            next_tok = strtok(NULL, " ");
        }

        match_count = completion_collect_matches(completion_items,
                                                 sizeof(completion_items) / sizeof(completion_items[0]),
                                                 "",
                                                 next_context_argc,
                                                 next_context_argv);
        token_start = input_len;
        token_len = 0;
        context_argc = next_context_argc;
    }

    completion_count = match_count;
    completion_context_argc = context_argc;
    completion_token_start = token_start;
    completion_token_len = token_len;
    if (completion_count <= 0) return false;
    return true;
}

static void completion_apply_text(const char *text, bool append_space) {
    if (!text) return;
    const int text_len = strlen(text);

    int new_len = completion_token_start + text_len;
    memcpy(input_buf + completion_token_start, text, text_len);
    if (append_space && new_len < INPUT_BUF_SIZE - 1) input_buf[new_len++] = ' ';
    input_buf[new_len] = '\0';
    input_len = new_len;
    input_cursor = new_len;
    redraw_input_line();
}

static int completion_common_prefix_len(void) {
    if (completion_count <= 0) return 0;
    int common = strlen(completion_items[0]);
    for (int i = 1; i < completion_count; i++) {
        int j = 0;
        while (j < common && completion_items[0][j] == completion_items[i][j]) j++;
        common = j;
    }
    return common;
}

static void completion_show_matches(void) {
    if (completion_count <= 0) return;
    for (int i = 0; i < completion_count; i++) {
        shell_print("\r\n");
        shell_print(completion_items[i]);
    }
    shell_print("\r\n");
    redraw_input_line();
}

static void completion_arm_tab_pending(void) {
    completion_tab_pending = true;
    completion_tab_snapshot_len = input_len;
    completion_tab_snapshot_cursor = input_cursor;
    memcpy(completion_tab_snapshot, input_buf, input_len);
    completion_tab_snapshot[input_len] = '\0';
}

static bool completion_is_tab_pending_same_line(void) {
    if (!completion_tab_pending) return false;
    if (completion_tab_snapshot_len != input_len) return false;
    if (completion_tab_snapshot_cursor != input_cursor) return false;
    return memcmp(completion_tab_snapshot, input_buf, input_len) == 0;
}

void shell_print(const char *text) {
    if (term) terminal_write(term, text, -1);
}

void shell_pump_ui(void) {
    if (!term) return;
    terminal_render(term);
    lv_timer_handler();
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
    completion_reset();
    input_masked = false;
    input_len = 0;
    input_cursor = 0;
    rendered_input_len = 0;
    input_buf[0] = '\0';
}

void shell_get_hidden_input(void (*callback)(const char *)) {
    input_callback = callback;
    completion_reset();
    input_masked = true;
    input_len = 0;
    input_cursor = 0;
    rendered_input_len = 0;
    input_buf[0] = '\0';
}

static void execute_command(const char *line) {
    char buf[INPUT_BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s", line);

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
    rendered_input_len = 0;
    input_buf[0] = '\0';
    history_idx = -1;
    input_masked = false;
    completion_reset();
}

static void prompt(bool leading_newline) {
    if (leading_newline) terminal_write(term, "\r\n", -1);
    terminal_write(term, "$ ", -1);
    input_len = 0;
    input_cursor = 0;
    rendered_input_len = 0;
    input_buf[0] = '\0';
    history_idx = -1;
    input_masked = false;
    completion_reset();
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
    if ((uint8_t)c == 0x80) {
        terminal_scrollback_step(term, 1);
        shell_pump_ui();
        return;
    }
    if ((uint8_t)c == 0x81) {
        terminal_scrollback_step(term, -1);
        shell_pump_ui();
        return;
    }

    if (terminal_scrollback_active(term)) {
        terminal_scrollback_reset(term);
        shell_pump_ui();
    }

    if (ssh_active) {
        char buf[3];
        int n;
        if (c == 0x1C) { buf[0] = 0x1B; buf[1] = '['; buf[2] = 'D'; n = 3; }
        else if (c == 0x1D) { buf[0] = 0x1B; buf[1] = '['; buf[2] = 'C'; n = 3; }
        else if (c == 0x1E) { buf[0] = 0x1B; buf[1] = '['; buf[2] = 'A'; n = 3; }
        else if (c == 0x1F) { buf[0] = 0x1B; buf[1] = '['; buf[2] = 'B'; n = 3; }
        else if (c == '\n' || c == '\r') { buf[0] = '\r'; n = 1; }
        else { buf[0] = c; n = 1; }
        if ((uint8_t)c == 0x1B) {
            ESP_LOGI(TAG, "SSH key TX: ESC");
        }
        ssh_write(buf, n);
        return;
    }

    if (c == '\b') {
        completion_reset();
        if (input_cursor > 0) {
            memmove(input_buf + input_cursor - 1, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_len--;
            input_cursor--;
            if (input_callback && input_masked && input_cursor == input_len) {
                terminal_write(term, "\b \b", 3);
                rendered_input_len = input_len;
            } else {
                redraw_input_line();
            }
        }
    } else if ((uint8_t)c == 0x7F) {
        completion_reset();
        if (input_cursor < input_len) {
            memmove(input_buf + input_cursor,
                    input_buf + input_cursor + 1,
                    input_len - input_cursor);
            input_len--;
            if (input_callback && input_masked) {
                rendered_input_len = input_len;
            } else {
                redraw_input_line();
            }
        }
    } else if (c == 0x1B) {
        completion_reset();
        input_callback = NULL;
        sshkey_reset_pending();
        input_masked = false;
        input_len = 0;
        input_cursor = 0;
        rendered_input_len = 0;
        input_buf[0] = '\0';
        prompt(true);
    } else if (c == '\n' || c == '\r') {
        completion_reset();
        input_buf[input_len] = '\0';
        terminal_write(term, "\r\n", 2);
        if (input_len > 0 || !input_callback) {
            char cmd[INPUT_BUF_SIZE];
            strcpy(cmd, input_buf);
            input_len = 0;
            input_cursor = 0;
            rendered_input_len = 0;
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
        completion_reset();
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
        rendered_input_len = new_len;
    } else if (c == 0x1F) {
        completion_reset();
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
        rendered_input_len = new_len;
    } else if (c == 0x1C) {
        completion_reset();
        if (input_cursor > 0) {
            terminal_write(term, "\b", 1);
            input_cursor--;
        }
    } else if (c == 0x1D) {
        completion_reset();
        if (input_cursor < input_len) {
            terminal_write(term, "\033[C", 3);
            input_cursor++;
        }
    } else if (c == '\t') {
        if (input_callback) return;

        if (completion_is_tab_pending_same_line()) {
            if (!completion_try_begin(true)) {
                completion_reset();
                terminal_write(term, "\a", 1);
                return;
            }
            completion_show_matches();
            completion_tab_pending = false;
            return;
        }

        if (!completion_try_begin(false)) {
            completion_reset();
            terminal_write(term, "\a", 1);
            return;
        }

        char current_prefix[33];
        memcpy(current_prefix, input_buf + completion_token_start, completion_token_len);
        current_prefix[completion_token_len] = '\0';

        if (completion_count == 1) {
            const bool exact_match = strcmp(completion_items[0], current_prefix) == 0;
            const bool trailing_space = (input_len > 0 && input_buf[input_len - 1] == ' ');

            if (exact_match) {
                if (!trailing_space && completion_context_argc > 0) {
                    completion_apply_text(completion_items[0], true);
                }
                completion_arm_tab_pending();
                return;
            }

            completion_apply_text(completion_items[0], true);
            completion_tab_pending = false;
            return;
        }

        int common_len = completion_common_prefix_len();
        if (common_len > completion_token_len) {
            char common_prefix[33];
            memcpy(common_prefix, completion_items[0], common_len);
            common_prefix[common_len] = '\0';
            completion_apply_text(common_prefix, false);
        }
        completion_arm_tab_pending();
    } else {
        completion_reset();
        if (input_len < INPUT_BUF_SIZE - 1) {
            char ch = c;
            const bool insert_in_middle = input_cursor < input_len;
            if (!insert_in_middle && input_callback && input_masked) {
                char mask = '*';
                terminal_write(term, &mask, 1);
            } else if (!insert_in_middle) {
                terminal_write(term, &ch, 1);
            }
            memmove(input_buf + input_cursor + 1, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_buf[input_cursor] = c;
            input_len++;
            input_cursor++;
            if (insert_in_middle) {
                redraw_input_line();
            } else {
                rendered_input_len = input_len;
            }
        }
    }
}

static void cmd_help_handler(int argc, char **argv) {
    (void)argc;
    (void)argv;
    shell_print("\r\n  Available commands:");
    const char *names[CMD_TABLE_SIZE];
    int n = cmd_count < CMD_TABLE_SIZE ? cmd_count : CMD_TABLE_SIZE;
    for (int i = 0; i < n; i++) names[i] = cmd_table[i].name;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                const char *tmp = names[i];
                names[i] = names[j];
                names[j] = tmp;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        shell_print("\r\n    ");
        shell_print(names[i]);
    }
    shell_print("\r\n");
}

static void cmd_clear_handler(int argc, char **argv) {
    (void)argc;
    (void)argv;
    suppress_prompt_once = true;
    clear_screen_and_prompt();
}

// Tokyo Night theme (the LazyVim default), as SGR truecolor escapes.
#define TN_RESET   "\033[0m"
#define TN_FG      "\033[38;2;192;202;245m"   // #c0caf5 foreground
#define TN_MUTED   "\033[38;2;86;95;137m"     // #565f89 comments/muted
#define TN_BLUE    "\033[38;2;122;162;247m"   // #7aa2f7
#define TN_CYAN    "\033[38;2;125;207;255m"   // #7dcfff
#define TN_GREEN   "\033[38;2;158;206;106m"   // #9ece6a
#define TN_PURPLE  "\033[38;2;187;154;247m"   // #bb9af7
#define TN_ORANGE  "\033[38;2;224;175;104m"   // #e0af68
#define TN_RED     "\033[38;2;247;118;142m"   // #f7768e
#define TN_DARK    "\033[38;2;65;72;104m"     // #414868 borders

// Box-drawing helpers (UTF-8). Box is 92 columns wide inside the borders.
#define TN_BAR     "\xe2\x96\x8c"             // '▌' section accent bar

static void about_row(const char *color, const char *label, const char *value) {
    char line[256];
    snprintf(line, sizeof(line),
             "\r\n  " TN_DARK "\xe2\x94\x82" TN_RESET "  %s\xe2\x80\xba" TN_RESET
             " %s%-18s" TN_RESET TN_FG "%s" TN_RESET,
             color, color, label, value);
    shell_print(line);
}

static void about_section(const char *color, const char *title) {
    char line[160];
    snprintf(line, sizeof(line),
             "\r\n  " TN_DARK "\xe2\x94\x82" TN_RESET "\r\n  " TN_DARK "\xe2\x94\x82" TN_RESET
             "  %s" TN_BAR " %s" TN_RESET,
             color, title);
    shell_print(line);
}

static void cmd_about_handler(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const esp_app_desc_t *app_desc = esp_app_get_description();

    shell_print("\033[2J\033[H");

    // Title block: ASCII art inside a rounded Tokyo Night frame.
    shell_print("\r\n  " TN_DARK
        "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae" TN_RESET);

    shell_print("\r\n  " TN_ORANGE "8888888b.                         888      8888888888 .d8888b.  8888888b.  888");
    shell_print("\r\n  " "888  \"Y88b                        888      888       d88P  Y88b 888   Y88b 888");
    shell_print("\r\n  " "888    888                        888      888       Y88b.      888    888 888");
    shell_print("\r\n  " "888    888 888  888 88888b.d88b.  88888b.  8888888    \"Y888b.   888   d88P 888888 888  888");
    shell_print("\r\n  " "888    888 888  888 888 \"888 \"88b 888 \"88b 888           \"Y88b. 8888888P\"  888    888  888");
    shell_print("\r\n  " "888    888 888  888 888  888  888 888  888 888             \"888 888        888    888  888");
    shell_print("\r\n  " "888  .d88P Y88b 888 888  888  888 888 d88P 888       Y88b  d88P 888        Y88b.  Y88b 888");
    shell_print("\r\n  " "8888888P\"   \"Y88888 888  888  888 88888P\"  8888888888 \"Y8888P\"  888         \"Y888  \"Y88888");
    shell_print("\r\n  " "                                                                                       888");
    shell_print("\r\n  " "                                                                                  Y8b d88P");
    shell_print("\r\n  " "                                                                                   \"Y88P\"" TN_RESET);

    shell_print("\r\n  " TN_DARK
        "\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xaf" TN_RESET);

    // Tagline.
    shell_print("\r\n  " TN_MUTED "  a dumb terminal for ESP32 \xe2\x80\xa2 SSH over Wi-Fi + Tailscale" TN_RESET);

    // DEVICE
    about_section(TN_ORANGE, "DEVICE");
    #if CONFIG_IDF_TARGET_ESP32P4
    about_row(TN_ORANGE, "Product", "Waveshare ESP32-P4-WIFI6-Touch-LCD-7B");
    #else
    about_row(TN_ORANGE, "Product", "Waveshare ESP32-S3-Touch-LCD-7");
    #endif
    about_row(TN_ORANGE, "Version", app_desc->version);
    about_row(TN_ORANGE, "Author", "Jason Throm");
    about_row(TN_ORANGE, "GitHub", "https://github.com/JThrom/DumbESPty");
    about_row(TN_ORANGE, "License", "GNU/GPL v2");
    if (term) {
        char display_val[96];
        snprintf(display_val, sizeof(display_val),
                 "7\" RGB panel \xe2\x80\xa2 grid %dx%d \xe2\x80\xa2 cell %dx%d",
                 term->cols, term->rows, term->font_w, term->font_h);
        about_row(TN_ORANGE, "Display", display_val);
    } else {
        about_row(TN_ORANGE, "Display", "7\" RGB panel");
    }
    about_row(TN_ORANGE, "I/O expander", "CH422G (I2C init path enabled)");

    // SYSTEM
    about_section(TN_BLUE, "SYSTEM");
    #if CONFIG_IDF_TARGET_ESP32P4
    about_row(TN_BLUE, "MCU", "ESP32-P4 (dual RISC-V + LP core)");
    about_row(TN_BLUE, "Wireless", "ESP32-C6 companion \xe2\x80\xa2 Wi-Fi 6 + BLE 5");
    #else
    about_row(TN_BLUE, "MCU", "ESP32-S3 (dual Xtensa LX7, Wi-Fi + BLE)");
    #endif
    about_row(TN_BLUE, "ESP-IDF", esp_get_idf_version());
    about_row(TN_BLUE, "FreeRTOS", tskKERNEL_VERSION_NUMBER);
    {
        char lvgl_ver[32];
        snprintf(lvgl_ver, sizeof(lvgl_ver), "%d.%d.%d",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
        about_row(TN_BLUE, "LVGL", LVGL_VERSION_MAJOR == 0 ? "unknown" : lvgl_ver);
    }

    // TERMINAL + CONNECTIVITY
    about_section(TN_PURPLE, "TERMINAL & CONNECTIVITY");
    about_row(TN_PURPLE, "Emulation", "VT100/xterm \xe2\x80\xa2 256-color, themable fg");
    {
        char ssh_val[80];
        snprintf(ssh_val, sizeof(ssh_val), "libssh2 %s (non-blocking)", LIBSSH2_VERSION);
        about_row(TN_PURPLE, "SSH", ssh_val);
    }
    about_row(TN_PURPLE, "Networking", "Wi-Fi station + Tailscale overlay");
    about_row(TN_PURPLE, "Keyboard", "BLE HID + USB OTG HID host");
    about_row(TN_PURPLE, "Console", "USB serial");

    shell_print("\r\n  " TN_DARK "\xe2\x94\x82" TN_RESET "\r\n");
}

static char ssh_host[64];
static uint16_t ssh_port;
static char ssh_user[32] = "root";

typedef struct {
    char host[64];
    uint16_t port;
    char user[32];
    char pass[SSH_PASS_BUF_SIZE];
    bool allow_password_prompt;
} ssh_connect_ctx_t;

static TaskHandle_t ssh_connect_worker_handle = NULL;
static ssh_connect_ctx_t *ssh_connect_ctx_pending = NULL;

static void ssh_password_cb(const char *pass);
static bool ensure_ssh_connect_worker(void);
static void cmd_sshkey_handler(int argc, char **argv);

typedef enum {
    SSHKEY_PENDING_NONE = 0,
    SSHKEY_PENDING_IMPORT_VAULT_PASS,
    SSHKEY_PENDING_IMPORT_PRIVATE_KEY,
    SSHKEY_PENDING_IMPORT_KEY_PASSPHRASE,
    SSHKEY_PENDING_LOAD_VAULT_PASS,
} sshkey_pending_t;

static sshkey_pending_t s_sshkey_pending = SSHKEY_PENDING_NONE;
static char s_sshkey_vault_pass[SSH_PASS_BUF_SIZE] = {0};
static char s_sshkey_private_key[SSH_KEY_MAX_LEN] = {0};
static char s_sshkey_key_passphrase[SSH_PASS_BUF_SIZE] = {0};

static inline void shell_secure_zero(void *p, size_t len) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len--) *v++ = 0;
}

static void sshkey_reset_pending(void) {
    s_sshkey_pending = SSHKEY_PENDING_NONE;
    shell_secure_zero(s_sshkey_vault_pass, sizeof(s_sshkey_vault_pass));
    shell_secure_zero(s_sshkey_private_key, sizeof(s_sshkey_private_key));
    shell_secure_zero(s_sshkey_key_passphrase, sizeof(s_sshkey_key_passphrase));
    s_sshkey_vault_pass[0] = '\0';
    s_sshkey_private_key[0] = '\0';
    s_sshkey_key_passphrase[0] = '\0';
}

static void decode_escaped_newlines(const char *src, char *dst, size_t dst_cap) {
    if (!src || !dst || dst_cap == 0) return;
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < dst_cap; i++) {
        if (src[i] == '\\' && src[i + 1] == 'n') {
            dst[o++] = '\n';
            i++;
            continue;
        }
        if (src[i] == '\r') continue;
        dst[o++] = src[i];
    }
    dst[o] = '\0';
}

static void sshkey_input_cb(const char *input) {
    if (!input) {
        sshkey_reset_pending();
        return;
    }

    if (s_sshkey_pending == SSHKEY_PENDING_IMPORT_VAULT_PASS) {
        snprintf(s_sshkey_vault_pass, sizeof(s_sshkey_vault_pass), "%s", input);
        if (s_sshkey_vault_pass[0] == '\0') {
            shell_print("\r\n  vault password required");
            sshkey_reset_pending();
            return;
        }

        if (!secret_vault_password_is_set()) {
            esp_err_t err = secret_vault_password_set(s_sshkey_vault_pass);
            if (err != ESP_OK) {
                shell_print("\r\n  failed to set vault password");
                sshkey_reset_pending();
                return;
            }
        } else if (secret_vault_password_check(s_sshkey_vault_pass) != ESP_OK) {
            shell_print("\r\n  invalid vault password");
            sshkey_reset_pending();
            return;
        }

        s_sshkey_pending = SSHKEY_PENDING_IMPORT_PRIVATE_KEY;
        shell_print("\r\n  Private key PEM (use \\\\n for line breaks): ");
        shell_get_hidden_input(sshkey_input_cb);
        return;
    }

    if (s_sshkey_pending == SSHKEY_PENDING_IMPORT_PRIVATE_KEY) {
        decode_escaped_newlines(input, s_sshkey_private_key, sizeof(s_sshkey_private_key));
        if (strstr(s_sshkey_private_key, "-----BEGIN") == NULL) {
            shell_print("\r\n  invalid key format (expected PEM with BEGIN line)");
            sshkey_reset_pending();
            return;
        }

        s_sshkey_pending = SSHKEY_PENDING_IMPORT_KEY_PASSPHRASE;
        shell_print("\r\n  Key passphrase (empty for none): ");
        shell_get_hidden_input(sshkey_input_cb);
        return;
    }

    if (s_sshkey_pending == SSHKEY_PENDING_IMPORT_KEY_PASSPHRASE) {
        snprintf(s_sshkey_key_passphrase, sizeof(s_sshkey_key_passphrase), "%s", input);

        if (!ssh_set_private_key_pem(s_sshkey_private_key, s_sshkey_key_passphrase)) {
            shell_print("\r\n  failed to load key in runtime");
            sshkey_reset_pending();
            return;
        }

        esp_err_t err = secret_vault_store(SSH_VAULT_KEY_PRIVATE,
                                           s_sshkey_private_key,
                                           s_sshkey_vault_pass);
        if (err != ESP_OK) {
            shell_print("\r\n  failed to store private key in vault");
            sshkey_reset_pending();
            return;
        }

        if (s_sshkey_key_passphrase[0]) {
            err = secret_vault_store(SSH_VAULT_KEY_PASSPHRASE,
                                     s_sshkey_key_passphrase,
                                     s_sshkey_vault_pass);
            if (err != ESP_OK) {
                shell_print("\r\n  key stored, but failed to store key passphrase");
                sshkey_reset_pending();
                return;
            }
        } else {
            secret_vault_remove(SSH_VAULT_KEY_PASSPHRASE);
        }

        shell_print("\r\n  SSH key imported and loaded");
        sshkey_reset_pending();
        return;
    }

    if (s_sshkey_pending == SSHKEY_PENDING_LOAD_VAULT_PASS) {
        char key_buf[SSH_KEY_MAX_LEN] = {0};
        char key_pass[SSH_PASS_BUF_SIZE] = {0};
        snprintf(s_sshkey_vault_pass, sizeof(s_sshkey_vault_pass), "%s", input);
        if (s_sshkey_vault_pass[0] == '\0') {
            shell_print("\r\n  vault password required");
            sshkey_reset_pending();
            return;
        }

        esp_err_t err = secret_vault_load(SSH_VAULT_KEY_PRIVATE,
                                          s_sshkey_vault_pass,
                                          key_buf,
                                          sizeof(key_buf));
        if (err != ESP_OK) {
            shell_print("\r\n  invalid vault password or missing ssh key");
            shell_secure_zero(key_buf, sizeof(key_buf));
            sshkey_reset_pending();
            return;
        }

        err = secret_vault_load(SSH_VAULT_KEY_PASSPHRASE,
                                s_sshkey_vault_pass,
                                key_pass,
                                sizeof(key_pass));
        if (err != ESP_OK) {
            key_pass[0] = '\0';
        }

        if (!ssh_set_private_key_pem(key_buf, key_pass)) {
            shell_print("\r\n  failed to load key from vault");
            shell_secure_zero(key_buf, sizeof(key_buf));
            shell_secure_zero(key_pass, sizeof(key_pass));
            sshkey_reset_pending();
            return;
        }

        shell_print("\r\n  SSH key loaded from vault");
        shell_secure_zero(key_buf, sizeof(key_buf));
        shell_secure_zero(key_pass, sizeof(key_pass));
        sshkey_reset_pending();
        return;
    }

    sshkey_reset_pending();
}

static bool queue_ssh_connect(const char *pass, bool allow_password_prompt) {
    if (ssh_connect_pending) {
        shell_print("\r\n  SSH connect already in progress");
        return false;
    }

    ssh_connect_ctx_t *ctx = (ssh_connect_ctx_t *)malloc(sizeof(ssh_connect_ctx_t));
    if (!ctx) {
        shell_print("\r\n  SSH connect alloc failed");
        return false;
    }

    snprintf(ctx->host, sizeof(ctx->host), "%s", ssh_host);
    ctx->port = ssh_port;
    snprintf(ctx->user, sizeof(ctx->user), "%s", ssh_user);
    snprintf(ctx->pass, sizeof(ctx->pass), "%s", pass ? pass : "");
    ctx->allow_password_prompt = allow_password_prompt;

    ssh_connect_pending = true;
    shell_print("\r\n  Connecting...");

    if (!ensure_ssh_connect_worker()) {
        ssh_connect_pending = false;
        shell_print("\r\n  SSH worker task create failed");
        free(ctx);
        return false;
    }

    ssh_connect_ctx_pending = ctx;
    xTaskNotifyGive(ssh_connect_worker_handle);
    return true;
}

static void ssh_connect_worker_task(void *param) {
    (void)param;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ssh_connect_ctx_t *ctx = ssh_connect_ctx_pending;
        ssh_connect_ctx_pending = NULL;
        if (!ctx) {
            ssh_connect_pending = false;
            continue;
        }

        bool ok = ssh_connect(ctx->host, ctx->port, ctx->user, ctx->pass);
        if (ok) {
            shell_set_ssh_active(true);
            shell_print("\033[2J\033[H");
        } else if (ctx->allow_password_prompt && ssh_last_connect_requires_password()) {
            shell_print("\r\n  Password: ");
            shell_get_hidden_input(ssh_password_cb);
        } else {
            const char *why = ssh_last_connect_error();
            if (why && why[0]) {
                shell_print("\r\n  SSH connection failed: ");
                shell_print(why);
            } else {
                shell_print("\r\n  SSH connection failed");
            }
            if (!ssh_active) prompt(true);
        }
        ssh_connect_pending = false;
        free(ctx);
    }
}

static bool ensure_ssh_connect_worker(void) {
    if (ssh_connect_worker_handle) return true;

    /*
     * SSH connect path may persist trust records (NVS flash writes).
     * Keep this task stack in internal RAM so cache-disabled flash sections
     * do not trip esp_task_stack_is_sane_cache_disabled().
     */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        ssh_connect_worker_task,
        "ssh_connect",
        16384,
        NULL,
        5,
        &ssh_connect_worker_handle,
        1,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ret = xTaskCreatePinnedToCoreWithCaps(
            ssh_connect_worker_task,
            "ssh_connect",
            12288,
            NULL,
            5,
            &ssh_connect_worker_handle,
            1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ret == pdPASS;
}

static void ssh_password_cb(const char *pass) {
    queue_ssh_connect(pass, false);
}

static void cmd_sshkey_handler(int argc, char **argv) {
    if (argc < 2) {
        shell_print("\r\n  usage: sshkey status|import|load|clear|erase");
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        shell_print(ssh_has_private_key()
                        ? "\r\n  runtime key: loaded"
                        : "\r\n  runtime key: not loaded");
        shell_print(secret_vault_exists(SSH_VAULT_KEY_PRIVATE)
                        ? "\r\n  vault key: present"
                        : "\r\n  vault key: missing");
        return;
    }

    if (strcmp(argv[1], "import") == 0) {
        if (s_sshkey_pending != SSHKEY_PENDING_NONE) {
            shell_print("\r\n  sshkey flow already in progress");
            return;
        }
        s_sshkey_pending = SSHKEY_PENDING_IMPORT_VAULT_PASS;
        shell_print("\r\n  Vault password: ");
        shell_get_hidden_input(sshkey_input_cb);
        return;
    }

    if (strcmp(argv[1], "load") == 0) {
        if (!secret_vault_exists(SSH_VAULT_KEY_PRIVATE)) {
            shell_print("\r\n  no ssh key found in vault");
            return;
        }
        if (s_sshkey_pending != SSHKEY_PENDING_NONE) {
            shell_print("\r\n  sshkey flow already in progress");
            return;
        }
        s_sshkey_pending = SSHKEY_PENDING_LOAD_VAULT_PASS;
        shell_print("\r\n  Vault password: ");
        shell_get_hidden_input(sshkey_input_cb);
        return;
    }

    if (strcmp(argv[1], "clear") == 0) {
        ssh_clear_private_key();
        shell_print("\r\n  runtime ssh key cleared");
        return;
    }

    if (strcmp(argv[1], "erase") == 0) {
        ssh_clear_private_key();
        secret_vault_remove(SSH_VAULT_KEY_PRIVATE);
        secret_vault_remove(SSH_VAULT_KEY_PASSPHRASE);
        shell_print("\r\n  ssh key erased from runtime and vault");
        return;
    }

    shell_print("\r\n  usage: sshkey status|import|load|clear|erase");
}

static void cmd_ssh_handler(int argc, char **argv) {
    if (argc < 2) {
        shell_print("\r\n  Usage: ssh [user@]host[:port]");
        return;
    }

    if (!wifi_mgr_is_connected()) {
        shell_print("\r\n  wifi disconnected");
        return;
    }

    const char *arg = argv[1];
    const char *at = strchr(arg, '@');
    const char *host_part = arg;

    if (at) {
        int user_len = at - arg;
        if (user_len <= 0) {
            shell_print("\r\n  Invalid SSH target");
            return;
        }
        if (user_len >= (int)sizeof(ssh_user)) user_len = sizeof(ssh_user) - 1;
        memcpy(ssh_user, arg, user_len);
        ssh_user[user_len] = '\0';
        host_part = at + 1;
    } else if (ssh_user[0] == '\0') {
        snprintf(ssh_user, sizeof(ssh_user), "%s", "root");
    }

    if (host_part[0] == '\0') {
        shell_print("\r\n  Invalid SSH target");
        return;
    }

    const char *colon = strchr(host_part, ':');
    if (colon) {
        int host_len = colon - host_part;
        if (host_len <= 0) {
            shell_print("\r\n  Invalid SSH host");
            return;
        }
        if (host_len >= (int)sizeof(ssh_host)) host_len = sizeof(ssh_host) - 1;
        memcpy(ssh_host, host_part, host_len);
        ssh_host[host_len] = '\0';

        const char *port_str = colon + 1;
        if (port_str[0] == '\0') {
            shell_print("\r\n  Invalid SSH port");
            return;
        }
        char *end = NULL;
        long parsed_port = strtol(port_str, &end, 10);
        if (*end != '\0' || parsed_port <= 0 || parsed_port > 65535) {
            shell_print("\r\n  Invalid SSH port");
            return;
        }
        ssh_port = (uint16_t)parsed_port;
    } else {
        snprintf(ssh_host, sizeof(ssh_host), "%s", host_part);
        ssh_port = 22;
    }

    queue_ssh_connect("", true);
}

// Parses "host" or "host:port" for sshknown subcommands. Defaults port to 22.
static bool parse_host_port(const char *arg, char *host_out, size_t host_cap, uint16_t *port_out) {
    if (!arg || !arg[0] || !host_out || host_cap == 0 || !port_out) return false;
    const char *colon = strrchr(arg, ':');
    if (colon) {
        size_t host_len = (size_t)(colon - arg);
        if (host_len == 0 || host_len >= host_cap) return false;
        const char *port_str = colon + 1;
        char *end = NULL;
        long p = strtol(port_str, &end, 10);
        if (*end != '\0' || p <= 0 || p > 65535) return false;
        memcpy(host_out, arg, host_len);
        host_out[host_len] = '\0';
        *port_out = (uint16_t)p;
    } else {
        snprintf(host_out, host_cap, "%s", arg);
        *port_out = 22;
    }
    return true;
}

static void sshknown_list_cb(const char *host_port,
                             const char *type,
                             const char *fingerprint,
                             void *ctx) {
    int *n = (int *)ctx;
    if (n) (*n)++;
    char line[160];
    snprintf(line, sizeof(line), "\r\n  %-28s %-22s %s",
             host_port ? host_port : "?",
             type ? type : "?",
             fingerprint ? fingerprint : "?");
    shell_print(line);
}

static void cmd_sshknown_handler(int argc, char **argv) {
    if (argc < 2) {
        shell_print("\r\n  usage: sshknown list|remove <host[:port]>|trust <host[:port]>|clear");
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        int n = 0;
        ssh_known_hosts_foreach(sshknown_list_cb, &n);
        if (n == 0) {
            shell_print("\r\n  no known hosts stored");
        }
        return;
    }

    // "remove" and "trust" both delete the pinned record. After removal the
    // next connection re-pins (TOFU) the server's current key, which is how a
    // user accepts a legitimately changed host key.
    if (strcmp(argv[1], "remove") == 0 || strcmp(argv[1], "trust") == 0) {
        if (argc < 3) {
            shell_print("\r\n  usage: sshknown remove <host[:port]>");
            return;
        }
        char host[80] = {0};
        uint16_t port = 22;
        if (!parse_host_port(argv[2], host, sizeof(host), &port)) {
            shell_print("\r\n  invalid host[:port]");
            return;
        }
        bool removed = ssh_known_host_remove(host, port);
        if (removed) {
            char line[128];
            if (strcmp(argv[1], "trust") == 0) {
                snprintf(line, sizeof(line),
                         "\r\n  trust reset for %s:%u (next connect re-pins key)",
                         host, (unsigned)port);
            } else {
                snprintf(line, sizeof(line), "\r\n  removed known host %s:%u",
                         host, (unsigned)port);
            }
            shell_print(line);
        } else {
            shell_print("\r\n  no matching known host record");
        }
        return;
    }

    if (strcmp(argv[1], "clear") == 0) {
        int n = ssh_known_hosts_clear();
        char line[64];
        snprintf(line, sizeof(line), "\r\n  cleared %d known host record(s)", n);
        shell_print(line);
        return;
    }

    shell_print("\r\n  usage: sshknown list|remove <host[:port]>|trust <host[:port]>|clear");
}

static bool parse_mac_string(const char *text, uint8_t mac[6]) {
    if (!text || !mac) return false;
    unsigned int b[6] = {0};
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) return false;
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

static void print_mac_line(const uint8_t mac[6]) {
    char line[48];
    snprintf(line,
             sizeof(line),
             "\r\n  %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
    shell_print(line);
}

static void cmd_mac_handler(int argc, char **argv) {
    if (argc == 1) {
        uint8_t mac[6] = {0};
        esp_err_t err = wifi_mgr_get_mac(mac);
        if (err != ESP_OK) {
            shell_print("\r\n  mac unavailable");
            return;
        }
        print_mac_line(mac);
        return;
    }

    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        uint8_t mac[6] = {0};
        if (!parse_mac_string(argv[2], mac)) {
            shell_print("\r\n  invalid mac format (use XX:XX:XX:XX:XX:XX)");
            return;
        }
        esp_err_t err = wifi_mgr_set_mac(mac);
        if (err != ESP_OK) {
            shell_print("\r\n  failed to set mac (must be unicast, non-zero)");
            return;
        }
        shell_print("\r\n  mac set:");
        print_mac_line(mac);
        return;
    }

    shell_print("\r\n  usage: mac | mac set <xx:xx:xx:xx:xx:xx>");
}

void shell_init(terminal_t *t) {
    term = t;

    input_buf[0] = '\0';
    input_len = 0;
    input_cursor = 0;
    rendered_input_len = 0;
    history_count = 0;
    history_idx = -1;
    input_callback = NULL;

    shell_register_cmd("help", cmd_help_handler);
    shell_register_cmd("clear", cmd_clear_handler);
    shell_register_cmd("wifi", cmd_wifi);
    shell_register_cmd("ssh", cmd_ssh_handler);
    shell_register_cmd("sshkey", cmd_sshkey_handler);
    shell_register_cmd("sshknown", cmd_sshknown_handler);
    shell_register_cmd("tailscale", cmd_tailscale);
    shell_register_cmd("hostname", cmd_hostname);
    shell_register_cmd("mac", cmd_mac_handler);
    shell_register_cmd("vault", cmd_vault);
    shell_register_cmd("about", cmd_about_handler);

    terminal_write(term, "$ ", -1);
}
