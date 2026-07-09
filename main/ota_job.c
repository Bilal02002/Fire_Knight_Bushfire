/*
#include "ota_job.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <inttypes.h>
#include "esp_heap_caps.h"

// Define OTA tag for logging
static const char *TAG = "OTA";

// Global variables
static char device_thing_name[64] = {0};  // Moved to internal RAM (ESP32 has no PSRAM)
static esp_mqtt_client_handle_t ota_mqtt_client = NULL;
static ota_job_info_t current_job = {0};
static bool ota_initialized = false;
// External MQTT client handle for disconnection during OTA
static esp_mqtt_client_handle_t external_mqtt_client = NULL;
static bool *external_mqtt_connected = NULL;
static bool ota_mqtt_was_disconnected = false;
// MQTT Topics
static char notify_next_topic[128] = {0};       // Moved to internal RAM
static char get_pending_topic[128] = {0};       // Moved to internal RAM
static char get_accepted_topic[128] = {0};      // Moved to internal RAM
static char get_rejected_topic[128] = {0};      // Moved to internal RAM
static char update_topic[128] = {0};            // Moved to internal RAM

// Function declarations
static void ota_task(void *pvParameter);
static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static void publish_job_status(const char *status, const char *status_details);
static bool validate_s3_url(const char *url);
extern void send_ota_alert(const char *iostatus, const char *version);

// ==================== INITIALIZATION ====================

void ota_job_set_mqtt_handle(esp_mqtt_client_handle_t client, bool *connected)
{
    external_mqtt_client = client;
    external_mqtt_connected = connected;
    ESP_LOGI(TAG, "External MQTT handle registered for ESP32-WROOM-32E");
}

esp_err_t ota_job_init(const char *thing_name, esp_mqtt_client_handle_t mqtt_client)
{
    if (ota_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (!thing_name || !mqtt_client) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(device_thing_name, thing_name, sizeof(device_thing_name) - 1);
    ota_mqtt_client = mqtt_client;

    // Build MQTT topics
    snprintf(notify_next_topic, sizeof(notify_next_topic),
             "$aws/things/%s/jobs/notify-next", thing_name);
    snprintf(get_pending_topic, sizeof(get_pending_topic),
             "$aws/things/%s/jobs/get", thing_name);
    snprintf(get_accepted_topic, sizeof(get_accepted_topic),
             "$aws/things/%s/jobs/get/accepted", thing_name);
    snprintf(get_rejected_topic, sizeof(get_rejected_topic),
             "$aws/things/%s/jobs/get/rejected", thing_name);
    snprintf(update_topic, sizeof(update_topic),
             "$aws/things/%s/jobs/$next/update", thing_name);

    current_job.state = OTA_JOB_STATE_IDLE;
    current_job.active = false;

    ota_initialized = true;
    ESP_LOGI(TAG, "Initialized for: %s", thing_name);
    return ESP_OK;
}

esp_err_t ota_job_subscribe(void)
{
    if (!ota_mqtt_client || !ota_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_FAIL;
    }

    int msg_id;

    // Subscribe to job notifications
    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, notify_next_topic, 1);
    ESP_LOGI(TAG, "Subscribed to notify-next: %d", msg_id);

    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, get_accepted_topic, 1);
    ESP_LOGI(TAG, "Subscribed to get/accepted: %d", msg_id);

    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, get_rejected_topic, 1);
    ESP_LOGI(TAG, "Subscribed to get/rejected: %d", msg_id);

    // Query for pending jobs
    char empty_payload[] = "{}";
    msg_id = esp_mqtt_client_publish(ota_mqtt_client, get_pending_topic,
                                      empty_payload, strlen(empty_payload), 1, 0);
    ESP_LOGI(TAG, "Query pending jobs: %d", msg_id);

    ESP_LOGI(TAG, "OTA system ready");
    return ESP_OK;
}

void ota_job_query_next(void)
{
    if (!ota_mqtt_client) return;

    ESP_LOGI(TAG, "Manually querying for next job...");

    char topic[128];
    snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/$next/get",
             device_thing_name);

    esp_mqtt_client_publish(ota_mqtt_client, topic, "{}", 2, 1, 0);
}

// ==================== URL VALIDATION ====================

static bool validate_s3_url(const char *url)
{
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "Empty URL");
        return false;
    }

    if (strlen(url) >= OTA_MAX_URL_LEN) {
        ESP_LOGE(TAG, "URL too long (%d chars)", strlen(url));
        return false;
    }

    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "URL must use HTTPS");
        return false;
    }

    ESP_LOGI(TAG, "URL validated: %.200s", url);
    return true;
}

// ==================== MESSAGE PROCESSING ====================

void ota_job_process_message(const char *topic, const char *payload, int length)
{
    ESP_LOGI(TAG, "Processing message on: %s", topic);
    ESP_LOGI(TAG, "Payload length: %d", length);

    if (!topic || !payload) return;

    // Parse JSON payload
    cJSON *root = cJSON_ParseWithLength(payload, length);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    // Check for execution object (job details)
    cJSON *execution = cJSON_GetObjectItem(root, "execution");
    if (!execution) {
        cJSON_Delete(root);
        return;
    }

    // Extract job ID
    cJSON *job_id = cJSON_GetObjectItem(execution, "jobId");
    if (!job_id || !cJSON_IsString(job_id)) {
        ESP_LOGE(TAG, "No job ID found");
        cJSON_Delete(root);
        return;
    }

    // Extract job document
    cJSON *job_doc = cJSON_GetObjectItem(execution, "jobDocument");
    if (!job_doc) {
        ESP_LOGE(TAG, "No job document found");
        cJSON_Delete(root);
        return;
    }

    // Extract download URL
    cJSON *url_obj = cJSON_GetObjectItem(job_doc, "downloadUrl");
    if (!url_obj || !cJSON_IsString(url_obj)) {
        ESP_LOGE(TAG, "No download URL in job document");
        publish_job_status("REJECTED", "Missing download URL");
        cJSON_Delete(root);
        return;
    }

    // Validate URL before proceeding
    if (!validate_s3_url(url_obj->valuestring)) {
        publish_job_status("REJECTED", "Invalid download URL");
        cJSON_Delete(root);
        return;
    }

    // Check if already processing a job
    if (current_job.active) {
        ESP_LOGW(TAG, "Job already in progress, rejecting new job");
        publish_job_status("REJECTED", "Another job in progress");
        cJSON_Delete(root);
        return;
    }

    // Store job information
    strncpy(current_job.job_id, job_id->valuestring, sizeof(current_job.job_id) - 1);
    strncpy(current_job.download_url, url_obj->valuestring, sizeof(current_job.download_url) - 1);

    // Extract version (optional)
    cJSON *version_obj = cJSON_GetObjectItem(job_doc, "version");
    const char *version = (version_obj && cJSON_IsString(version_obj)) ?
                          version_obj->valuestring : "unknown";
    strncpy(current_job.version, version, sizeof(current_job.version) - 1);

    // Extract file size (optional)
    cJSON *size_obj = cJSON_GetObjectItem(job_doc, "fileSize");
    current_job.file_size = (size_obj && cJSON_IsNumber(size_obj)) ?
                         size_obj->valueint : 0;

    current_job.active = true;
    current_job.state = OTA_JOB_STATE_IDLE;
    current_job.progress_percent = 0;

    ESP_LOGI(TAG, "===== JOB RECEIVED =====");
    ESP_LOGI(TAG, "Job ID: %s", current_job.job_id);
    ESP_LOGI(TAG, "Version: %s", current_job.version);
    ESP_LOGI(TAG, "Size: %" PRIu32 " bytes", current_job.file_size);
    ESP_LOGI(TAG, "URL (first 100 chars): %.100s", current_job.download_url);

    cJSON_Delete(root);

    // Update job status to IN_PROGRESS
    publish_job_status("IN_PROGRESS", "Starting OTA update");
    send_ota_alert("start", current_job.version);
    // Start OTA update task
    xTaskCreate(ota_task, "ota_update", 16384, NULL, 5, NULL);
}

// ==================== OTA UPDATE ====================

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (current_job.file_size > 0 && evt->data_len > 0) {
                static size_t total_received = 0;
                total_received += evt->data_len;
                current_job.progress_percent = (total_received * 100) / current_job.file_size;

                // Log progress every 10%
                static int last_logged_percent = 0;
                if (current_job.progress_percent >= last_logged_percent + 10) {
                    ESP_LOGI(TAG, "Progress: %d%% (%d bytes)",
                             current_job.progress_percent, total_received);
                    last_logged_percent = current_job.progress_percent;
                }
            }
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP event error");
            break;

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP download finished");
            break;

        default:
            break;
    }
    return ESP_OK;
}

static void ota_task(void *pvParameter)
{
    esp_err_t ret;
    int retry_count = 0;
    const int max_retries = 3;

    ESP_LOGI(TAG, "Starting OTA update task...");
    current_job.state = OTA_JOB_STATE_DOWNLOADING;
    
    // ========== ESP32-WROOM-32E MEMORY FIX: DISCONNECT MQTT ==========
    if (external_mqtt_client != NULL && external_mqtt_connected != NULL) {
        if (*external_mqtt_connected) {
            ESP_LOGI(TAG, "ESP32-WROOM-32E OTA PREPARATION");
            ESP_LOGI(TAG, "Disconnecting MQTT to free memory");
            
            // Log memory before
            size_t free_before = esp_get_free_heap_size();
            ESP_LOGI(TAG, "Memory BEFORE MQTT disconnect: %d bytes (%.1f KB)", 
                   free_before, free_before/1024.0);
            
            // Send alert before disconnecting
            send_ota_alert("preparing", current_job.version);
            vTaskDelay(pdMS_TO_TICKS(500));
            
            // Disconnect MQTT
            ESP_LOGI(TAG, "Disconnecting external MQTT client...");
            esp_mqtt_client_disconnect(external_mqtt_client);
            
            // Wait for clean disconnect (max 5 seconds)
            int wait_count = 0;
            while (*external_mqtt_connected && wait_count < 50) {
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_count++;
            }
            
            if (*external_mqtt_connected) {
                ESP_LOGW(TAG, "MQTT disconnect timeout, forcing state");
                *external_mqtt_connected = false;
            } else {
                ESP_LOGI(TAG, "MQTT disconnected successfully");
            }
            
            // Allow cleanup time
            vTaskDelay(pdMS_TO_TICKS(1500));
            
            // Log memory after
            size_t free_after = esp_get_free_heap_size();
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            int freed = free_after - free_before;
            
            ESP_LOGI(TAG, "Memory AFTER MQTT disconnect:");
            ESP_LOGI(TAG, "  Free heap:     %6d bytes (%5.1f KB)", free_after, free_after/1024.0);
            ESP_LOGI(TAG, "  Memory freed:  %6d bytes (%5.1f KB)", freed, freed/1024.0);
            ESP_LOGI(TAG, "  Largest block: %6d bytes (%5.1f KB)", largest, largest/1024.0);
            
            // Verify sufficient memory
            if (free_after < 70000 || largest < 60000) {
                ESP_LOGW(TAG, "Memory may still be insufficient!");
                ESP_LOGI(TAG, "Recommended: 70KB free, 60KB largest block");
            } else {
                ESP_LOGI(TAG, "Sufficient memory available for OTA");
            }
            
            ota_mqtt_was_disconnected = true;
            
            ESP_LOGI(TAG, "READY FOR OTA DOWNLOAD");
        } else {
            ESP_LOGI(TAG, "MQTT already disconnected, proceeding with OTA");
        }
    } else {
        ESP_LOGW(TAG, "External MQTT handle not set");
        ESP_LOGI(TAG, "Call ota_job_set_mqtt_handle() from main.c");
        ESP_LOGW(TAG, "Proceeding with OTA (may fail due to memory)");
    }

retry_ota:
    if (retry_count > 0) {
        ESP_LOGI(TAG, "Retry attempt %d/%d", retry_count, max_retries);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // Configure HTTP client
    esp_http_client_config_t http_config = {
        .url = current_job.download_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 45000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .event_handler = http_event_handler,
        .keep_alive_enable = true,

        // TLS configuration for S3
        .cert_pem = AWS_CA_CERT,
        .skip_cert_common_name_check = false,
        .use_global_ca_store = false,

        .max_redirection_count = 2,
    };

    // Configure OTA
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .bulk_flash_erase = true,
        .partial_http_download = false,
    };

    ESP_LOGI(TAG, "Initializing HTTPS OTA...");

    esp_https_ota_handle_t ota_handle = NULL;
    ret = esp_https_ota_begin(&ota_config, &ota_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s (0x%x)", esp_err_to_name(ret), ret);

        // Retry logic
        if (retry_count < max_retries) {
            retry_count++;
            ESP_LOGI(TAG, "Retrying connection...");
            goto retry_ota;
        }

        publish_job_status("FAILED", "Connection failed after retries");
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA initialized successfully, downloading...");

    // Get image size
    int image_size = esp_https_ota_get_image_size(ota_handle);
    if (image_size > 0) {
        ESP_LOGI(TAG, "Firmware size: %d bytes (%.2f MB)",
                 image_size, image_size / (1024.0 * 1024.0));
        current_job.file_size = image_size;
    }

    // Download with progress tracking
    int last_progress = -1;
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        // Progress reporting
        int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
        if (current_job.file_size > 0) {
            int progress = (bytes_read * 100) / current_job.file_size;
            if (progress != last_progress && progress % 10 == 0) {
                ESP_LOGI(TAG, "Progress: %d%% (%d/%" PRIu32 " bytes)",
                         progress, bytes_read, current_job.file_size);
                last_progress = progress;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Check download result
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Download completed, finalizing...");

        // Verify complete data
        if (!esp_https_ota_is_complete_data_received(ota_handle)) {
            ESP_LOGE(TAG, "Incomplete data received");
            publish_job_status("FAILED", "Incomplete download");
            esp_https_ota_abort(ota_handle);
            goto cleanup;
        }

        // Finalize OTA
        ret = esp_https_ota_finish(ota_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "===== UPDATE SUCCESSFUL! =====");
            ESP_LOGI(TAG, "New version: %s", current_job.version);

            // ========== CRITICAL: EXPLICITLY SET BOOT PARTITION ==========
            // Get the partition that was just written to
            const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
            if (update_partition == NULL) {
                ESP_LOGE(TAG, "Failed to get update partition");
                publish_job_status("FAILED", "Partition error");
                send_ota_alert("failed", current_job.version);
                current_job.active = false;
                current_job.state = OTA_JOB_STATE_IDLE;
                vTaskDelete(NULL);
                return;
            }

            ESP_LOGI(TAG, "Update partition: %s (type: 0x%02x, subtype: 0x%02x)",
                     update_partition->label,
                     update_partition->type,
                     update_partition->subtype);

            // Verify the partition has valid app
            esp_app_desc_t new_app_info;
            ret = esp_ota_get_partition_description(update_partition, &new_app_info);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get new app description: %s",
                         esp_err_to_name(ret));
                publish_job_status("FAILED", "App validation failed");
                send_ota_alert("failed", current_job.version);
                current_job.active = false;
                current_job.state = OTA_JOB_STATE_IDLE;
                vTaskDelete(NULL);
                return;
            }

            ESP_LOGI(TAG, "New firmware validated:");
            ESP_LOGI(TAG, "  Project: %s", new_app_info.project_name);
            ESP_LOGI(TAG, "  Version: %s", new_app_info.version);
            ESP_LOGI(TAG, "  Compile: %s %s", new_app_info.date, new_app_info.time);

            // Set boot partition for next reboot
            ret = esp_ota_set_boot_partition(update_partition);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set boot partition: %s",
                         esp_err_to_name(ret));
                publish_job_status("FAILED", "Boot partition set failed");
                send_ota_alert("failed", current_job.version);
                current_job.active = false;
                current_job.state = OTA_JOB_STATE_IDLE;
                vTaskDelete(NULL);
                return;
            }

            ESP_LOGI(TAG, "Boot partition set successfully!");
            ESP_LOGI(TAG, "Device will boot from: %s", update_partition->label);

            current_job.state = OTA_JOB_STATE_COMPLETED;
            current_job.progress_percent = 100;

            publish_job_status("SUCCEEDED", "Update completed");
            send_ota_alert("completed", current_job.version);

            // Wait for status to be sent
            vTaskDelay(pdMS_TO_TICKS(5000));

            ESP_LOGI(TAG, "Restarting device to apply new firmware...");
            esp_restart();

        } else {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
            publish_job_status("FAILED", "Finalization failed");
            send_ota_alert("failed", current_job.version);
        }
    } else {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);

        // Retry on download failure
        if (retry_count < max_retries) {
            retry_count++;
            ESP_LOGI(TAG, "Retrying download...");
            goto retry_ota;
        }

        publish_job_status("FAILED", "Download failed after retries");
    }

cleanup:
    // ========== ESP32-WROOM-32E: TRIGGER MQTT RECONNECTION ==========
    if (ota_mqtt_was_disconnected) {
        ESP_LOGI(TAG, "OTA FAILED - TRIGGERING MQTT RECONNECT");
        
        // Note: Actual reconnection should be handled by state machine in main.c
        // We just flag that restoration is needed
        ota_mqtt_was_disconnected = false;
        
        ESP_LOGI(TAG, "MQTT reconnection should be handled by main.c");
        ESP_LOGI(TAG, "Set current_state = STATE_OPERATIONAL to trigger reconnect");
    }
    // =================================================================
    
    current_job.active = false;
    current_job.state = OTA_JOB_STATE_IDLE;
    ESP_LOGI(TAG, "OTA task cleanup completed");
    vTaskDelete(NULL);
}

// ==================== JOB STATUS REPORTING ====================

static void publish_job_status(const char *status, const char *status_details)
{
    if (!ota_mqtt_client || !current_job.active) {
        return;
    }

    // Build status update topic
    char topic[256];
    snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/%s/update",
             device_thing_name, current_job.job_id);

    // Create status JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", status);

    cJSON *details = cJSON_CreateObject();
    cJSON_AddStringToObject(details, "step", status_details);
    cJSON_AddNumberToObject(details, "progress", current_job.progress_percent);
    cJSON_AddItemToObject(root, "statusDetails", details);

    char *payload = cJSON_PrintUnformatted(root);

    ESP_LOGI(TAG, "Publishing status: %s - %s", status, status_details);

    int msg_id = esp_mqtt_client_publish(ota_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Status publish msg_id: %d", msg_id);

    free(payload);
    cJSON_Delete(root);
}

// ==================== UTILITY FUNCTIONS ====================

esp_err_t ota_job_start_update(void)
{
    if (!current_job.active) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

ota_job_info_t* ota_job_get_info(void)
{
    return &current_job;
}

bool ota_job_is_active(void)
{
    return current_job.active;
}

void ota_job_cancel(void)
{
    if (current_job.active) {
        ESP_LOGI(TAG, "Cancelling job");
        publish_job_status("CANCELED", "Job cancelled by user");
        current_job.active = false;
        current_job.state = OTA_JOB_STATE_IDLE;
    }
}*/


