/**
 * @file spiffs_handler.c
 * @brief SPIFFS operations implementation
 */

#include "spiffs_handler.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "time_manager.h"

static bool spiffs_initialized = false;

// ========================================
// HELPER FUNCTIONS
// ========================================

/**
 * @brief Get current timestamp string
 */
const char* get_custom_timestamp(void) {
    static char timestamp[32];
    
    // Use time_manager to get UTC timestamp
    if (time_manager_get_timestamp(timestamp, sizeof(timestamp)) == ESP_OK) {
        return timestamp;
    }
    
    // Fallback if time not synced yet
    return "D:00-00-0000&T:00:00:00Z";
}

/**
 * @brief Check if SPIFFS is initialized
 */
bool spiffs_is_initialized(void) {
    return spiffs_initialized;
}

// ========================================
// MAIN SPIFFS FUNCTIONS
// ========================================

esp_err_t spiffs_init(void)
{
    if (spiffs_initialized) {
        printf("SPIFFS already initialized\n");
        return ESP_OK;
    }

    printf("\nInitializing SPIFFS...\n");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 15,  // Increased to accommodate more files (alerts, etc.)
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            printf("Failed to mount or format filesystem\n");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            printf("Failed to find SPIFFS partition\n");
        } else {
            printf("Failed to initialize SPIFFS (%s)\n", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        printf("Failed to get SPIFFS partition information (%s)\n", esp_err_to_name(ret));
    }

    spiffs_initialized = true;
    printf("SPIFFS initialized successfully at %s\n", get_custom_timestamp());
    return ESP_OK;
}

esp_err_t spiffs_deinit(void)
{
    if (!spiffs_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_spiffs_unregister(NULL);
    if (ret == ESP_OK) {
        spiffs_initialized = false;
        printf("SPIFFS deinitialized\n");
    }
    return ret;
}

esp_err_t spiffs_store_credentials(const char *cert_pem, const char *private_key)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (cert_pem == NULL || private_key == NULL) {
        printf("Invalid credentials\n");
        return ESP_ERR_INVALID_ARG;
    }

    // Store certificate
    FILE *cert_file = fopen(SPIFFS_CERT_PATH, "w");
    if (cert_file == NULL) {
        printf("Failed to open certificate file for writing: %s\n", SPIFFS_CERT_PATH);
        return ESP_FAIL;
    }

    size_t cert_written = fwrite(cert_pem, 1, strlen(cert_pem), cert_file);
    fclose(cert_file);

    if (cert_written != strlen(cert_pem)) {
        printf("Failed to write complete certificate (wrote %d of %d bytes)\n", 
               cert_written, strlen(cert_pem));
        // Clean up partial file
        spiffs_delete_file(SPIFFS_CERT_PATH);
        return ESP_FAIL;
    }

    // Store private key
    FILE *key_file = fopen(SPIFFS_KEY_PATH, "w");
    if (key_file == NULL) {
        printf("Failed to open private key file for writing: %s\n", SPIFFS_KEY_PATH);
        // Clean up certificate file since we failed
        spiffs_delete_file(SPIFFS_CERT_PATH);
        return ESP_FAIL;
    }

    size_t key_written = fwrite(private_key, 1, strlen(private_key), key_file);
    fclose(key_file);

    if (key_written != strlen(private_key)) {
        printf("Failed to write complete private key (wrote %d of %d bytes)\n", 
               key_written, strlen(private_key));
        // Clean up both files since we failed
        spiffs_delete_file(SPIFFS_CERT_PATH);
        spiffs_delete_file(SPIFFS_KEY_PATH);
        return ESP_FAIL;
    }

    printf("AWS IoT credentials stored successfully at %s\n", get_custom_timestamp());
    printf("Certificate: %d bytes\n", strlen(cert_pem));
    printf("Private key: %d bytes\n", strlen(private_key));
    
    return ESP_OK;
}

