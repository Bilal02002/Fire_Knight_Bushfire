/**
 * @file time_manager.c
 * @brief Simplified UTC Time Management System - WiFi Only Version
 *
 * Features:
 * - SNTP synchronization for accurate UTC time
 * - NVS caching for persistence across reboots
 * - WiFi network awareness
 * - All timestamps in UTC with 'Z' suffix
 *
 * Note: GSM support removed for simplicity - WiFi only
 */
/*
#include "time_manager.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"


// ==================== CONFIGURATION ====================

// NVS storage
#define TIMEZONE_NVS_NAMESPACE "time_mgr"
#define LASTTIME_NVS_KEY "last_time"

// Timing configuration
#define SNTP_RETRY_COUNT 15
#define SNTP_RETRY_DELAY_MS 2000
#define SYNC_TASK_STACK_SIZE 4096
#define SYNC_TASK_PRIORITY 5
#define SYNC_INTERVAL_SECONDS (60 * 60)  // 1 hour

// ==================== GLOBAL STATE ====================

// WiFi status
static bool wifi_connected = false;
static SemaphoreHandle_t network_mutex = NULL;

// Time sync state
static bool time_synced = false;
static SemaphoreHandle_t time_mutex = NULL;
static TaskHandle_t sync_task_handle = NULL;
static bool sync_task_created = false;

// Event group for sync status
static EventGroupHandle_t time_event_group = NULL;
#define TIME_EVENT_SYNC_STARTED  BIT0
#define TIME_EVENT_SYNC_COMPLETE BIT1
#define TIME_EVENT_SYNC_FAILED   BIT2

// ==================== FORWARD DECLARATIONS ====================

static esp_err_t init_sntp_and_sync(void);
static void save_epoch_to_nvs(time_t epoch);
static esp_err_t read_epoch_from_nvs(time_t *epoch_out);
static void time_sync_task(void *arg);
static bool is_wifi_available(void);

// ==================== UTILITY FUNCTIONS ====================


static bool is_wifi_available(void)
{
    bool available = false;

    if (network_mutex && xSemaphoreTake(network_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        available = wifi_connected;
        xSemaphoreGive(network_mutex);
    }

    return available;
}


static esp_err_t ensure_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

// ==================== PUBLIC API ====================


esp_err_t time_manager_init(void)
{
    esp_err_t err = ensure_nvs_init();
    if (err != ESP_OK) {
        printf("\n[TIME] NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create mutexes
    if (time_mutex == NULL) {
        time_mutex = xSemaphoreCreateMutex();
        if (time_mutex == NULL) {
            printf("\n[TIME] Failed to create time mutex");
            return ESP_FAIL;
        }
    }

    if (network_mutex == NULL) {
        network_mutex = xSemaphoreCreateMutex();
        if (network_mutex == NULL) {
            printf("\n[TIME] Failed to create network mutex");
            return ESP_FAIL;
        }
    }

    if (time_event_group == NULL) {
        time_event_group = xEventGroupCreate();
        if (time_event_group == NULL) {
            printf("\n[TIME] Failed to create time event group");
            return ESP_FAIL;
        }
    }

    // Set timezone to UTC
    setenv("TZ", "UTC0", 1);
    tzset();

    // Try to restore epoch from NVS
    time_t saved_epoch = 0;
    if (read_epoch_from_nvs(&saved_epoch) == ESP_OK && saved_epoch > 1577836800) {
        struct timeval tv = {.tv_sec = saved_epoch, .tv_usec = 0};
        if (settimeofday(&tv, NULL) == 0) {
            printf("\n[TIME] RTC restored from NVS: %lld", (long long)saved_epoch);
            time_synced = true;
        }
    }

    printf("\n[TIME] Time manager initialized (UTC mode, waiting for WiFi)");
    return ESP_OK;
}


esp_err_t time_manager_notify_wifi_status(bool connected)
{
    if (network_mutex == NULL) {
        return ESP_FAIL;
    }

    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool was_connected = wifi_connected;
    wifi_connected = connected;

    xSemaphoreGive(network_mutex);

    if (connected && !was_connected) {
        printf("\n[TIME] WiFi connected - starting time sync");

        if (!sync_task_created) {
            if (xTaskCreate(time_sync_task, "time_sync", SYNC_TASK_STACK_SIZE,
                           NULL, SYNC_TASK_PRIORITY, &sync_task_handle) == pdPASS) {
                sync_task_created = true;
                printf("\n[TIME] Time sync task started");
            } else {
                printf("\n[TIME] Failed to create time sync task");
                return ESP_FAIL;
            }
        } else {
            // Notify existing task to sync now
            xTaskNotifyGive(sync_task_handle);
        }
    } else if (!connected && was_connected) {
        printf("\n[TIME] WiFi disconnected");
    }

    return ESP_OK;
}


esp_err_t time_manager_get_timestamp(char *timestamp_out, size_t max_len)
{
    if (timestamp_out == NULL || max_len < 32) {
        return ESP_ERR_INVALID_ARG;
    }

    if (time_mutex == NULL) {
        time_mutex = xSemaphoreCreateMutex();
        if (time_mutex == NULL) {
            snprintf(timestamp_out, max_len, "D:00-00-0000&T:00:00:00Z");
            return ESP_FAIL;
        }
    }

    if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    time_t now;
    time(&now);

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);  // Always use UTC

    if (timeinfo.tm_year < 120) {  // Year < 2020
        xSemaphoreGive(time_mutex);
        snprintf(timestamp_out, max_len, "D:00-00-0000&T:00:00:00Z");
        return ESP_FAIL;
    }

    snprintf(timestamp_out, max_len, "D:%02d-%02d-%04d&T:%02d:%02d:%02dZ",
             timeinfo.tm_mday,
             timeinfo.tm_mon + 1,
             timeinfo.tm_year + 1900,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);

    xSemaphoreGive(time_mutex);
    return ESP_OK;
}


bool time_manager_is_synced(void)
{
    if (time_mutex == NULL) {
        return false;
    }

    bool synced = false;
    if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        synced = time_synced;
        xSemaphoreGive(time_mutex);
    }
    return synced;
}


esp_err_t time_manager_wait_sync(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    printf("\n[TIME] Waiting for time sync (timeout: %lu ms)...", (unsigned long)timeout_ms);

    while (!time_synced) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            printf("\n[TIME] Time sync timeout after %lu ms", (unsigned long) timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    printf("\n[TIME] Time sync complete!");
    return ESP_OK;
}

esp_err_t time_manager_wait_for_sync_completion(uint32_t timeout_ms)
{
    if (time_event_group == NULL) {
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(
        time_event_group,
        TIME_EVENT_SYNC_COMPLETE | TIME_EVENT_SYNC_FAILED,
        pdFALSE,  // Don't clear bits
        pdFALSE,  // Wait for ANY bit
        pdMS_TO_TICKS(timeout_ms)
    );

    if (bits & TIME_EVENT_SYNC_COMPLETE) {
        return ESP_OK;
    } else if (bits & TIME_EVENT_SYNC_FAILED) {
        return ESP_FAIL;
    } else {
        return ESP_ERR_TIMEOUT;
    }
}


esp_err_t time_manager_get_epoch(time_t *epoch_out)
{
    if (epoch_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (time_mutex != NULL) {
        if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }

    time(epoch_out);

    if (time_mutex != NULL) {
        xSemaphoreGive(time_mutex);
    }

    return ESP_OK;
}


esp_err_t time_manager_ensure_initialized(void)
{
    if (time_mutex == NULL) {
        time_mutex = xSemaphoreCreateMutex();
        if (time_mutex == NULL) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}


esp_err_t time_manager_force_sync(void)
{
    if (!is_wifi_available()) {
        printf("\n[TIME] Cannot sync - WiFi not available");
        return ESP_FAIL;
    }

    if (sync_task_handle != NULL) {
        xTaskNotifyGive(sync_task_handle);
        return ESP_OK;
    }
    return ESP_FAIL;
}

// ==================== SNTP FUNCTIONS ====================


static esp_err_t init_sntp_and_sync(void)
{
    if (!is_wifi_available()) {
        printf("\n[TIME] Cannot initialize SNTP - WiFi not available");
        return ESP_FAIL;
    }

    printf("\n[TIME] Initializing SNTP (UTC mode)...");

    if (esp_sntp_enabled()) {
        printf("\n[TIME] SNTP already enabled, reinitializing...");
        esp_netif_sntp_deinit();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = NULL;
    config.smooth_sync = false;
    config.wait_for_sync = true;
    config.index_of_first_server = 0;
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        printf("\n[TIME] SNTP init failed: %s", esp_err_to_name(err));
        return err;
    }

    printf("\n[TIME] Waiting for SNTP sync (up to %d attempts)...", SNTP_RETRY_COUNT);

    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_RETRY_DELAY_MS)) != ESP_OK &&
           ++retry < SNTP_RETRY_COUNT) {

        if (retry % 3 == 0) {
            printf("\n[TIME] Attempt %d/%d...", retry, SNTP_RETRY_COUNT);
        }

        if (!is_wifi_available()) {
            printf("\n[TIME] WiFi lost during SNTP sync");
            esp_netif_sntp_deinit();
            return ESP_FAIL;
        }
    }

    if (retry < SNTP_RETRY_COUNT) {
        time_t now;
        time(&now);
        printf("\n[TIME] SNTP synced! Epoch: %ld", (long)now);

        if (now < 1577836800) {  // Before Jan 1, 2020
            printf("\n[TIME] Time looks wrong (before 2020)");
            return ESP_ERR_INVALID_STATE;
        }

        return ESP_OK;
    } else {
        printf("\n[TIME] SNTP timeout after %d attempts", SNTP_RETRY_COUNT);
        esp_netif_sntp_deinit();
        return ESP_ERR_TIMEOUT;
    }
}

// ==================== NVS STORAGE ====================

static void save_epoch_to_nvs(time_t epoch)
{
    nvs_handle_t h;
    if (nvs_open(TIMEZONE_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    nvs_set_i64(h, LASTTIME_NVS_KEY, (int64_t)epoch);
    nvs_commit(h);
    nvs_close(h);
}

static esp_err_t read_epoch_from_nvs(time_t *epoch_out)
{
    if (!epoch_out) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TIMEZONE_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    int64_t val = 0;
    err = nvs_get_i64(h, LASTTIME_NVS_KEY, &val);
    nvs_close(h);

    if (err == ESP_OK) {
        *epoch_out = (time_t)val;
    }
    return err;
}

// ==================== MAIN SYNC TASK ====================

static void time_sync_task(void *arg)
{
    while (1) {
        // Wait for WiFi
        while (!is_wifi_available()) {
            printf("\nTIME] Time sync paused - waiting for WiFi...");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        }

        // Clear previous sync status
        xEventGroupClearBits(time_event_group,
                   TIME_EVENT_SYNC_STARTED | TIME_EVENT_SYNC_COMPLETE | TIME_EVENT_SYNC_FAILED);

        xEventGroupSetBits(time_event_group, TIME_EVENT_SYNC_STARTED);

        printf("\n[TIME] === UTC TIME SYNC START ===");

        // Sync with NTP servers
        printf("\n[TIME] Syncing with NTP servers...");

        if (init_sntp_and_sync() == ESP_OK) {
            time_t now;
            time(&now);
            save_epoch_to_nvs(now);

            if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                time_synced = true;
                xSemaphoreGive(time_mutex);
            }

            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);

            printf("\n[TIME] UTC Time: %04d-%02d-%02d %02d:%02d:%02dZ",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

            xEventGroupSetBits(time_event_group, TIME_EVENT_SYNC_COMPLETE);
            printf("\n[TIME] === UTC TIME SYNC COMPLETE ===\n");

        } else {
            xEventGroupSetBits(time_event_group, TIME_EVENT_SYNC_FAILED);
            printf("\n[TIME] SNTP sync failed - using last known time");
            printf("\n[TIME] === UTC TIME SYNC FAILED ===\n");
        }

        // Wait for next sync interval or notification
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYNC_INTERVAL_SECONDS * 1000));
    }

    vTaskDelete(NULL);
}
*/
///---------------------------DEEP
/**
 * @file time_manager.c
 * @brief Simplified UTC Time Management System
 *
 * Features:
 * - SNTP synchronization for accurate UTC time
 * - NVS caching for persistence across reboots
 * - Network-aware timeouts (WiFi vs GSM)
 * - All timestamps in UTC with 'Z' suffix
 *
 * Note: AT+CCLK? is NOT used because GSM modem is in DATA mode (PPP)
 *       when connected, making AT commands unavailable.
 */

