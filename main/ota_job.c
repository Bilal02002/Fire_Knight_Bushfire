/**
 * @file ota_job.c
 * @brief AWS IoT OTA Jobs Implementation - SIMPLIFIED WORKING VERSION
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

// Global variables
static char device_thing_name[64] = {0};  // Moved to internal RAM (ESP32 has no PSRAM)
static esp_mqtt_client_handle_t ota_mqtt_client = NULL;
static ota_job_info_t current_job = {0};
static bool ota_initialized = false;

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

esp_err_t ota_job_init(const char *thing_name, esp_mqtt_client_handle_t mqtt_client)
{
    if (ota_initialized) {
        printf("\n[OTA] Already initialized");
        return ESP_OK;
    }

    if (!thing_name || !mqtt_client) {
        printf("\n[OTA] ERROR: Invalid parameters");
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
    printf("\n[OTA] Initialized for: %s", thing_name);
    return ESP_OK;
}

esp_err_t ota_job_subscribe(void)
{
    if (!ota_mqtt_client || !ota_initialized) {
        printf("\n[OTA] ERROR: Not initialized");
        return ESP_FAIL;
    }

    int msg_id;

    // Subscribe to job notifications
    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, notify_next_topic, 1);
    printf("\n[OTA] Subscribed to notify-next: %d", msg_id);

    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, get_accepted_topic, 1);
    printf("\n[OTA] Subscribed to get/accepted: %d", msg_id);

    msg_id = esp_mqtt_client_subscribe(ota_mqtt_client, get_rejected_topic, 1);
    printf("\n[OTA] Subscribed to get/rejected: %d", msg_id);

    // Query for pending jobs
    char empty_payload[] = "{}";
    msg_id = esp_mqtt_client_publish(ota_mqtt_client, get_pending_topic,
                                      empty_payload, strlen(empty_payload), 1, 0);
    printf("\n[OTA] Query pending jobs: %d", msg_id);

    printf("\n[OTA] OTA system ready");
    return ESP_OK;
}

void ota_job_query_next(void)
{
    if (!ota_mqtt_client) return;

    printf("\n[OTA] Manually querying for next job...");

    char topic[128];
    snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/$next/get",
             device_thing_name);

    esp_mqtt_client_publish(ota_mqtt_client, topic, "{}", 2, 1, 0);
}

// ==================== URL VALIDATION ====================

static bool validate_s3_url(const char *url)
{
    if (!url || strlen(url) == 0) {
        printf("\n[OTA] ERROR: Empty URL");
        return false;
    }

    if (strlen(url) >= OTA_MAX_URL_LEN) {
        printf("\n[OTA] ERROR: URL too long (%d chars)", strlen(url));
        return false;
    }

    if (strncmp(url, "https://", 8) != 0) {
        printf("\n[OTA] ERROR: URL must use HTTPS");
        return false;
    }

    printf("\n[OTA] URL validated: %.200s", url);
    return true;
}

// ==================== MESSAGE PROCESSING ====================

void ota_job_process_message(const char *topic, const char *payload, int length)
{
    printf("\n[OTA] Processing message on: %s", topic);
    printf("\n[OTA] Payload length: %d", length);

    if (!topic || !payload) return;

    // Parse JSON payload
    cJSON *root = cJSON_ParseWithLength(payload, length);
    if (!root) {
        printf("\n[OTA] ERROR: Failed to parse JSON");
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
        printf("\n[OTA] ERROR: No job ID found");
        cJSON_Delete(root);
        return;
    }

    // Extract job document
    cJSON *job_doc = cJSON_GetObjectItem(execution, "jobDocument");
    if (!job_doc) {
        printf("\n[OTA] ERROR: No job document found");
        cJSON_Delete(root);
        return;
    }

    // Extract download URL
    cJSON *url_obj = cJSON_GetObjectItem(job_doc, "downloadUrl");
    if (!url_obj || !cJSON_IsString(url_obj)) {
        printf("\n[OTA] ERROR: No download URL in job document");
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
        printf("\n[OTA] Job already in progress, rejecting new job");
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

    printf("\n[OTA] ===== JOB RECEIVED =====");
    printf("\n[OTA] Job ID: %s", current_job.job_id);
    printf("\n[OTA] Version: %s", current_job.version);
    printf("\n[OTA] Size: %" PRIu32 " bytes", current_job.file_size);
    printf("\n[OTA] URL (first 100 chars): %.100s", current_job.download_url);

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
                    printf("\n[OTA] Progress: %d%% (%d bytes)",
                                     current_job.progress_percent, total_received);
                    last_logged_percent = current_job.progress_percent;
                }
            }
            break;

        case HTTP_EVENT_ERROR:
            printf("\n[OTA] HTTP event error");
            break;

        case HTTP_EVENT_ON_FINISH:
            printf("\n[OTA] HTTP download finished");
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

    printf("\n[OTA] Starting OTA update task...");
    current_job.state = OTA_JOB_STATE_DOWNLOADING;
    // set_led2_status(LED2_OTA_IN_PROGRESS);  // REMOVED - LED controller not available

retry_ota:
    if (retry_count > 0) {
        printf("\n[OTA] Retry attempt %d/%d", retry_count, max_retries);
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

    printf("\n[OTA] Initializing HTTPS OTA...");

    esp_https_ota_handle_t ota_handle = NULL;
    ret = esp_https_ota_begin(&ota_config, &ota_handle);

    if (ret != ESP_OK) {
        printf("\n[OTA] ERROR: OTA begin failed: %s (0x%x)", esp_err_to_name(ret), ret);

        // Retry logic
        if (retry_count < max_retries) {
            retry_count++;
            printf("\n[OTA] Retrying connection...");
            goto retry_ota;
        }

        publish_job_status("FAILED", "Connection failed after retries");
        goto cleanup;
    }

    printf("\n[OTA] OTA initialized successfully, downloading...");

    // Get image size
    int image_size = esp_https_ota_get_image_size(ota_handle);
    if (image_size > 0) {
        printf("\n[OTA] Firmware size: %d bytes (%.2f MB)",
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
                printf("\n[OTA] Progress: %d%% (%d/%" PRIu32 " bytes)",
    				   progress,
     				   bytes_read,
       				   current_job.file_size);
                last_progress = progress;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Check download result
    if (ret == ESP_OK) {
        printf("\n[OTA] Download completed, finalizing...");

        // Verify complete data
        if (!esp_https_ota_is_complete_data_received(ota_handle)) {
            printf("\n[OTA] ERROR: Incomplete data received");
            publish_job_status("FAILED", "Incomplete download");
            esp_https_ota_abort(ota_handle);
            goto cleanup;
        }

        // Finalize OTA
        ret = esp_https_ota_finish(ota_handle);
        if (ret == ESP_OK) {
            printf("\n[OTA] ===== UPDATE SUCCESSFUL! =====");
            printf("\n[OTA] New version: %s", current_job.version);

            // ========== CRITICAL: EXPLICITLY SET BOOT PARTITION ==========
            // Get the partition that was just written to
            const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
            if (update_partition == NULL) {
                printf("\n[OTA] ERROR: Failed to get update partition");
                publish_job_status("FAILED", "Partition error");
                send_ota_alert("failed", current_job.version);
                current_job.active = false;
                current_job.state = OTA_JOB_STATE_IDLE;
                vTaskDelete(NULL);
                return;
            }

            printf("\n[OTA] Update partition: %s (type: 0x%02x, subtype: 0x%02x)",
                             update_partition->label,
                             update_partition->type,
                             update_partition->subtype);

            // Verify the partition has valid app
            esp_app_desc_t new_app_info;
            ret = esp_ota_get_partition_description(update_partition, &new_app_info);
            if (ret != ESP_OK) {
                printf("\n[OTA] ERROR: Failed to get new app description: %s",
                                 esp_err_to_name(ret));
                publish_job_status("FAILED", "App validation failed");
                send_ota_alert("failed", current_job.version);
                current_job.active = false;
                current_job.state = OTA_JOB_STATE_IDLE;
                vTaskDelete(NULL);
                return;
            }

            printf("\n[OTA] New firmware validated:");
            printf("\n[OTA]   Project: %s", new_app_info.project_name);
            printf("\n[OTA]   Version: %s", new_app_info.version);
            printf("\n[OTA]   Compile: %s %s", new_app_info.date, new_app_info.time);

            // Set boot partition for next reboot
            ret = esp_ota_set_boot_partition(update_partition);
            if (ret != ESP_OK) {
                printf("\n[OTA] ERROR: Failed to set boot partition: %s",
                                 esp_err_to_name(ret));
                publish_job_status("FAILED", "Boot partition set failed");
                send_ota_alert("failed", current_job.version);
                current_job.active = false;
                current_job.state = OTA_JOB_STATE_IDLE;
                vTaskDelete(NULL);
                return;
            }

            printf("\n[OTA] Boot partition set successfully!");
            printf("\n[OTA] Device will boot from: %s", update_partition->label);

            current_job.state = OTA_JOB_STATE_COMPLETED;
            current_job.progress_percent = 100;

            publish_job_status("SUCCEEDED", "Update completed");
            send_ota_alert("completed", current_job.version);

            // set_led2_status(LED1_WIFI_OFF);  // REMOVED - LED controller not available
            // set_led2_status(LED2_BLE_OFF);   // REMOVED - LED controller not available

            // Wait for status to be sent
            vTaskDelay(pdMS_TO_TICKS(5000));

            printf("\n[OTA] Restarting device to apply new firmware...");
            esp_restart();

        } else {
            printf("\n[OTA] ERROR: OTA finish failed: %s", esp_err_to_name(ret));
            publish_job_status("FAILED", "Finalization failed");
            send_ota_alert("failed", current_job.version);
        }
    } else {
        printf("\n[OTA] ERROR: Download failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);

        // Retry on download failure
        if (retry_count < max_retries) {
            retry_count++;
            printf("\n[OTA] Retrying download...");
            goto retry_ota;
        }

        publish_job_status("FAILED", "Download failed after retries");
    }

cleanup:
    current_job.active = false;
    current_job.state = OTA_JOB_STATE_IDLE;
    printf("\n[OTA] OTA task cleanup completed");
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

    printf("\n[OTA] Publishing status: %s - %s", status, status_details);

    int msg_id = esp_mqtt_client_publish(ota_mqtt_client, topic, payload, 0, 1, 0);
    printf("\n[OTA] Status publish msg_id: %d", msg_id);

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
        printf("\n[OTA] Cancelling job");
        publish_job_status("CANCELED", "Job cancelled by user");
        current_job.active = false;
        current_job.state = OTA_JOB_STATE_IDLE;
    }
}
