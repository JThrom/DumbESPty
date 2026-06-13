#ifndef SHELL_HPP
#define SHELL_HPP

#include "terminal.hpp"

#ifdef __cplusplus
extern "C" {
#endif

void shell_init(terminal_t *term);
void shell_handle_key(char c);
void shell_print(const char *text);
void shell_pump_ui(void);
void shell_register_cmd(const char *name, void (*handler)(int argc, char **argv));
void shell_get_input(void (*callback)(const char *input));
void shell_get_hidden_input(void (*callback)(const char *input));
void shell_set_ssh_active(bool active);
bool shell_is_ssh_active(void);

#ifdef __cplusplus
}
#endif

#endif