esp_err_t spiffs_read_file(const char *path, char **buffer, size_t *size)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (path == NULL || buffer == NULL || size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check if file exists first
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("File does not exist: %s\n", path);
        return ESP_FAIL;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        printf("Failed to open file: %s\n", path);
        return ESP_FAIL;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(file);
        printf("Invalid file size: %ld for %s\n", file_size, path);
        return ESP_FAIL;
    }

    // Validate reasonable file size
    if (file_size > 20000) { // 20KB max for any file
        fclose(file);
        printf("File too large: %ld bytes for %s\n", file_size, path);
        return ESP_FAIL;
    }

    // Allocate buffer
    *buffer = malloc(file_size + 1);
    if (*buffer == NULL) {
        fclose(file);
        printf("Failed to allocate %ld bytes for file %s\n", file_size + 1, path);
        return ESP_ERR_NO_MEM;
    }

    // Read file
    size_t read_size = fread(*buffer, 1, file_size, file);
    fclose(file);

    if (read_size != (size_t)file_size) {
        free(*buffer);
        *buffer = NULL;
        printf("Failed to read complete file: %s (read %d of %ld bytes)\n", 
               path, read_size, file_size);
        return ESP_FAIL;
    }

    (*buffer)[file_size] = '\0';
    *size = file_size;
    return ESP_OK;
}

bool spiffs_credentials_exist(void)
{
    if (!spiffs_initialized) {
        return false;
    }

    struct stat st;

    // Check certificate file
    if (stat(SPIFFS_CERT_PATH, &st) != 0) {
        return false;
    }

    // Check private key file
    if (stat(SPIFFS_KEY_PATH, &st) != 0) {
        return false;
    }

    return true;
}

esp_err_t spiffs_get_info(size_t *total_bytes, size_t *used_bytes)
{
    if (!spiffs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_spiffs_info(NULL, total_bytes, used_bytes);
}

esp_err_t spiffs_delete_file(const char *path)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (remove(path) != 0) {
        printf("Failed to delete file: %s\n", path);
        return ESP_FAIL;
    }

    printf("File deleted: %s at %s\n", path, get_custom_timestamp());
    return ESP_OK;
}

/**
 * @brief Store thing name in SPIFFS
 */
