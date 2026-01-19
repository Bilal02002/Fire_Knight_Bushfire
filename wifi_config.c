#include "wifi_config.h"
#include "spiffs_handler.h"  // For SPIFFS storage
#include <stdio.h>
#include <string.h>

#define LOG_BUFFER_SIZE 256

// ========================================
// GLOBAL VARIABLES
// ========================================
static unsigned long lastReconnectAttempt = 0;
static EventGroupHandle_t wifi_event_group;
static int s_retry_num = 0;
static bool wifi_connected = false;

// Shadow-controlled WiFi configuration
static WiFiShadowConfig wifi_shadow_config = {
    .ssid = "",
    .password = "",
    .custom_configured = false,
    .pending_update = false
};

// ========================================
// HELPER FUNCTIONS
// ========================================

/**
 * @brief Get pointer to WiFi shadow config (for main.c access)
 */
WiFiShadowConfig* get_wifi_shadow_config(void) {
    return &wifi_shadow_config;
}

/**
 * @brief Check if WiFi has pending update
 */
bool wifi_has_pending_update(void) {
    return wifi_shadow_config.pending_update;
}

/**
 * @brief Validate WiFi credentials
 */
bool validate_wifi_credentials(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 31) {
        printf("[WIFI] Invalid SSID length\n");
        return false;
    }
    
    if (!password || strlen(password) == 0 || strlen(password) > 63) {
        printf("[WIFI] Invalid password length\n");
        return false;
    }
    
    // Check for special characters that might cause issues
    for (int i = 0; ssid[i]; i++) {
        if (ssid[i] < 32 || ssid[i] > 126) {
            printf("[WIFI] SSID contains invalid characters\n");
            return false;
        }
    }
    
    return true;
}

/**
 * @brief Print current WiFi configuration
 */
void print_wifi_configuration(void) {
    printf("\n=== WIFI CONFIGURATION ===\n");
    printf("Current Mode: %s\n", 
           wifi_shadow_config.custom_configured ? "SHADOW-CONTROLLED" : "DEFAULT");
    printf("SSID: %s\n", get_current_wifi_ssid());
    // Show password in plain text - Security disabled as requested
    if (wifi_shadow_config.custom_configured) {
        printf("Password: %s\n", wifi_shadow_config.password);
    } else {
        printf("Password: %s (default)\n", WIFI_PASSWORD);
    }
    printf("Custom Configured: %s\n", 
           wifi_shadow_config.custom_configured ? "YES" : "NO");
    printf("Pending Update: %s\n", 
           wifi_shadow_config.pending_update ? "YES" : "NO");
    printf("WiFi Connected: %s\n", 
           is_wifi_connected() ? "YES" : "NO");
    
    if (is_wifi_connected()) {
        char ip[16];
        get_wifi_ip_address(ip, sizeof(ip));
        printf("IP Address: %s\n", ip);
        printf("Signal Strength: %d dBm\n", get_wifi_rssi());
    }
    printf("===========================\n");
}

// ========================================
// WIFI EVENT HANDLER
// ========================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                              int32_t event_id, void* event_data) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf("[WIFI] WiFi station started\n");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        wifi_event_sta_disconnected_t* disconnected_event = (wifi_event_sta_disconnected_t*) event_data;
        
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] WiFi disconnected. Reason: %d", 
                disconnected_event->reason);
        printf("%s\n", log_msg);
        
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[WIFI] Retrying to connect (attempt %d)", s_retry_num);
            printf("%s\n", log_msg);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[WIFI] Failed to connect after %d attempts", s_retry_num);
            printf("%s\n", log_msg);
            s_retry_num = 0; // Reset for next attempt
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        printf("[WIFI] Connected to AP\n");
    }
}