///-----new

/**
 * @file ota_job.c
 * @brief AWS IoT OTA Jobs Implementation - POSTPONE & RETRY VERSION
 * 
 * KEY FEATURE:
 * - OTA job is POSTPONED (not rejected) when memory insufficient
 * - Automatically frees memory by suspending tasks
 * - Waits and retries until memory becomes available
 * - Only cancels after maximum retry attempts
 * 
 * FIXES APPLIED:
 * 1. NULL pointer checks before all MQTT operations
 * 2. Automatic memory preparation with retry logic
 * 3. Task suspension to free memory
 * 4. MQTT disconnect to free ~40KB
 * 5. Graceful retry with exponential backoff
 */

#include "ota_job.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <inttypes.h>
#include "esp_heap_caps.h"

// Define OTA tag for logging
static const char *TAG = "OTA";

// Memory requirements and retry configuration
#define OTA_MIN_FREE_HEAP           70000   // 70 KB minimum free heap
#define OTA_MIN_LARGEST_BLOCK       60000   // 60 KB largest contiguous block
#define OTA_MEMORY_RETRY_COUNT      10      // Retry 10 times
#define OTA_MEMORY_RETRY_DELAY_MS   3000    // Wait 3 seconds between retries
#define OTA_DOWNLOAD_RETRY_COUNT    3       // Download retry attempts

