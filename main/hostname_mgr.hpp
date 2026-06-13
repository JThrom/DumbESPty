#ifndef HOSTNAME_MGR_HPP
#define HOSTNAME_MGR_HPP

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hostname_mgr_init(void);
const char *hostname_mgr_get(void);
esp_err_t hostname_mgr_set(const char *hostname);
void cmd_hostname(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