esp_err_t spiffs_store_thing_name(const char *thing_name)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (thing_name == NULL || strlen(thing_name) == 0) {
        printf("Invalid thing name (NULL or empty)\n");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate thing name length
    if (strlen(thing_name) > 63) {
        printf("Thing name too long: %s\n", thing_name);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(SPIFFS_THING_NAME_PATH, "w");
    if (file == NULL) {
        printf("Failed to open thing name file for writing: %s\n", SPIFFS_THING_NAME_PATH);
        return ESP_FAIL;
    }

    size_t written = fwrite(thing_name, 1, strlen(thing_name), file);
    fclose(file);

    if (written != strlen(thing_name)) {
        printf("Failed to write complete thing name (wrote %d of %d bytes)\n", 
               written, strlen(thing_name));
        return ESP_FAIL;
    }

    // Verify the write was successful
    char verify_name[64] = {0};
    if (spiffs_read_thing_name(verify_name, sizeof(verify_name)) != ESP_OK ||
        strcmp(verify_name, thing_name) != 0) {
        printf("Thing name verification failed!\n");
        return ESP_FAIL;
    }

    printf("Thing name stored and verified: %s at %s\n", thing_name, get_custom_timestamp());
    return ESP_OK;
}

/**
 * @brief Read thing name from SPIFFS
 */
esp_err_t spiffs_read_thing_name(char *thing_name, size_t max_len)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (thing_name == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize output
    thing_name[0] = '\0';

    char *file_data = NULL;
    size_t file_size = 0;
    
    esp_err_t ret = spiffs_read_file(SPIFFS_THING_NAME_PATH, &file_data, &file_size);
    if (ret != ESP_OK || file_data == NULL) {
        printf("Failed to read thing name file\n");
        return ESP_FAIL;
    }

    // Copy with bounds checking
    strncpy(thing_name, file_data, max_len - 1);
    thing_name[max_len - 1] = '\0';
    
    // Remove any newline/carriage return characters
    char *newline = strchr(thing_name, '\n');
    if (newline) *newline = '\0';
    
    newline = strchr(thing_name, '\r');
    if (newline) *newline = '\0';

    free(file_data);

    // Validate thing name is not empty
    if (strlen(thing_name) == 0) {
        printf("Thing name is empty\n");
        return ESP_FAIL;
    }

    printf("Thing name read successfully: %s\n", thing_name);
    return ESP_OK;
}

/**
 * @brief Check if thing name exists in SPIFFS
 */
bool spiffs_thing_name_exists(void)
{
    if (!spiffs_initialized) {
        return false;
    }

    struct stat st;
    return (stat(SPIFFS_THING_NAME_PATH, &st) == 0);
}

/**
 * @brief Check if a file exists in SPIFFS
 */
bool spiffs_file_exists(const char *path)
{
    if (!spiffs_initialized) {
        return false;
    }

    struct stat st;
    return (stat(path, &st) == 0);
}

/**
 * @brief Validate certificate and key files
 */
esp_err_t spiffs_validate_credentials(void)
{
    if (!spiffs_credentials_exist()) {
        return ESP_FAIL;
    }

    char *cert_data = NULL;
    char *key_data = NULL;
    size_t cert_size = 0, key_size = 0;

    // Read and validate certificate
    if (spiffs_read_file(SPIFFS_CERT_PATH, &cert_data, &cert_size) == ESP_OK && cert_data) {
        bool cert_valid = (strstr(cert_data, "-----BEGIN CERTIFICATE-----") != NULL) &&
                         (strstr(cert_data, "-----END CERTIFICATE-----") != NULL);
        
        if (!cert_valid) {
            printf("Certificate format invalid in %s\n", SPIFFS_CERT_PATH);
            free(cert_data);
            return ESP_FAIL;
        }
        free(cert_data);
    } else {
        printf("Failed to read certificate file: %s\n", SPIFFS_CERT_PATH);
        return ESP_FAIL;
    }

    // Read and validate private key
    if (spiffs_read_file(SPIFFS_KEY_PATH, &key_data, &key_size) == ESP_OK && key_data) {
        bool key_valid = (strstr(key_data, "-----BEGIN RSA PRIVATE KEY-----") != NULL ||
                         strstr(key_data, "-----BEGIN PRIVATE KEY-----") != NULL) &&
                        (strstr(key_data, "-----END RSA PRIVATE KEY-----") != NULL ||
                         strstr(key_data, "-----END PRIVATE KEY-----") != NULL);
        
        if (!key_valid) {
            printf("Private key format invalid in %s\n", SPIFFS_KEY_PATH);
            free(key_data);
            return ESP_FAIL;
        }
        free(key_data);
    } else {
        printf("Failed to read private key file: %s\n", SPIFFS_KEY_PATH);
        return ESP_FAIL;
    }

    printf("AWS IoT credentials validation passed at %s\n", get_custom_timestamp());
    return ESP_OK;
}

/**
 * @brief Delete all provisioning data from SPIFFS
 */
esp_err_t spiffs_clean_provisioning_data(void)
{
    if (!spiffs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    printf("Cleaning up all provisioning data at %s...\n", get_custom_timestamp());
    
    bool all_success = true;
    
    // Delete certificate files
    if (spiffs_delete_file(SPIFFS_CERT_PATH) != ESP_OK) {
        all_success = false;
    }
    
    if (spiffs_delete_file(SPIFFS_KEY_PATH) != ESP_OK) {
        all_success = false;
    }
    
    // Delete thing name
    if (spiffs_delete_file(SPIFFS_THING_NAME_PATH) != ESP_OK) {
        all_success = false;
    }
    
    if (all_success) {
        printf("All provisioning data cleaned up successfully\n");
        return ESP_OK;
    } else {
        printf("Some provisioning files could not be deleted\n");
        return ESP_FAIL;
    }
}

// ========================================
// WIFI CREDENTIALS SPIFFS FUNCTIONS
// ========================================

/**
 * @brief Store WiFi credentials in SPIFFS
 */
esp_err_t spiffs_store_wifi_credentials(const char *ssid, const char *password)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || password == NULL || strlen(ssid) == 0) {
        printf("Invalid WiFi credentials\n");
        return ESP_ERR_INVALID_ARG;
    }

    // Create JSON structure
    char json_buffer[512];
    snprintf(json_buffer, sizeof(json_buffer),
             "{\"ssid\":\"%s\",\"password\":\"%s\",\"timestamp\":\"%s\"}",
             ssid, password, get_custom_timestamp());

    // Store to file
    FILE *file = fopen(SPIFFS_WIFI_CREDS_PATH, "w");
    if (file == NULL) {
        printf("Failed to open WiFi credentials file for writing: %s\n", SPIFFS_WIFI_CREDS_PATH);
        return ESP_FAIL;
    }

    size_t written = fwrite(json_buffer, 1, strlen(json_buffer), file);
    fclose(file);

    if (written != strlen(json_buffer)) {
        printf("Failed to write complete WiFi credentials (wrote %d of %d bytes)\n", 
               written, strlen(json_buffer));
        return ESP_FAIL;
    }

    // Verify the write was successful
    char verify_ssid[32] = {0};
    char verify_password[64] = {0};
    if (spiffs_load_wifi_credentials(verify_ssid, verify_password, 
                                     sizeof(verify_ssid), sizeof(verify_password)) != ESP_OK ||
        strcmp(verify_ssid, ssid) != 0) {
        printf("WiFi credentials verification failed!\n");
        return ESP_FAIL;
    }

    printf("WiFi credentials stored and verified: SSID='%s' at %s\n", ssid, get_custom_timestamp());
    return ESP_OK;
}