// ========================================
// IP EVENT HANDLER
// ========================================
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] Got IP: " IPSTR, 
                IP2STR(&event->ip_info.ip));
        printf("%s\n", log_msg);
        
        s_retry_num = 0;
        wifi_connected = true;
        wifi_shadow_config.pending_update = false; // Clear pending flag on successful connection
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ========================================
// WIFI DISCONNECT
// ========================================
void wifi_disconnect(void) {
    printf("[WIFI] Disconnecting WiFi...\n");
    wifi_connected = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ========================================
// WIFI RECONNECT
// ========================================
void wifi_reconnect(void) {
    printf("[WIFI] Reconnecting WiFi...\n");
    
    // Reset retry counter
    s_retry_num = 0;
    
    // Disconnect first
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Then connect
    esp_wifi_connect();
    
    printf("[WIFI] WiFi reconnection initiated\n");
}

// ========================================
// SHADOW-CONTROLLED WIFI FUNCTIONS
// ========================================

/**
 * @brief Get current WiFi SSID (shadow or default)
 */
const char* get_current_wifi_ssid(void) {
    if (wifi_has_custom_credentials()) {
        return wifi_shadow_config.ssid;
    }
    return WIFI_SSID;  // Default from header
}

/**
 * @brief Get current WiFi password (shadow or default)
 */
const char* get_current_wifi_password(void) {
    if (wifi_shadow_config.custom_configured && 
        strlen(wifi_shadow_config.password) > 0) {
        return wifi_shadow_config.password;
    }
    return WIFI_PASSWORD;  // Return default if no custom
}

/**
 * @brief Set WiFi credentials from shadow
 */
void set_wifi_credentials(const char* ssid, const char* password) {
    if (!validate_wifi_credentials(ssid, password)) {
        printf("[WIFI-SHADOW] Invalid credentials\n");
        return;
    }
    
    // Check if credentials are different
    bool credentials_changed = false;
    
    if (strcmp(wifi_shadow_config.ssid, ssid) != 0) {
        credentials_changed = true;
    } else if (!wifi_shadow_config.custom_configured) {
        credentials_changed = true;
    }
    
    if (!credentials_changed) {
        printf("[WIFI-SHADOW] Credentials unchanged, skipping update\n");
        return;
    }
    
    // Store new credentials
    strncpy(wifi_shadow_config.ssid, ssid, sizeof(wifi_shadow_config.ssid) - 1);
    wifi_shadow_config.ssid[sizeof(wifi_shadow_config.ssid) - 1] = '\0';
    
    strncpy(wifi_shadow_config.password, password, sizeof(wifi_shadow_config.password) - 1);
    wifi_shadow_config.password[sizeof(wifi_shadow_config.password) - 1] = '\0';
    
    wifi_shadow_config.custom_configured = true;
    wifi_shadow_config.pending_update = true;  // Mark as pending
    
    // Save to SPIFFS for persistence
    wifi_save_credentials_to_spiffs();
    
    // DO NOT APPLY IMMEDIATELY - Wait for reset
    printf("[WIFI-SHADOW] Credentials saved. Device needs reset to use new WiFi.\n");
}

/**
 * @brief Apply new WiFi credentials (only for immediate application if needed)
 */
bool wifi_apply_new_credentials(void) {
    if (!wifi_shadow_config.custom_configured || strlen(wifi_shadow_config.ssid) == 0) {
        printf("[WIFI-SHADOW] No custom credentials to apply\n");
        return false;
    }
    
    printf("[WIFI-SHADOW] Applying new WiFi credentials: SSID='%s'\n", wifi_shadow_config.ssid);
    
    // Mark as pending
    wifi_shadow_config.pending_update = true;
    
    // Disconnect first
    wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Configure new WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    strncpy((char*)wifi_config.sta.ssid, wifi_shadow_config.ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, wifi_shadow_config.password, sizeof(wifi_config.sta.password));
    
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        printf("[WIFI-SHADOW] Failed to set new config: %s\n", esp_err_to_name(ret));
        wifi_shadow_config.pending_update = false;
        return false;
    }
    
    // Reconnect with new credentials
    wifi_reconnect();
    printf("[WIFI-SHADOW] WiFi credentials applied successfully\n");
    return true;
}

/**
 * @brief Save WiFi credentials to SPIFFS
 */