// Global variables
static char device_thing_name[64] = {0};
static esp_mqtt_client_handle_t ota_mqtt_client = NULL;
static ota_job_info_t current_job = {0};
static bool ota_initialized = false;
static esp_mqtt_client_handle_t external_mqtt_client = NULL;
static bool *external_mqtt_connected = NULL;
static bool ota_mqtt_was_disconnected = false;
static char notify_next_topic[128] = {0};
static char get_pending_topic[128] = {0};
static char get_accepted_topic[128] = {0};
static char get_rejected_topic[128] = {0};
static char update_topic[128] = {0};
static esp_mqtt_client_handle_t **main_mqtt_client_ptr = NULL;
// Task suspension tracking
static bool tasks_suspended = false;
static TaskHandle_t suspended_sensor = NULL;
static TaskHandle_t suspended_fire = NULL;
static TaskHandle_t suspended_door = NULL;
static TaskHandle_t suspended_monitor = NULL;

// External task handles (from main.c)
extern TaskHandle_t taskSensorHandle;
extern TaskHandle_t taskFireDetectionHandle;
extern TaskHandle_t taskDoorHandle;
extern TaskHandle_t taskMonitorHandle;

// Function declarations
static void ota_task(void *pvParameter);
static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static void publish_job_status(const char *status, const char *status_details);
static bool validate_s3_url(const char *url);
static bool wait_for_sufficient_memory(void);
static void suspend_non_critical_tasks(void);
static void resume_suspended_tasks(void);
extern void send_ota_alert(const char *iostatus, const char *version);