/**
 * @brief Load WiFi credentials from SPIFFS
 */
esp_err_t spiffs_load_wifi_credentials(char *ssid, char *password, 
                                       size_t ssid_size, size_t password_size)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || password == NULL || ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize outputs
    ssid[0] = '\0';
    password[0] = '\0';

    // Check if file exists
    if (!spiffs_file_exists(SPIFFS_WIFI_CREDS_PATH)) {
        printf("WiFi credentials file does not exist: %s\n", SPIFFS_WIFI_CREDS_PATH);
        return ESP_FAIL;
    }

    char *file_data = NULL;
    size_t file_size = 0;
    
    esp_err_t ret = spiffs_read_file(SPIFFS_WIFI_CREDS_PATH, &file_data, &file_size);
    if (ret != ESP_OK || file_data == NULL) {
        printf("Failed to read WiFi credentials file\n");
        return ESP_FAIL;
    }

    // Simple JSON parsing (we know the exact format)
    // Expected format: {"ssid":"SSID","password":"PASSWORD","timestamp":"..."}
    const char *ssid_start = strstr(file_data, "\"ssid\":\"");
    const char *password_start = strstr(file_data, "\"password\":\"");
    
    if (ssid_start && password_start) {
        ssid_start += 8; // Move past "\"ssid\":\""
        const char *ssid_end = strchr(ssid_start, '"');
        
        password_start += 12; // Move past "\"password\":\""
        const char *password_end = strchr(password_start, '"');
        
        if (ssid_end && password_end) {
            size_t ssid_len = ssid_end - ssid_start;
            size_t password_len = password_end - password_start;
            
            if (ssid_len < ssid_size && password_len < password_size) {
                strncpy(ssid, ssid_start, ssid_len);
                ssid[ssid_len] = '\0';
                
                strncpy(password, password_start, password_len);
                password[password_len] = '\0';
                
                printf("WiFi credentials loaded: SSID='%s' (file created: %s)\n", 
                       ssid, strstr(file_data, "\"timestamp\":\"") ? "timestamp available" : "no timestamp");
                free(file_data);
                return ESP_OK;
            }
        }
    }

    printf("Failed to parse WiFi credentials JSON\n");
    free(file_data);
    return ESP_FAIL;
}

/**
 * @brief Check if WiFi credentials exist in SPIFFS
 */
bool spiffs_wifi_credentials_exist(void)
{
    if (!spiffs_initialized) {
        return false;
    }

    return spiffs_file_exists(SPIFFS_WIFI_CREDS_PATH);
}

/**
 * @brief Clean all WiFi credentials from SPIFFS
 */