void wifi_save_credentials_to_spiffs(void) {
    if (!wifi_shadow_config.custom_configured) {
        printf("[WIFI-SHADOW] No custom credentials to save\n");
        return;
    }
    
    printf("\n[WIFI-SHADOW] Saving credentials to SPIFFS...\n");
    
    // Use spiffs_handler function
    esp_err_t ret = spiffs_store_wifi_credentials(wifi_shadow_config.ssid, wifi_shadow_config.password);
    
    if (ret == ESP_OK) {
        printf("[WIFI-SHADOW] Credentials saved to SPIFFS successfully\n");
    } else {
        printf("[WIFI-SHADOW] Failed to save credentials to SPIFFS\n");
    }
}

/**
 * @brief Load WiFi credentials from SPIFFS
 */
bool load_wifi_credentials_from_spiffs(void) {
    printf("[WIFI-SHADOW] Loading credentials from SPIFFS...\n");
    
    char loaded_ssid[32] = {0};
    char loaded_password[64] = {0};
    
    esp_err_t ret = spiffs_load_wifi_credentials(loaded_ssid, loaded_password, 
                                                sizeof(loaded_ssid), sizeof(loaded_password));
    
    if (ret == ESP_OK && strlen(loaded_ssid) > 0) {
        strncpy(wifi_shadow_config.ssid, loaded_ssid, sizeof(wifi_shadow_config.ssid) - 1);
        strncpy(wifi_shadow_config.password, loaded_password, sizeof(wifi_shadow_config.password) - 1);
        wifi_shadow_config.custom_configured = true;
        wifi_shadow_config.pending_update = false; // Clear pending flag on load
        
        printf("[WIFI-SHADOW] Credentials loaded from SPIFFS: SSID='%s'\n", wifi_shadow_config.ssid);
        return true;
    } else {
        printf("[WIFI-SHADOW] No credentials found in SPIFFS or load failed\n");
        return false;
    }
}

/**
 * @brief Check if custom WiFi credentials are configured
 */
bool wifi_has_custom_credentials(void) {
    return wifi_shadow_config.custom_configured && strlen(wifi_shadow_config.ssid) > 0;
}

/**
 * @brief Reset to default WiFi credentials
 */
void wifi_reset_to_default(void) {
    printf("[WIFI-SHADOW] Resetting to default WiFi credentials\n");
    
    wifi_shadow_config.custom_configured = false;
    wifi_shadow_config.ssid[0] = '\0';
    wifi_shadow_config.password[0] = '\0';
    wifi_shadow_config.pending_update = true;
    
    // Delete saved credentials from SPIFFS
    spiffs_delete_file("/spiffs/wifi_creds.json");
    
    // Reconnect with default credentials
    wifi_apply_new_credentials();
}

/**
 * @brief Get WiFi connection status for shadow reporting
 */
bool get_wifi_connection_status(void) {
    return wifi_connected;
}

/**
 * @brief Get WiFi signal strength
 */
int get_wifi_signal_strength(void) {
    return get_wifi_rssi();
}

// ========================================
// GET IP ADDRESS (string version)
// ========================================
const char* get_ip_address(void) {
    static char ip_str[16] = "0.0.0.0";
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        return ip_str;
    }
    return "No IP";
}

