/**
 * @file time_manager.h
 * @brief Simplified UTC Time Management System Header - WiFi Only Version
 *
 * Provides UTC time synchronization via SNTP with NVS persistence.
 * All timestamps are in UTC format with 'Z' suffix.
 * 
 * WiFi Only - GSM support removed for simplicity
 */
/*
#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t time_manager_init(void);

esp_err_t time_manager_notify_wifi_status(bool connected);


esp_err_t time_manager_get_timestamp(char *timestamp_out, size_t max_len);

bool time_manager_is_synced(void);

esp_err_t time_manager_wait_sync(uint32_t timeout_ms);

esp_err_t time_manager_wait_for_sync_completion(uint32_t timeout_ms);

esp_err_t time_manager_get_epoch(time_t *epoch_out);

esp_err_t time_manager_ensure_initialized(void);

esp_err_t time_manager_force_sync(void);

#ifdef __cplusplus
}
#endif

#endif // TIME_MANAGER_H
*/
//----------------------------DEEP
/**
 * @file time_manager.h
 * @brief Simplified UTC Time Management System Header
 *
 * Provides UTC time synchronization via SNTP with NVS persistence.
 * All timestamps are in UTC format with 'Z' suffix.
 */

#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Network type for time synchronization
 */
typedef enum {
    TIME_NET_NONE = 0,
    TIME_NET_WIFI,
    TIME_NET_GSM
} time_network_status_t;

/**
 * @brief Initialize time manager
 *
 * Sets up mutexes, event groups, and restores last known time from NVS.
 * Time is always in UTC.
 *
 * @return ESP_OK on success
 */
esp_err_t time_manager_init(void);

/**
 * @brief Notify time manager of network status change
 *
 * Call this when network connects/disconnects to trigger SNTP sync.
 *
 * @param connected true if network is connected
 * @param network_type Type of network (WiFi or GSM)
 * @return ESP_OK on success
 */
esp_err_t time_manager_notify_network(bool connected, time_network_status_t network_type);

/**
 * @brief Get formatted UTC timestamp for MQTT/alerts
 *
 * Format: "D:DD-MM-YYYY&T:HH:MM:SSZ"
 *
 * @param timestamp_out Output buffer for timestamp string
 * @param max_len Maximum length of output buffer (must be >= 32)
 * @return ESP_OK on success, ESP_FAIL if time not synced
 */
esp_err_t time_manager_get_timestamp(char *timestamp_out, size_t max_len);

/**
 * @brief Check if time is synchronized
 *
 * @return true if time has been synced via SNTP
 */
bool time_manager_is_synced(void);

/**
 * @brief Wait for time synchronization (blocking)
 *
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return ESP_OK if synced, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t time_manager_wait_sync(uint32_t timeout_ms);

/**
 * @brief Wait for sync completion using event group (for state machine)
 *
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return ESP_OK if complete, ESP_FAIL if failed, ESP_ERR_TIMEOUT if still in progress
 */
esp_err_t time_manager_wait_for_sync_completion(uint32_t timeout_ms);

/**
 * @brief Get current UTC epoch time
 *
 * @param epoch_out Pointer to store epoch value
 * @return ESP_OK on success
 */
esp_err_t time_manager_get_epoch(time_t *epoch_out);

/**
 * @brief Ensure time manager mutex is initialized
 *
 * Call before any other time_manager functions if init may not have been called.
 *
 * @return ESP_OK on success
 */
esp_err_t time_manager_ensure_initialized(void);

/**
 * @brief Force an immediate time synchronization
 *
 * Triggers the sync task to perform SNTP sync.
 *
 * @return ESP_OK if sync triggered, ESP_FAIL if network unavailable
 */
esp_err_t time_manager_force_sync(void);

#ifdef __cplusplus
}
#endif

#endif // TIME_MANAGER_H