esp_err_t spiffs_clean_wifi_credentials(void)
{
    if (!spiffs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    printf("Cleaning WiFi credentials at %s...\n", get_custom_timestamp());
    
    esp_err_t ret = spiffs_delete_file(SPIFFS_WIFI_CREDS_PATH);
    if (ret == ESP_OK) {
        printf("WiFi credentials cleaned successfully\n");
    } else {
        printf("Failed to clean WiFi credentials\n");
    }
    
    return ret;
}

// ========================================
// ALERT STORAGE SPIFFS FUNCTIONS
// ========================================

/**
 * @brief Store an alert in SPIFFS for later transmission
 */
esp_err_t spiffs_store_alert(const char* topic, const char* payload)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL || payload == NULL || strlen(topic) == 0 || strlen(payload) == 0) {
        printf("Invalid alert data (topic or payload empty)\n");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate payload size
    if (strlen(payload) > MAX_ALERT_SIZE) {
        printf("Alert payload too large: %d bytes (max: %d)\n", 
               strlen(payload), MAX_ALERT_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    printf("Storing alert to SPIFFS...\n");
    printf("Topic: %s\n", topic);
    printf("Payload size: %d bytes\n", strlen(payload));

    // Read existing alerts or create new array
    cJSON *alerts_array = NULL;
    char *existing_data = NULL;
    size_t existing_size = 0;

    // Try to read existing file
    esp_err_t ret = spiffs_read_file(SPIFFS_ALERTS_PATH, &existing_data, &existing_size);
    
    if (ret == ESP_OK && existing_data && existing_size > 0) {
        // Parse existing JSON array
        alerts_array = cJSON_Parse(existing_data);
        free(existing_data);
        
        if (!alerts_array || !cJSON_IsArray(alerts_array)) {
            printf("Failed to parse existing alerts, creating new array\n");
            if (alerts_array) cJSON_Delete(alerts_array);
            alerts_array = cJSON_CreateArray();
        }
    } else {
        // Create new array if file doesn't exist or is invalid
        printf("Creating new alerts array\n");
        alerts_array = cJSON_CreateArray();
    }

    if (!alerts_array) {
        printf("Failed to create alerts array\n");
        return ESP_ERR_NO_MEM;
    }

    // Check if we've reached maximum storage
    int alert_count = cJSON_GetArraySize(alerts_array);
    if (alert_count >= MAX_ALERTS_IN_STORAGE) {
        printf("Alert storage full (%d alerts), removing oldest\n", alert_count);
        
        // Remove oldest alert (first in array)
        cJSON *oldest = cJSON_DetachItemFromArray(alerts_array, 0);
        if (oldest) {
            cJSON_Delete(oldest);
            alert_count--;
        }
    }

    // Create new alert object
    cJSON *new_alert = cJSON_CreateObject();
    if (!new_alert) {
        cJSON_Delete(alerts_array);
        printf("Failed to create alert object\n");
        return ESP_ERR_NO_MEM;
    }

    // Add alert data
    cJSON_AddStringToObject(new_alert, "topic", topic);
    cJSON_AddStringToObject(new_alert, "payload", payload);
    cJSON_AddNumberToObject(new_alert, "retry_count", 0);
    
    // Add timestamps
    char timestamp[32];
    if (time_manager_get_timestamp(timestamp, sizeof(timestamp)) == ESP_OK) {
        cJSON_AddStringToObject(new_alert, "storage_time", timestamp);
        cJSON_AddStringToObject(new_alert, "last_retry", timestamp);
    } else {
        cJSON_AddStringToObject(new_alert, "storage_time", get_custom_timestamp());
        cJSON_AddStringToObject(new_alert, "last_retry", get_custom_timestamp());
    }

    // Add to array
    cJSON_AddItemToArray(alerts_array, new_alert);
    alert_count++;

    // Convert back to JSON string
    char *json_str = cJSON_PrintUnformatted(alerts_array);
    if (!json_str) {
        cJSON_Delete(alerts_array);
        printf("Failed to create JSON string\n");
        return ESP_ERR_NO_MEM;
    }

    // Store to SPIFFS
    FILE *file = fopen(SPIFFS_ALERTS_PATH, "w");
    if (file == NULL) {
        free(json_str);
        cJSON_Delete(alerts_array);
        printf("Failed to open alerts file for writing: %s\n", SPIFFS_ALERTS_PATH);
        return ESP_FAIL;
    }

    size_t json_len = strlen(json_str);  // Store length BEFORE free
	size_t written = fwrite(json_str, 1, json_len, file);
	fclose(file);
	free(json_str);
	cJSON_Delete(alerts_array);
	
	if (written != json_len) {  // ✅ Use stored length
	    printf("Failed to write complete alerts file (wrote %d of %d bytes)\n", 
	           written, json_len);  // ✅ Use stored length
	    return ESP_FAIL;
	}
    printf("Alert stored successfully! Total alerts in storage: %d at %s\n", 
           alert_count, get_custom_timestamp());
    return ESP_OK;
}

/**
 * @brief Read all pending alerts from SPIFFS
 */
cJSON* spiffs_read_pending_alerts(void)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return cJSON_CreateArray();
    }

    // Check if file exists
    if (!spiffs_file_exists(SPIFFS_ALERTS_PATH)) {
        printf("No pending alerts file found\n");
        return cJSON_CreateArray();
    }

    char *file_data = NULL;
    size_t file_size = 0;
    
    esp_err_t ret = spiffs_read_file(SPIFFS_ALERTS_PATH, &file_data, &file_size);
    if (ret != ESP_OK || file_data == NULL) {
        printf("Failed to read alerts file\n");
        return cJSON_CreateArray();
    }

    // Parse JSON array
    cJSON *alerts_array = cJSON_Parse(file_data);
    free(file_data);

    if (!alerts_array || !cJSON_IsArray(alerts_array)) {
        printf("Failed to parse alerts JSON, creating empty array\n");
        if (alerts_array) cJSON_Delete(alerts_array);
        return cJSON_CreateArray();
    }

    int alert_count = cJSON_GetArraySize(alerts_array);
    printf("Read %d pending alerts from storage at %s\n", 
           alert_count, get_custom_timestamp());
    
    return alerts_array;
}