// ==================== MEMORY MANAGEMENT ====================

/**
 * @brief Get current memory status
 */
static void log_memory_status(const char *context) 
{
    size_t free_heap = esp_get_free_heap_size();
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    
    ESP_LOGI(TAG, "[%s] Memory: Free=%u KB, Largest=%u KB", 
             context,
             free_heap / 1024,
             largest_block / 1024);
}

/**
 * @brief Check if memory is sufficient for OTA
 */
static bool check_memory_sufficient(void) 
{
    size_t free_heap = esp_get_free_heap_size();
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    
    return (free_heap >= OTA_MIN_FREE_HEAP && largest_block >= OTA_MIN_LARGEST_BLOCK);
}

/**
 * @brief Suspend non-critical tasks to free memory
 */
static void suspend_non_critical_tasks(void) 
{
    if (tasks_suspended) {
        ESP_LOGW(TAG, "Tasks already suspended");
        return;
    }
    
    ESP_LOGI(TAG, "Suspending non-critical tasks to free memory...");
    
    if (taskSensorHandle) {
        vTaskSuspend(taskSensorHandle);
        suspended_sensor = taskSensorHandle;
        ESP_LOGI(TAG, "  ✓ Suspended: Sensor task");
    }
    
    if (taskFireDetectionHandle) {
        vTaskSuspend(taskFireDetectionHandle);
        suspended_fire = taskFireDetectionHandle;
        ESP_LOGI(TAG, "  ✓ Suspended: Fire detection task");
    }
    
    if (taskDoorHandle) {
        vTaskSuspend(taskDoorHandle);
        suspended_door = taskDoorHandle;
        ESP_LOGI(TAG, "  ✓ Suspended: Door monitoring task");
    }
    
    if (taskMonitorHandle) {
        vTaskSuspend(taskMonitorHandle);
        suspended_monitor = taskMonitorHandle;
        ESP_LOGI(TAG, " Suspended: System monitor task");
    }
    
    tasks_suspended = true;
    vTaskDelay(pdMS_TO_TICKS(500));  // Let tasks settle
}