#include "time_manager.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <intr_types.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"


// ==================== CONFIGURATION ====================

// NVS storage
#define TIMEZONE_NVS_NAMESPACE "time_mgr"
#define LASTTIME_NVS_KEY "last_time"

// Timing configuration
#define SNTP_RETRY_COUNT 15
#define SNTP_RETRY_DELAY_MS 2000
#define SYNC_TASK_STACK_SIZE 4096
#define SYNC_TASK_PRIORITY 5
#define SYNC_INTERVAL_SECONDS (60 * 60)  // 1 hour

// ==================== GLOBAL STATE ====================

// Network status
static bool network_connected = false;
static time_network_status_t current_network_type = TIME_NET_NONE;
static SemaphoreHandle_t network_mutex = NULL;

// Time sync state
static bool time_synced = false;
static SemaphoreHandle_t time_mutex = NULL;
static TaskHandle_t sync_task_handle = NULL;
static bool sync_task_created = false;

// Event group for sync status
static EventGroupHandle_t time_event_group = NULL;
#define TIME_EVENT_SYNC_STARTED  BIT0
#define TIME_EVENT_SYNC_COMPLETE BIT1
#define TIME_EVENT_SYNC_FAILED   BIT2

// ==================== FORWARD DECLARATIONS ====================

static esp_err_t init_sntp_and_sync(void);
static void save_epoch_to_nvs(time_t epoch);
static esp_err_t read_epoch_from_nvs(time_t *epoch_out);
static void time_sync_task(void *arg);
static bool is_network_available(void);