/**
 * @brief Increment retry count for an alert
 */
esp_err_t spiffs_increment_alert_retry(int alert_index)
{
    if (!spiffs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Read all alerts
    cJSON *alerts_array = spiffs_read_pending_alerts();
    if (!alerts_array || !cJSON_IsArray(alerts_array)) {
        if (alerts_array) cJSON_Delete(alerts_array);
        return ESP_FAIL;
    }

    int alert_count = cJSON_GetArraySize(alerts_array);
    if (alert_index < 0 || alert_index >= alert_count) {
        cJSON_Delete(alerts_array);
        return ESP_ERR_INVALID_ARG;
    }

    // Get the alert
    cJSON *alert = cJSON_GetArrayItem(alerts_array, alert_index);
    if (!alert) {
        cJSON_Delete(alerts_array);
        return ESP_FAIL;
    }

    // Increment retry count
    cJSON *retry_obj = cJSON_GetObjectItem(alert, "retry_count");
    int retry_count = retry_obj ? retry_obj->valueint : 0;
    
    // Remove old retry count and add new one
    cJSON_DeleteItemFromObject(alert, "retry_count");
    cJSON_AddNumberToObject(alert, "retry_count", retry_count + 1);
    
    // Update last retry timestamp
    char timestamp[32];
    if (time_manager_get_timestamp(timestamp, sizeof(timestamp)) == ESP_OK) {
        cJSON_DeleteItemFromObject(alert, "last_retry");
        cJSON_AddStringToObject(alert, "last_retry", timestamp);
    } else {
        cJSON_DeleteItemFromObject(alert, "last_retry");
        cJSON_AddStringToObject(alert, "last_retry", get_custom_timestamp());
    }

    // Save updated alerts
    char *json_str = cJSON_PrintUnformatted(alerts_array);
    if (!json_str) {
        cJSON_Delete(alerts_array);
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(SPIFFS_ALERTS_PATH, "w");
    if (file == NULL) {
        free(json_str);
        cJSON_Delete(alerts_array);
        return ESP_FAIL;
    }

    size_t json_len = strlen(json_str);  // Store length BEFORE free
	size_t written = fwrite(json_str, 1, json_len, file);
	fclose(file);
	free(json_str);
	cJSON_Delete(alerts_array);
	
	if (written != json_len) {  // ✅ Use stored length
	    return ESP_FAIL;
	}

    printf("Incremented retry count for alert %d (now %d retries)\n", 
           alert_index, retry_count + 1);
    
    return ESP_OK;
}

/**
 * @brief Remove sent alerts from storage
 */
esp_err_t spiffs_remove_sent_alerts(cJSON *sent_indices, int count)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (count == 0 || sent_indices == NULL || !cJSON_IsArray(sent_indices)) {
        printf("No alerts to remove or invalid indices\n");
        return ESP_OK;
    }

    printf("Removing %d sent alerts from storage...\n", count);

    // Read all alerts
    cJSON *all_alerts = spiffs_read_pending_alerts();
    if (!all_alerts || !cJSON_IsArray(all_alerts)) {
        printf("Failed to read alerts for removal\n");
        if (all_alerts) cJSON_Delete(all_alerts);
        return ESP_FAIL;
    }

    int original_count = cJSON_GetArraySize(all_alerts);
    if (original_count == 0) {
        printf("No alerts to remove\n");
        cJSON_Delete(all_alerts);
        return ESP_OK;
    }

    // Create new array without sent alerts
    cJSON *remaining_alerts = cJSON_CreateArray();
    if (!remaining_alerts) {
        cJSON_Delete(all_alerts);
        printf("Failed to create new alerts array\n");
        return ESP_ERR_NO_MEM;
    }

    // Sort indices in descending order for safe removal
    int indices[MAX_ALERTS_IN_STORAGE];
    int sorted_count = 0;
    
    for (int i = 0; i < count && i < cJSON_GetArraySize(sent_indices); i++) {
        cJSON *index_item = cJSON_GetArrayItem(sent_indices, i);
        if (index_item && cJSON_IsNumber(index_item)) {
            indices[sorted_count++] = index_item->valueint;
        }
    }
    
    // Sort in descending order
    for (int i = 0; i < sorted_count - 1; i++) {
        for (int j = i + 1; j < sorted_count; j++) {
            if (indices[i] < indices[j]) {
                int temp = indices[i];
                indices[i] = indices[j];
                indices[j] = temp;
            }
        }
    }

    // Copy all alerts except those being removed
    for (int i = 0; i < original_count; i++) {
        bool should_remove = false;
        
        // Check if this index should be removed
        for (int j = 0; j < sorted_count; j++) {
            if (indices[j] == i) {
                should_remove = true;
                break;
            }
        }
        
        if (!should_remove) {
            cJSON *alert = cJSON_GetArrayItem(all_alerts, i);
            if (alert) {
                cJSON_AddItemToArray(remaining_alerts, cJSON_Duplicate(alert, 1));
            }
        }
    }

    // Save remaining alerts
    char *json_str = cJSON_PrintUnformatted(remaining_alerts);
    if (!json_str) {
        cJSON_Delete(all_alerts);
        cJSON_Delete(remaining_alerts);
        printf("Failed to create JSON string for remaining alerts\n");
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(SPIFFS_ALERTS_PATH, "w");
    if (file == NULL) {
        free(json_str);
        cJSON_Delete(all_alerts);
        cJSON_Delete(remaining_alerts);
        printf("Failed to open alerts file for writing\n");
        return ESP_FAIL;
    }

	size_t json_len = strlen(json_str);  // Store length BEFORE free
	size_t written = fwrite(json_str, 1, json_len, file);
	fclose(file);
	free(json_str);
	
	cJSON_Delete(all_alerts);
	cJSON_Delete(remaining_alerts);
	
	if (written != json_len) {  // ✅ Use stored length
	    printf("Failed to write complete alerts file after removal\n");
	    return ESP_FAIL;
	}

    int remaining_count = original_count - sorted_count;
    printf("Successfully removed %d alerts, %d remain in storage at %s\n", 
           sorted_count, remaining_count, get_custom_timestamp());
    
    return ESP_OK;
}

/**
 * @brief Clear all pending alerts from storage
 */
esp_err_t spiffs_clear_all_alerts(void)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }

    printf("Clearing all pending alerts at %s...\n", get_custom_timestamp());
    
    // Simply delete the alerts file
    esp_err_t ret = spiffs_delete_file(SPIFFS_ALERTS_PATH);
    if (ret == ESP_OK) {
        printf("All alerts cleared successfully\n");
    } else {
        printf("Failed to clear alerts\n");
    }
    
    return ret;
}