// ========================================
// WIFI INITIALIZATION
// ========================================
void init_wifi(void) {
    char log_msg[LOG_BUFFER_SIZE];
    
    
    // Determine which credentials to use
    bool use_custom = wifi_has_custom_credentials();
    const char* connect_ssid = use_custom ? wifi_shadow_config.ssid : WIFI_SSID;
    const char* connect_password = use_custom ? wifi_shadow_config.password : WIFI_PASSWORD;
    
    printf("[WIFI] ===== WIFI INITIALIZATION =====\n");
    printf("[WIFI] Mode: %s\n", use_custom ? "SHADOW-CONTROLLED" : "DEFAULT");
    printf("[WIFI] Connecting to: %s\n", connect_ssid);
    printf("[WIFI] Password: %s\n", connect_password);
    
    // Initialize TCP/IP stack
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] Network interface init failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
        return;
    }
    
    // Create event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] Event loop creation failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
        return;
    }
    
    // Create default station
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        printf("[WIFI] Failed to create default WiFi station\n");
        return;
    }

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] WiFi init failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
        return;
    }

    // Register event handlers
    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                             ESP_EVENT_ANY_ID,
                                             &wifi_event_handler,
                                             NULL,
                                             NULL);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] WiFi event handler registration failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                             IP_EVENT_STA_GOT_IP,
                                             &ip_event_handler,
                                             NULL,
                                             NULL);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] IP event handler registration failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
    }

    // Create event group
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        printf("[WIFI] Failed to create event group\n");
        return;
    }

    // Configure WiFi station
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // Set SSID and password based on configuration
    strncpy((char*)wifi_config.sta.ssid, connect_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, connect_password, sizeof(wifi_config.sta.password));
    
    // Set WiFi mode and config
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] Set WiFi mode failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
        return;
    }
    
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] Set WiFi config failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
        return;
    }
    
    // Start WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[WIFI] WiFi start failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
        return;
    }

    printf("[WIFI] WiFi initialization completed.\n");

    // Wait for connection with timeout
    printf("[WIFI] Connecting to Wi-Fi...\n");
    
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    // Print connection status
    wifi_ap_record_t ap_info;
    if (bits & WIFI_CONNECTED_BIT) {
        printf("[WIFI] Wi-Fi connected!\n");
        
        // Get RSSI
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[WIFI] Signal Strength: %d dBm", ap_info.rssi);
            printf("%s\n", log_msg);
        }
        
        // Get IP address
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[WIFI] IP Address: " IPSTR, IP2STR(&ip_info.ip));
            printf("%s\n", log_msg);
        }
    } else if (bits & WIFI_FAIL_BIT) {
        printf("[WIFI] Wi-Fi connection failed!\n");
        printf("[WIFI] System will continue attempting to connect...\n");
    } else {
        printf("[WIFI] Wi-Fi connection timeout!\n");
        printf("[WIFI] System will continue attempting to connect...\n");
    }
    
    printf("[WIFI] ==================================\n");
}

// ========================================
// MAINTAIN CONNECTION (Call in task)
// ========================================
void maintain_wifi_connection(void) {
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Check if disconnected and it's time to retry
    if (!is_wifi_connected() && 
        (now - lastReconnectAttempt >= WIFI_RETRY_DELAY)) {
        
        lastReconnectAttempt = now;
        reconnect_wifi();
    }
}

// ========================================
// RECONNECT WIFI
// ========================================
void reconnect_wifi(void) {
    char log_msg[LOG_BUFFER_SIZE];
    
    printf("[WIFI] WiFi disconnected. Attempting to reconnect...\n");
    
    // Disconnect and reconnect
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_connect();

    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        printf("[WIFI] WiFi reconnected!\n");
        
        // Get IP address
        esp_netif_ip_info_t ip_info;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[WIFI] IP Address: " IPSTR, IP2STR(&ip_info.ip));
            printf("%s\n", log_msg);
        }
    } else {
        printf("[WIFI] Reconnection failed. Will retry...\n");
    }
}

// ========================================
// CHECK WIFI STATUS
// ========================================
bool is_wifi_connected(void) {
    return wifi_connected;
}

// ========================================
// GET WIFI RSSI
// ========================================
int get_wifi_rssi(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

// ========================================
// GET WIFI IP ADDRESS
// ========================================
void get_wifi_ip_address(char* ip_buffer, size_t buffer_size) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_buffer, buffer_size, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(ip_buffer, buffer_size, "0.0.0.0");
    }
}

// ========================================
// PRINT WIFI STATUS
// ========================================
void print_wifi_status(void) {
    char log_msg[LOG_BUFFER_SIZE];
    char ip_address[16];
    
    get_wifi_ip_address(ip_address, sizeof(ip_address));
    int rssi = get_wifi_rssi();
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[WIFI] Status: %s, IP: %s, RSSI: %d dBm, SSID: %s",
            wifi_connected ? "CONNECTED" : "DISCONNECTED",
            ip_address, 
            rssi,
            get_current_wifi_ssid());
    printf("%s\n", log_msg);
}
