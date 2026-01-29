/**
 * @file ota_job.h
 * @brief AWS IoT OTA Jobs Handler for ESP32-S3
 */

#ifndef OTA_JOB_H
#define OTA_JOB_H

#include "esp_err.h"
#include "mqtt_client.h"
#include <stdbool.h>
#include "config.h"
// OTA Configuration
#define OTA_UPDATE_PARTITION_LABEL "ota_0"
#define OTA_URL_SIZE 4096
#define OTA_BUFFER_SIZE 4096
#define OTA_TIMEOUT_MS 300000  // 5 minutes

// OTA Job Status
typedef enum {
    OTA_JOB_STATE_IDLE,
    OTA_JOB_STATE_DOWNLOADING,
    OTA_JOB_STATE_VERIFYING,
    OTA_JOB_STATE_APPLYING,
    OTA_JOB_STATE_COMPLETED,
    OTA_JOB_STATE_FAILED,
    OTA_JOB_STATE_REJECTED
} ota_job_state_t;

// OTA Job Information
#define OTA_MAX_URL_LEN 4096      // ← CHANGE FROM 256 to 512

typedef struct {
    char job_id[64];
    char download_url[OTA_MAX_URL_LEN];  // ← Now 512 bytes
    char version[32];
    uint32_t file_size;
    int progress_percent;
    ota_job_state_t state;
    bool active;
} ota_job_info_t;
/**
 * @brief Initialize OTA Jobs handler
 * @param thing_name Device thing name
 * @param mqtt_client MQTT client handle
 * @return ESP_OK on success
 */
esp_err_t ota_job_init(const char *thing_name, esp_mqtt_client_handle_t mqtt_client);

/**
 * @brief Subscribe to OTA job topics
 * @return ESP_OK on success
 */
esp_err_t ota_job_subscribe(void);

/**
 * @brief Process incoming OTA job message
 * @param topic MQTT topic
 * @param payload Message payload
 * @param length Payload length
 */
void ota_job_process_message(const char *topic, const char *payload, int length);
void ota_job_query_next(void);

/**
 * @brief Start OTA update process
 * @return ESP_OK on success
 */
esp_err_t ota_job_start_update(void);

/**
 * @brief Get current OTA job information
 * @return Pointer to job info structure
 */
ota_job_info_t* ota_job_get_info(void);

/**
 * @brief Check if OTA is in progress
 * @return true if OTA is active
 */
bool ota_job_is_active(void);

/**
 * @brief Cancel ongoing OTA update
 */
void ota_job_cancel(void);

#endif // OTA_JOB_H