/**
 * @brief Get count of pending alerts
 */
int spiffs_get_pending_alert_count(void)
{
    if (!spiffs_initialized) {
        return 0;
    }

    // Read alerts file to get count
    cJSON *alerts = spiffs_read_pending_alerts();
    if (!alerts || !cJSON_IsArray(alerts)) {
        if (alerts) cJSON_Delete(alerts);
        return 0;
    }

    int count = cJSON_GetArraySize(alerts);
    cJSON_Delete(alerts);
    
    return count;
}

/**
 * @brief Clean alert data (alias for clear_all_alerts)
 */
esp_err_t spiffs_clean_alert_data(void)
{
    return spiffs_clear_all_alerts();
}

/**
 * @brief Get oldest pending alert
 */
int spiffs_get_oldest_alert(char *topic, char *payload)
{
    if (!spiffs_initialized || !topic || !payload) {
        return -1;
    }

    cJSON *alerts_array = spiffs_read_pending_alerts();
    if (!alerts_array || !cJSON_IsArray(alerts_array)) {
        if (alerts_array) cJSON_Delete(alerts_array);
        return -1;
    }

    int alert_count = cJSON_GetArraySize(alerts_array);
    if (alert_count == 0) {
        cJSON_Delete(alerts_array);
        return -1;
    }

    // Get first alert (oldest)
    cJSON *alert = cJSON_GetArrayItem(alerts_array, 0);
    if (!alert) {
        cJSON_Delete(alerts_array);
        return -1;
    }

    cJSON *topic_obj = cJSON_GetObjectItem(alert, "topic");
    cJSON *payload_obj = cJSON_GetObjectItem(alert, "payload");

    if (!topic_obj || !payload_obj) {
        cJSON_Delete(alerts_array);
        return -1;
    }

    strncpy(topic, cJSON_GetStringValue(topic_obj), 127);
    topic[127] = '\0';
    
    strncpy(payload, cJSON_GetStringValue(payload_obj), MAX_ALERT_SIZE - 1);
    payload[MAX_ALERT_SIZE - 1] = '\0';

    cJSON_Delete(alerts_array);
    return 0; // Return index 0 for oldest
}