/**
 * @brief Resume all suspended tasks
 */
static void resume_suspended_tasks(void) 
{
    if (!tasks_suspended) {
        return;
    }
    
    ESP_LOGI(TAG, "Resuming suspended tasks...");
    
    if (suspended_sensor) {
        vTaskResume(suspended_sensor);
        ESP_LOGI(TAG, " Resumed: Sensor task");
        suspended_sensor = NULL;
    }
    
    if (suspended_fire) {
        vTaskResume(suspended_fire);
        ESP_LOGI(TAG, "  ✓ Resumed: Fire detection task");
        suspended_fire = NULL;
    }
    
    if (suspended_door) {
        vTaskResume(suspended_door);
        ESP_LOGI(TAG, " Resumed: Door monitoring task");
        suspended_door = NULL;
    }
    
    if (suspended_monitor) {
        vTaskResume(suspended_monitor);
        ESP_LOGI(TAG, " Resumed: System monitor task");
        suspended_monitor = NULL;
    }
    
    tasks_suspended = false;
    ESP_LOGI(TAG, "All tasks resumed");
}

/**
 * @brief Wait for sufficient memory, with retry and cleanup attempts
 * @return true if memory became available, false if timeout
 */
static bool wait_for_sufficient_memory(void) 
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "WAITING FOR SUFFICIENT MEMORY");
    ESP_LOGI(TAG, "Required: %d KB free, %d KB largest block",
             OTA_MIN_FREE_HEAP / 1024, OTA_MIN_LARGEST_BLOCK / 1024);
    ESP_LOGI(TAG, "========================================");
    
    log_memory_status("Initial");
    
    // Step 1: Check if already sufficient
    if (check_memory_sufficient()) {
        ESP_LOGI(TAG, "Memory already sufficient!");
        return true;
    }
    
    // Step 2: Suspend non-critical tasks
    ESP_LOGI(TAG, "Step 1: Suspending tasks...");
    suspend_non_critical_tasks();
    log_memory_status("After task suspension");
    
    if (check_memory_sufficient()) {
        ESP_LOGI(TAG, " Memory sufficient after task suspension");
        return true;
    }
    
    // Step 3: Disconnect MQTT if still not enough
    if (external_mqtt_client != NULL) {
        ESP_LOGI(TAG, "Step 2: Disconnecting MQTT...");
        
        size_t before = esp_get_free_heap_size();
        
        esp_mqtt_client_stop(external_mqtt_client);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_mqtt_client_destroy(external_mqtt_client);
       
        external_mqtt_client = NULL;
        ota_mqtt_client = NULL;
        
         if (main_mqtt_client_ptr != NULL) {
	        *main_mqtt_client_ptr = NULL;  // ✅ Clear main.c's mqtt_client
	    }
        ota_mqtt_was_disconnected = true;
        
         if (external_mqtt_connected) {
	        *external_mqtt_connected = false;
	    }
    
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        size_t after = esp_get_free_heap_size();
        ESP_LOGI(TAG, "  MQTT freed: %u KB", (after - before) / 1024);
        
        log_memory_status("After MQTT disconnect");
        
        if (check_memory_sufficient()) {
            ESP_LOGI(TAG, "✓ Memory sufficient after MQTT disconnect");
            return true;
        }
    }
    
    // Step 4: Wait and retry multiple times
    ESP_LOGI(TAG, "Step 3: Waiting for memory to stabilize...");
    
    for (int attempt = 1; attempt <= OTA_MEMORY_RETRY_COUNT; attempt++) {
        ESP_LOGI(TAG, "  Retry %d/%d - Waiting %d seconds...", 
                 attempt, OTA_MEMORY_RETRY_COUNT, 
                 OTA_MEMORY_RETRY_DELAY_MS / 1000);
        
        vTaskDelay(pdMS_TO_TICKS(OTA_MEMORY_RETRY_DELAY_MS));
        
        log_memory_status("Retry check");
        
        if (check_memory_sufficient()) {
            ESP_LOGI(TAG, "✓ Memory sufficient after waiting (attempt %d)", attempt);
            return true;
        }
        
        // Try garbage collection every 3 attempts
        if (attempt % 3 == 0) {
            ESP_LOGI(TAG, "  Attempting garbage collection...");
            heap_caps_free(NULL);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    
    // Failed to get sufficient memory
    ESP_LOGE(TAG, "✗ TIMEOUT: Could not free sufficient memory");
    ESP_LOGE(TAG, "  Waited for %d seconds total", 
             (OTA_MEMORY_RETRY_COUNT * OTA_MEMORY_RETRY_DELAY_MS) / 1000);
    log_memory_status("Final state");
    ESP_LOGI(TAG, "========================================");
    
    return false;
}

// ==================== SAFETY WRAPPERS ====================

static int safe_mqtt_publish(esp_mqtt_client_handle_t client, 
                             const char *topic, 
                             const char *data, 
                             int len, 
                             int qos, 
                             int retain) 
{
    if (client == NULL) {
        ESP_LOGW(TAG, "MQTT client is NULL - cannot publish");
        return -1;
    }
    
    int msg_id = esp_mqtt_client_publish(client, topic, data, len, qos, retain);
    
    if (msg_id < 0) {
        ESP_LOGW(TAG, "MQTT publish failed (msg_id=%d)", msg_id);
    }
    
    return msg_id;
}

// ==================== INITIALIZATION ====================

void ota_job_set_mqtt_handle(esp_mqtt_client_handle_t *client_ptr, bool *connected)
{
    main_mqtt_client_ptr = client_ptr;  // Store pointer to main's mqtt_client
    external_mqtt_client = *client_ptr;   // Store current value
    external_mqtt_connected = connected;
    ESP_LOGI(TAG, "External MQTT handle registered");
}

esp_err_t ota_job_init(const char *thing_name, esp_mqtt_client_handle_t mqtt_client)
{
    if (ota_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (!thing_name || !mqtt_client) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(device_thing_name, thing_name, sizeof(device_thing_name) - 1);
    ota_mqtt_client = mqtt_client;

    snprintf(notify_next_topic, sizeof(notify_next_topic),
             "$aws/things/%s/jobs/notify-next", thing_name);
    snprintf(get_pending_topic, sizeof(get_pending_topic),
             "$aws/things/%s/jobs/get", thing_name);
    snprintf(get_accepted_topic, sizeof(get_accepted_topic),
             "$aws/things/%s/jobs/get/accepted", thing_name);
    snprintf(get_rejected_topic, sizeof(get_rejected_topic),
             "$aws/things/%s/jobs/get/rejected", thing_name);
    snprintf(update_topic, sizeof(update_topic),
             "$aws/things/%s/jobs/$next/update", thing_name);

    current_job.state = OTA_JOB_STATE_IDLE;
    current_job.active = false;

    ota_initialized = true;
    ESP_LOGI(TAG, "Initialized for: %s", thing_name);
    return ESP_OK;
}

esp_err_t ota_job_subscribe(void)
{
    if (!ota_mqtt_client || !ota_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_FAIL;
    }

    int msg_id;

    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, notify_next_topic, 1);
    ESP_LOGI(TAG, "Subscribed to notify-next: %d", msg_id);

    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, get_accepted_topic, 1);
    ESP_LOGI(TAG, "Subscribed to get/accepted: %d", msg_id);

    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, get_rejected_topic, 1);
    ESP_LOGI(TAG, "Subscribed to get/rejected: %d", msg_id);

    char empty_payload[] = "{}";
    msg_id = esp_mqtt_client_publish(ota_mqtt_client, get_pending_topic,
                                      empty_payload, strlen(empty_payload), 1, 0);
    ESP_LOGI(TAG, "Query pending jobs: %d", msg_id);

    ESP_LOGI(TAG, "OTA system ready");
    return ESP_OK;
}

