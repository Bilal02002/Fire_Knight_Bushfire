/**
 * @file spiffs_handler.h
 * @brief SPIFFS file system operations for credential and data storage
 */

#ifndef SPIFFS_HANDLER_H
#define SPIFFS_HANDLER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

// File paths
#define SPIFFS_CERT_PATH "/spiffs/device_cert.pem"
#define SPIFFS_KEY_PATH "/spiffs/device_key.pem"
#define SPIFFS_THING_NAME_PATH "/spiffs/thing_name.txt"
#define SPIFFS_WIFI_CREDS_PATH "/spiffs/wifi_creds.json"
#define SPIFFS_ALERTS_PATH "/spiffs/pending_alerts.json"

// Alert storage constants
#define MAX_ALERTS_IN_STORAGE 50
#define MAX_ALERT_SIZE 512
#define MAX_ALERT_RETRIES 3

/**
 * @brief Initialize SPIFFS file system
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_init(void);

/**
 * @brief Deinitialize SPIFFS file system
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_deinit(void);

/**
 * @brief Check if SPIFFS is initialized
 * @return true if initialized, false otherwise
 */
bool spiffs_is_initialized(void);

/**
 * @brief Store AWS IoT credentials in SPIFFS
 * @param cert_pem Certificate PEM data
 * @param private_key Private key PEM data
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_store_credentials(const char *cert_pem, const char *private_key);

/**
 * @brief Read file from SPIFFS
 * @param path File path
 * @param buffer Pointer to buffer that will be allocated
 * @param size Pointer to store file size
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_read_file(const char *path, char **buffer, size_t *size);

/**
 * @brief Check if AWS credentials exist in SPIFFS
 * @return true if credentials exist, false otherwise
 */
bool spiffs_credentials_exist(void);

/**
 * @brief Get SPIFFS filesystem information
 * @param total_bytes Total bytes in filesystem (output)
 * @param used_bytes Used bytes in filesystem (output)
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_get_info(size_t *total_bytes, size_t *used_bytes);

/**
 * @brief Delete file from SPIFFS
 * @param path File path
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_delete_file(const char *path);

/**
 * @brief Store thing name in SPIFFS
 * @param thing_name Thing name to store
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_store_thing_name(const char *thing_name);

/**
 * @brief Read thing name from SPIFFS
 * @param thing_name Buffer to store thing name
 * @param max_len Maximum length of buffer
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_read_thing_name(char *thing_name, size_t max_len);

/**
 * @brief Check if thing name exists in SPIFFS
 * @return true if thing name exists, false otherwise
 */
bool spiffs_thing_name_exists(void);

/**
 * @brief Check if file exists in SPIFFS
 * @param path File path
 * @return true if file exists, false otherwise
 */
bool spiffs_file_exists(const char *path);

/**
 * @brief Validate AWS IoT credentials in SPIFFS
 * @return esp_err_t ESP_OK if valid, error code otherwise
 */
esp_err_t spiffs_validate_credentials(void);

/**
 * @brief Delete all provisioning data from SPIFFS
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_clean_provisioning_data(void);

/**
 * @brief Store WiFi credentials in SPIFFS
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_store_wifi_credentials(const char *ssid, const char *password);

/**
 * @brief Load WiFi credentials from SPIFFS
 * @param ssid Buffer to store SSID
 * @param password Buffer to store password
 * @param ssid_size Size of SSID buffer
 * @param password_size Size of password buffer
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_load_wifi_credentials(char *ssid, char *password, 
                                       size_t ssid_size, size_t password_size);

/**
 * @brief Check if WiFi credentials exist in SPIFFS
 * @return true if WiFi credentials exist, false otherwise
 */
bool spiffs_wifi_credentials_exist(void);

/**
 * @brief Delete WiFi credentials from SPIFFS
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_clean_wifi_credentials(void);

// ========================================
// ALERT STORAGE FUNCTIONS
// ========================================

/**
 * @brief Store an alert in SPIFFS for later transmission
 * @param topic MQTT topic for the alert
 * @param payload JSON payload of the alert
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_store_alert(const char* topic, const char* payload);

/**
 * @brief Read all pending alerts from SPIFFS
 * @return cJSON* Array of pending alerts (must be freed with cJSON_Delete)
 */
cJSON* spiffs_read_pending_alerts(void);

/**
 * @brief Remove sent alerts from storage
 * @param sent_indices Array of indices to remove (cJSON array)
 * @param count Number of indices to remove
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_remove_sent_alerts(cJSON *sent_indices, int count);

/**
 * @brief Clear all pending alerts from storage
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_clear_all_alerts(void);

/**
 * @brief Get count of pending alerts
 * @return int Number of pending alerts
 */
int spiffs_get_pending_alert_count(void);

/**
 * @brief Clean alert data (alias for clear_all_alerts)
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_clean_alert_data(void);

/**
 * @brief Get oldest pending alert
 * @param topic Buffer to store topic (must be at least 128 bytes)
 * @param payload Buffer to store payload (must be at least MAX_ALERT_SIZE bytes)
 * @return int Alert index if found, -1 if no alerts
 */
int spiffs_get_oldest_alert(char *topic, char *payload);

/**
 * @brief Check if any alert has exceeded max retries
 * @return true If any alert should be discarded
 * @return false If all alerts are still valid
 */
bool spiffs_should_discard_old_alerts(void);

/**
 * @brief Print summary of pending alerts
 */
void spiffs_print_alert_summary(void);

/**
 * @brief Increment retry count for a specific alert
 * @param alert_index Index of the alert to update
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t spiffs_increment_alert_retry(int alert_index);
const char* get_custom_timestamp(void);
#endif /* SPIFFS_HANDLER_H */