#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// ========================================
// WIFI CONFIGURATION
// ========================================
#define WIFI_SSID       "Pixel"
#define WIFI_PASSWORD   "123456788"

// Connection settings
#define WIFI_TIMEOUT_MS 20000  // 20 seconds timeout
#define WIFI_RETRY_DELAY 5000  // 5 seconds between retries

// Event group bits for WiFi status
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// ========================================
// TYPE DEFINITIONS
// ========================================

// WiFi credentials structure for shadow control
typedef struct {
    char ssid[32];
    char password[64];
    bool custom_configured;
    bool pending_update;
} WiFiShadowConfig;

// ========================================
// FUNCTION DECLARATIONS
// ========================================

// Basic WiFi functions
void init_wifi(void);
void maintain_wifi_connection(void);
bool is_wifi_connected(void);
int get_wifi_rssi(void);
void get_wifi_ip_address(char* ip_buffer, size_t buffer_size);
void print_wifi_status(void);
void wifi_disconnect(void);
bool wifi_reconnect(void);
const char* get_ip_address(void);
// Shadow-controlled WiFi credentials management
void set_wifi_credentials(const char* ssid, const char* password);
bool wifi_apply_new_credentials(void);
void wifi_save_credentials_to_spiffs(void);
bool wifi_has_custom_credentials(void);
const char* get_current_wifi_ssid(void);
const char* get_current_wifi_password(void);
bool load_wifi_credentials_from_spiffs(void);
void wifi_reset_to_default(void);

// NEW: Added function to check pending update status
bool wifi_has_pending_update(void);

// WiFi status getters for shadow reporting
bool get_wifi_connection_status(void);
int get_wifi_signal_strength(void);

// WiFi configuration helpers
bool validate_wifi_credentials(const char* ssid, const char* password);
void print_wifi_configuration(void);

// NEW: Added getter for WiFi configuration struct
WiFiShadowConfig* get_wifi_shadow_config(void);

#endif // WIFI_CONFIG_H