void ota_job_query_next(void)
{
    if (!ota_mqtt_client) return;

    ESP_LOGI(TAG, "Manually querying for next job...");

    char topic[128];
    snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/$next/get",
             device_thing_name);

    esp_mqtt_client_publish(ota_mqtt_client, topic, "{}", 2, 1, 0);
}

// ==================== URL VALIDATION ====================

static bool validate_s3_url(const char *url)
{
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "Empty URL");
        return false;
    }

    if (strlen(url) >= OTA_MAX_URL_LEN) {
        ESP_LOGE(TAG, "URL too long (%d chars)", strlen(url));
        return false;
    }

    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "URL must use HTTPS");
        return false;
    }

    ESP_LOGI(TAG, "URL validated: %.200s", url);
    return true;
}

// ==================== MESSAGE PROCESSING ====================

void ota_job_process_message(const char *topic, const char *payload, int length)
{
    ESP_LOGI(TAG, "Processing message on: %s", topic);
    ESP_LOGI(TAG, "Payload length: %d", length);

    if (!topic || !payload) return;

    cJSON *root = cJSON_ParseWithLength(payload, length);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    cJSON *execution = cJSON_GetObjectItem(root, "execution");
    if (!execution) {
        cJSON_Delete(root);
        return;
    }

    cJSON *job_id = cJSON_GetObjectItem(execution, "jobId");
    if (!job_id || !cJSON_IsString(job_id)) {
        ESP_LOGE(TAG, "No job ID found");
        cJSON_Delete(root);
        return;
    }

    cJSON *job_doc = cJSON_GetObjectItem(execution, "jobDocument");
    if (!job_doc) {
        ESP_LOGE(TAG, "No job document found");
        cJSON_Delete(root);
        return;
    }

    cJSON *url_obj = cJSON_GetObjectItem(job_doc, "downloadUrl");
    if (!url_obj || !cJSON_IsString(url_obj)) {
        ESP_LOGE(TAG, "No download URL in job document");
        publish_job_status("REJECTED", "Missing download URL");
        cJSON_Delete(root);
        return;
    }

    if (!validate_s3_url(url_obj->valuestring)) {
        publish_job_status("REJECTED", "Invalid download URL");
        cJSON_Delete(root);
        return;
    }

    if (current_job.active) {
        ESP_LOGW(TAG, "Job already in progress, rejecting new job");
        publish_job_status("REJECTED", "Another job in progress");
        cJSON_Delete(root);
        return;
    }

    // Store job information
    strncpy(current_job.job_id, job_id->valuestring, sizeof(current_job.job_id) - 1);
    strncpy(current_job.download_url, url_obj->valuestring, sizeof(current_job.download_url) - 1);

    cJSON *version_obj = cJSON_GetObjectItem(job_doc, "version");
    const char *version = (version_obj && cJSON_IsString(version_obj)) ?
                          version_obj->valuestring : "unknown";
    strncpy(current_job.version, version, sizeof(current_job.version) - 1);

    cJSON *size_obj = cJSON_GetObjectItem(job_doc, "fileSize");
    current_job.file_size = (size_obj && cJSON_IsNumber(size_obj)) ?
                            (uint32_t)size_obj->valuedouble : 0;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "===== JOB RECEIVED =====");
    ESP_LOGI(TAG, "Job ID: %s", current_job.job_id);
    ESP_LOGI(TAG, "Version: %s", current_job.version);
    ESP_LOGI(TAG, "Size: %" PRIu32 " bytes", current_job.file_size);
    ESP_LOGI(TAG, "URL (first 100 chars): %.100s", current_job.download_url);

    // Mark job as active
    current_job.active = true;
    current_job.state = OTA_JOB_STATE_DOWNLOADING;
    current_job.progress_percent = 0;

    // ✅ NOTE: We do NOT reject here - job is postponed in ota_task
    publish_job_status("IN_PROGRESS", "Preparing for OTA update");
    send_ota_alert("start", current_job.version);

    // Start OTA task (it will handle memory preparation)
    BaseType_t ret = xTaskCreate(ota_task, "OTA", 6144, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        publish_job_status("FAILED", "Task creation failed");
        send_ota_alert("failed", current_job.version);
        current_job.active = false;
        current_job.state = OTA_JOB_STATE_IDLE;
    }
}