// ==================== UTILITY FUNCTIONS ====================

/**
 * @brief Check if network is available
 */
static bool is_network_available(void)
{
    bool available = false;

    if (network_mutex && xSemaphoreTake(network_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        available = network_connected;
        xSemaphoreGive(network_mutex);
    }

    return available;
}

/**
 * @brief Ensure NVS is initialized
 */
static esp_err_t ensure_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

// ==================== PUBLIC API ====================

/**
 * @brief Initialize time manager
 */
esp_err_t time_manager_init(void)
{
	printf("\nInitializing Time Manager");
    esp_err_t err = ensure_nvs_init();
    if (err != ESP_OK) {
        printf("\n NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create mutexes
    if (time_mutex == NULL) {
        time_mutex = xSemaphoreCreateMutex();
        if (time_mutex == NULL) {
            printf("\n Failed to create time mutex");
            return ESP_FAIL;
        }
    }

    if (network_mutex == NULL) {
        network_mutex = xSemaphoreCreateMutex();
        if (network_mutex == NULL) {
            printf("\n Failed to create network mutex");
            return ESP_FAIL;
        }
    }

    if (time_event_group == NULL) {
        time_event_group = xEventGroupCreate();
        if (time_event_group == NULL) {
            printf("\n Failed to create time event group");
            return ESP_FAIL;
        }
    }

    // Set timezone to UTC
    setenv("TZ", "UTC0", 1);
    tzset();

    // Try to restore epoch from NVS
    time_t saved_epoch = 0;
    if (read_epoch_from_nvs(&saved_epoch) == ESP_OK && saved_epoch > 1577836800) {
        struct timeval tv = {.tv_sec = saved_epoch, .tv_usec = 0};
        if (settimeofday(&tv, NULL) == 0) {
            time_synced = true;
        }
    }

    printf("\nTime manager initialized (UTC mode, waiting for network)");
    return ESP_OK;
}

/**
 * @brief Notify time manager of network status change
 */
esp_err_t time_manager_notify_network(bool connected, time_network_status_t network_type)
{
    if (network_mutex == NULL) {
        return ESP_FAIL;
    }

    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool was_connected = network_connected;
    network_connected = connected;
    current_network_type = network_type;

    xSemaphoreGive(network_mutex);

    const char *net_name = (network_type == TIME_NET_WIFI) ? "WiFi" :
                          (network_type == TIME_NET_GSM) ? "GSM" : "None";

    if (connected && !was_connected) {
        printf("\n Network connected (%s) - starting time sync", net_name);

        if (!sync_task_created) {
            if (xTaskCreate(time_sync_task, "time_sync", SYNC_TASK_STACK_SIZE,
                           NULL, SYNC_TASK_PRIORITY, &sync_task_handle) == pdPASS) {
                sync_task_created = true;
                printf("\n Time sync task started");
            } else {
                printf("\n Failed to create time sync task");
                return ESP_FAIL;
            }
        } else {
            xTaskNotifyGive(sync_task_handle);
        }
    } else if (!connected && was_connected) {
        printf("\n Network disconnected (%s)", net_name);
    }

    return ESP_OK;
}

/**
 * @brief Get formatted UTC timestamp for MQTT
 * Format: "D:DD-MM-YYYY&T:HH:MM:SSZ"
 */
esp_err_t time_manager_get_timestamp(char *timestamp_out, size_t max_len)
{
    if (timestamp_out == NULL || max_len < 32) {
        return ESP_ERR_INVALID_ARG;
    }

    if (time_mutex == NULL) {
        time_mutex = xSemaphoreCreateMutex();
        if (time_mutex == NULL) {
            snprintf(timestamp_out, max_len, "D:00-00-0000&T:00:00:00Z");
            return ESP_FAIL;
        }
    }

    if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    time_t now;
    time(&now);

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);  // Always use UTC

    if (timeinfo.tm_year < 120) {  // Year < 2020
        xSemaphoreGive(time_mutex);
        snprintf(timestamp_out, max_len, "D:00-00-0000&T:00:00:00Z");
        return ESP_FAIL;
    }

    snprintf(timestamp_out, max_len, "D:%02d-%02d-%04d&T:%02d:%02d:%02dZ",
             timeinfo.tm_mday,
             timeinfo.tm_mon + 1,
             timeinfo.tm_year + 1900,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);

    xSemaphoreGive(time_mutex);
    return ESP_OK;
}

/**
 * @brief Check if time is synchronized
 */
bool time_manager_is_synced(void)
{
    if (time_mutex == NULL) {
        return false;
    }

    bool synced = false;
    if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        synced = time_synced;
        xSemaphoreGive(time_mutex);
    }
    return synced;
}

/**
 * @brief Wait for time synchronization with timeout
 */
esp_err_t time_manager_wait_sync(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

	printf("\n Waiting for time sync (timeout: %" PRIu32 " ms)...", timeout_ms);
	
	while (!time_synced) {
	    if ((xTaskGetTickCount() - start) >= timeout_ticks) {
    printf("\n Time sync timeout after %" PRIu32 " ms", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    printf("\n Time sync complete!");
    return ESP_OK;
}

/**
 * @brief Wait for sync completion using event group (non-blocking check)
 */
esp_err_t time_manager_wait_for_sync_completion(uint32_t timeout_ms)
{
    if (time_event_group == NULL) {
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(
        time_event_group,
        TIME_EVENT_SYNC_COMPLETE | TIME_EVENT_SYNC_FAILED,
        pdFALSE,  // Don't clear bits
        pdFALSE,  // Wait for ANY bit
        pdMS_TO_TICKS(timeout_ms)
    );

    if (bits & TIME_EVENT_SYNC_COMPLETE) {
        return ESP_OK;
    } else if (bits & TIME_EVENT_SYNC_FAILED) {
        return ESP_FAIL;
    } else {
        return ESP_ERR_TIMEOUT;
    }
}

/**
 * @brief Get current epoch time
 */
esp_err_t time_manager_get_epoch(time_t *epoch_out)
{
    if (epoch_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (time_mutex != NULL) {
        if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }

    time(epoch_out);

    if (time_mutex != NULL) {
        xSemaphoreGive(time_mutex);
    }

    return ESP_OK;
}

/**
 * @brief Ensure time manager is initialized
 */
esp_err_t time_manager_ensure_initialized(void)
{
    if (time_mutex == NULL) {
        time_mutex = xSemaphoreCreateMutex();
        if (time_mutex == NULL) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/**
 * @brief Force a time synchronization
 */
esp_err_t time_manager_force_sync(void)
{
    if (!is_network_available()) {
        printf("\n Cannot sync - network not available");
        return ESP_FAIL;
    }

    if (sync_task_handle != NULL) {
        xTaskNotifyGive(sync_task_handle);
        return ESP_OK;
    }
    return ESP_FAIL;
}

// ==================== SNTP FUNCTIONS ====================

/**
 * @brief Initialize SNTP and sync time
 */
static esp_err_t init_sntp_and_sync(void)
{
    if (!is_network_available()) {
        printf("\n Cannot initialize SNTP - network not available");
        return ESP_FAIL;
    }

    printf("\n Initializing SNTP (UTC mode)...");

    if (esp_sntp_enabled()) {
        printf("\n SNTP already enabled, reinitializing...");
        esp_netif_sntp_deinit();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = NULL;
    config.smooth_sync = false;
    config.wait_for_sync = true;
    config.index_of_first_server = 0;
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        printf("\n SNTP init failed: %s", esp_err_to_name(err));
        return err;
    }

    printf("\n Waiting for SNTP sync (up to %d attempts)...", SNTP_RETRY_COUNT);

    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_RETRY_DELAY_MS)) != ESP_OK &&
           ++retry < SNTP_RETRY_COUNT) {

        if (retry % 3 == 0) {
            printf("\n   Attempt %d/%d...", retry, SNTP_RETRY_COUNT);
        }

        if (!is_network_available()) {
            printf("\n Network lost during SNTP sync");
            esp_netif_sntp_deinit();
            return ESP_FAIL;
        }
    }

    if (retry < SNTP_RETRY_COUNT) {
        time_t now;
        time(&now);
        printf("\n SNTP synced! Epoch: %ld", (long)now);

        if (now < 1577836800) {  // Before Jan 1, 2020
            printf("\n Time looks wrong (before 2020)");
            return ESP_ERR_INVALID_STATE;
        }

        return ESP_OK;
    } else {
        printf("\n SNTP timeout after %d attempts", SNTP_RETRY_COUNT);
        esp_netif_sntp_deinit();
        return ESP_ERR_TIMEOUT;
    }
}

// ==================== NVS STORAGE ====================

static void save_epoch_to_nvs(time_t epoch)
{
    nvs_handle_t h;
    if (nvs_open(TIMEZONE_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    nvs_set_i64(h, LASTTIME_NVS_KEY, (int64_t)epoch);
    nvs_commit(h);
    nvs_close(h);
}

static esp_err_t read_epoch_from_nvs(time_t *epoch_out)
{
    if (!epoch_out) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TIMEZONE_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    int64_t val = 0;
    err = nvs_get_i64(h, LASTTIME_NVS_KEY, &val);
    nvs_close(h);

    if (err == ESP_OK) {
        *epoch_out = (time_t)val;
    }
    return err;
}

// ==================== MAIN SYNC TASK ====================

/**
 * @brief Time synchronization task - SNTP only for UTC
 */
static void time_sync_task(void *arg)
{
    while (1) {
        // Wait for network
        while (!is_network_available()) {
            printf("\n Time sync paused - waiting for network...");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        }

        // Clear previous sync status
        xEventGroupClearBits(time_event_group,
                   TIME_EVENT_SYNC_STARTED | TIME_EVENT_SYNC_COMPLETE | TIME_EVENT_SYNC_FAILED);

        xEventGroupSetBits(time_event_group, TIME_EVENT_SYNC_STARTED);

        printf("\n === UTC TIME SYNC START ===");

        // Sync with NTP servers
        printf("\n Syncing with NTP servers...");

        if (init_sntp_and_sync() == ESP_OK) {
            time_t now;
            time(&now);
            save_epoch_to_nvs(now);

            if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                time_synced = true;
                xSemaphoreGive(time_mutex);
            }

            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);

            printf("\n UTC Time: %04d-%02d-%02d %02d:%02d:%02dZ",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

            xEventGroupSetBits(time_event_group, TIME_EVENT_SYNC_COMPLETE);
            printf("\n === UTC TIME SYNC COMPLETE ===\n");

        } else {
            xEventGroupSetBits(time_event_group, TIME_EVENT_SYNC_FAILED);
            printf("\n SNTP sync failed - using last known time");
            printf("\n === UTC TIME SYNC FAILED ===\n");
        }

        // Wait for next sync interval or notification
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYNC_INTERVAL_SECONDS * 1000));
    }

    vTaskDelete(NULL);
}