/**
 * @brief Check if any alert has exceeded max retries
 */
bool spiffs_should_discard_old_alerts(void)
{
    if (!spiffs_initialized) {
        return false;
    }

    cJSON *alerts_array = spiffs_read_pending_alerts();
    if (!alerts_array || !cJSON_IsArray(alerts_array)) {
        if (alerts_array) cJSON_Delete(alerts_array);
        return false;
    }

    int alert_count = cJSON_GetArraySize(alerts_array);
    bool should_discard = false;

    for (int i = 0; i < alert_count; i++) {
        cJSON *alert = cJSON_GetArrayItem(alerts_array, i);
        if (!alert) continue;

        cJSON *retry_obj = cJSON_GetObjectItem(alert, "retry_count");
        int retry_count = retry_obj ? retry_obj->valueint : 0;

        if (retry_count >= MAX_ALERT_RETRIES) {
            should_discard = true;
            break;
        }
    }

    cJSON_Delete(alerts_array);
    return should_discard;
}

/**
 * @brief Print summary of pending alerts
 */
void spiffs_print_alert_summary(void)
{
    if (!spiffs_initialized) {
        printf("SPIFFS not initialized\n");
        return;
    }

    cJSON *alerts_array = spiffs_read_pending_alerts();
    if (!alerts_array || !cJSON_IsArray(alerts_array)) {
        printf("No alerts or failed to read\n");
        if (alerts_array) cJSON_Delete(alerts_array);
        return;
    }

    int alert_count = cJSON_GetArraySize(alerts_array);
    printf("\n=== PENDING ALERTS SUMMARY ===\n");
    printf("Total alerts: %d\n", alert_count);
    printf("Storage limit: %d\n", MAX_ALERTS_IN_STORAGE);
    printf("Max retries: %d\n", MAX_ALERT_RETRIES);

    if (alert_count > 0) {
        printf("\nAlert details:\n");
        for (int i = 0; i < alert_count && i < 5; i++) { // Show first 5 only
            cJSON *alert = cJSON_GetArrayItem(alerts_array, i);
            if (!alert) continue;

            cJSON *topic = cJSON_GetObjectItem(alert, "topic");
            cJSON *retry = cJSON_GetObjectItem(alert, "retry_count");
            cJSON *storage_time = cJSON_GetObjectItem(alert, "storage_time");

            printf("  [%d] Topic: %s, Retries: %d, Stored: %s\n",
                   i,
                   topic ? cJSON_GetStringValue(topic) : "unknown",
                   retry ? retry->valueint : 0,
                   storage_time ? cJSON_GetStringValue(storage_time) : "unknown");
        }
        
        if (alert_count > 5) {
            printf("  ... and %d more\n", alert_count - 5);
        }
    }
    
    printf("==============================\n");
    
    cJSON_Delete(alerts_array);
}