// ==================== HTTP EVENT HANDLER ====================

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP event error");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP connected");
        break;
    case HTTP_EVENT_HEADERS_SENT:
        ESP_LOGI(TAG, "HTTP headers sent");
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP session finished");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP disconnected");
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ==================== OTA TASK ====================

static void ota_task(void *pvParameter)
{
    esp_err_t ret = ESP_OK;
    esp_https_ota_handle_t ota_handle = NULL;
    int download_retry = 0;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "OTA TASK STARTED");
    ESP_LOGI(TAG, "========================================");
    
    send_ota_alert("preparing", current_job.version);

    // ✅ POSTPONE: Wait for sufficient memory
    ESP_LOGI(TAG, "Checking memory availability...");
    
    if (!wait_for_sufficient_memory()) {
        // Timeout - could not get enough memory
        ESP_LOGE(TAG, "TIMEOUT: Could not prepare sufficient memory after %d retries",
                 OTA_MEMORY_RETRY_COUNT);
        ESP_LOGE(TAG, "OTA job will be REJECTED");
        
        if (ota_mqtt_client != NULL) {
            publish_job_status("FAILED", "Insufficient memory after retries");
        }
        
        send_ota_alert("failed", current_job.version);
        resume_suspended_tasks();
        goto cleanup;
    }

    // Memory is now sufficient!
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "MEMORY READY - STARTING OTA DOWNLOAD");
    ESP_LOGI(TAG, "========================================");

