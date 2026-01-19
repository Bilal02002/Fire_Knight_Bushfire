#ifndef GSM_MANAGER_H
#define GSM_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
// Network types
typedef enum {
    NETWORK_NONE = 0,
    NETWORK_WIFI,
    NETWORK_GSM
} network_type_t;




// Global variables
extern bool gsm_active;
extern bool gsm_connected;
extern network_type_t active_network;

// Public functions
esp_err_t gsm_manager_init(void);
esp_err_t gsm_manager_connect(void);
void gsm_manager_disconnect(void);
void gsm_manager_deinit(void);
bool gsm_manager_is_connected(void);
int gsm_manager_get_signal_quality(void);

#endif // GSM_MANAGER_H
