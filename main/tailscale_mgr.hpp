#ifndef TAILSCALE_MGR_HPP
#define TAILSCALE_MGR_HPP

#include <cstddef>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TS_BACKEND_SAAS = 0,
    TS_BACKEND_HEADSCALE = 1,
} tailscale_backend_t;

esp_err_t tailscale_mgr_init(void);
void tailscale_mgr_process_queue(void);

bool tailscale_mgr_is_enabled(void);
bool tailscale_mgr_is_connected(void);
bool tailscale_mgr_is_connecting(void);
const char *tailscale_mgr_backend_name(void);
uint32_t tailscale_mgr_get_vpn_ip(void);
void tailscale_mgr_get_status_line(char *buf, size_t len);
// Short status line for the on-screen status menu (fits in ~30 chars).
void tailscale_mgr_get_status_short(char *buf, size_t len);

void cmd_tailscale(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