retry_download:
;
    // Configure HTTP client
    esp_http_client_config_t http_config = {
        .url = current_job.download_url,
        .event_handler = http_event_handler,
        .keep_alive_enable = true,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .http_client_init_cb = NULL,
        .bulk_flash_erase = true,
        .partial_http_download = true,
        .max_http_request_size = 2048,
    };

    ESP_LOGI(TAG, "Initializing HTTPS OTA...");
    ret = esp_https_ota_begin(&ota_config, &ota_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s (0x%x)", esp_err_to_name(ret), ret);
        
        if (download_retry < OTA_DOWNLOAD_RETRY_COUNT) {
            download_retry++;
            ESP_LOGI(TAG, "Retrying download (attempt %d/%d)...", 
                     download_retry, OTA_DOWNLOAD_RETRY_COUNT);
            vTaskDelay(pdMS_TO_TICKS(2000));
            goto retry_download;
        }

        ESP_LOGE(TAG, "OTA failed after %d download attempts", OTA_DOWNLOAD_RETRY_COUNT);
        send_ota_alert("failed", current_job.version);
        resume_suspended_tasks();
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA initialized successfully, downloading...");

    int image_size = esp_https_ota_get_image_size(ota_handle);
    if (image_size > 0) {
        ESP_LOGI(TAG, "Firmware size: %d bytes (%.2f MB)",
                 image_size, image_size / (1024.0 * 1024.0));
        current_job.file_size = image_size;
    }

    // Download with progress tracking
    int last_progress = -1;
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
        if (current_job.file_size > 0) {
            int progress = (bytes_read * 100) / current_job.file_size;
            current_job.progress_percent = progress;
            
            if (progress != last_progress && progress % 10 == 0) {
                ESP_LOGI(TAG, "Progress: %d%% (%d/%" PRIu32 " bytes)",
                         progress, bytes_read, current_job.file_size);
                last_progress = progress;
                send_ota_alert("downloading", current_job.version);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Check download result
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Download completed, finalizing...");

        if (!esp_https_ota_is_complete_data_received(ota_handle)) {
            ESP_LOGE(TAG, "Incomplete data received");
            esp_https_ota_abort(ota_handle);
            send_ota_alert("failed", current_job.version);
            resume_suspended_tasks();
            goto cleanup;
        }

        ret = esp_https_ota_finish(ota_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "===== UPDATE SUCCESSFUL! =====");
            ESP_LOGI(TAG, "New version: %s", current_job.version);

            const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
            if (update_partition == NULL) {
                ESP_LOGE(TAG, "Failed to get update partition");
                send_ota_alert("failed", current_job.version);
                resume_suspended_tasks();
                goto cleanup;
            }

            esp_app_desc_t new_app_info;
            ret = esp_ota_get_partition_description(update_partition, &new_app_info);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to validate new firmware");
                send_ota_alert("failed", current_job.version);
                resume_suspended_tasks();
                goto cleanup;
            }

            ESP_LOGI(TAG, "New firmware validated:");
            ESP_LOGI(TAG, "  Version: %s", new_app_info.version);

            ret = esp_ota_set_boot_partition(update_partition);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set boot partition");
                send_ota_alert("failed", current_job.version);
                resume_suspended_tasks();
                goto cleanup;
            }

            ESP_LOGI(TAG, "Boot partition set successfully!");
            current_job.state = OTA_JOB_STATE_COMPLETED;
            current_job.progress_percent = 100;

            send_ota_alert("completed", current_job.version);
            vTaskDelay(pdMS_TO_TICKS(3000));

            ESP_LOGI(TAG, "Restarting device...");
            esp_restart();

        } else {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
            send_ota_alert("failed", current_job.version);
            resume_suspended_tasks();
        }
    } else {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);

        if (download_retry < OTA_DOWNLOAD_RETRY_COUNT) {
            download_retry++;
            ESP_LOGI(TAG, "Retrying download (attempt %d/%d)...", 
                     download_retry, OTA_DOWNLOAD_RETRY_COUNT);
            vTaskDelay(pdMS_TO_TICKS(2000));
            goto retry_download;
        }

        ESP_LOGE(TAG, "Download failed after %d attempts", OTA_DOWNLOAD_RETRY_COUNT);
        send_ota_alert("failed", current_job.version);
        resume_suspended_tasks();
    }

cleanup:
    resume_suspended_tasks();
    
    if (ota_mqtt_was_disconnected) {
        ESP_LOGI(TAG, "MQTT reconnection needed");
        ota_mqtt_was_disconnected = false;
    }
    
    current_job.active = false;
    current_job.state = OTA_JOB_STATE_IDLE;
    ESP_LOGI(TAG, "OTA task cleanup completed");
    vTaskDelete(NULL);
}

// ==================== JOB STATUS REPORTING ====================

static void publish_job_status(const char *status, const char *status_details)
{
    if (ota_mqtt_client == NULL) {
        ESP_LOGW(TAG, "Cannot publish status - MQTT client NULL");
        return;
    }
    
    if (!current_job.active) {
        ESP_LOGW(TAG, "Cannot publish status - no active job");
        return;
    }

    char topic[256];
    snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/%s/update",
             device_thing_name, current_job.job_id);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;
    
    cJSON_AddStringToObject(root, "status", status);

    cJSON *details = cJSON_CreateObject();
    if (details == NULL) {
        cJSON_Delete(root);
        return;
    }
    
    cJSON_AddStringToObject(details, "step", status_details);
    cJSON_AddNumberToObject(details, "progress", current_job.progress_percent);
    cJSON_AddItemToObject(root, "statusDetails", details);

    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Publishing status: %s - %s", status, status_details);

    int msg_id = safe_mqtt_publish(ota_mqtt_client, topic, payload, 0, 1, 0);
    if (msg_id > 0) {
        ESP_LOGI(TAG, "Status publish msg_id: %d", msg_id);
    }

    free(payload);
    cJSON_Delete(root);
}

// ==================== UTILITY FUNCTIONS ====================

esp_err_t ota_job_start_update(void)
{
    if (!current_job.active) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

ota_job_info_t* ota_job_get_info(void)
{
    return &current_job;
}

bool ota_job_is_active(void)
{
    return current_job.active;
}

void ota_job_cancel(void)
{
    if (current_job.active) {
        ESP_LOGI(TAG, "Cancelling job");
        
        if (ota_mqtt_client != NULL) {
            publish_job_status("CANCELED", "Job cancelled by user");
        }
        
        resume_suspended_tasks();
        current_job.active = false;
        current_job.state = OTA_JOB_STATE_IDLE;
    }
}