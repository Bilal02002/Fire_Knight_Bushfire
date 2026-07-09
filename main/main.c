#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "mqtt_client.h"
#include "cJSON.h"


#include "spiffs_handler.h"
#include "fire_system.h"
#include "wifi_config.h"
#include "time_manager.h"
#include "gsm_manager.h"
#include "config.h"

// ========================================
// ALERT SYSTEM CONFIGURATION
// ========================================


// Alert severity levels
typedef enum {
    ALERT_SEVERITY_INFO,
    ALERT_SEVERITY_WARNING,
    ALERT_SEVERITY_CRITICAL,
    ALERT_SEVERITY_EMERGENCY
} alert_severity_t;

// Alert types
typedef enum {
    ALERT_TYPE_PROFILE_CHANGE,  //DONE
    ALERT_TYPE_EMERGENCY_STOP_ACTIVATED, //DONE
    ALERT_TYPE_EMERGENCY_STOP_DEACTIVATED,  //DONE         
    ALERT_TYPE_SYSTEM_RESET,    //DONE       
     ALERT_TYPE_START_ALL_PUMPS,              // Activation
    ALERT_TYPE_START_ALL_PUMPS_TIMER_EXPIRED,   // Normal completion
    ALERT_TYPE_START_ALL_PUMPS_EMERGENCY_STOP,  // Emergency stop
    ALERT_TYPE_START_ALL_PUMPS_WATER_LOCKOUT,   // Water lockout   //DONE       
    ALERT_TYPE_PUMP_STATE_CHANGE, //DONE
    ALERT_TYPE_FIRE_DETECTED, //DONE
    ALERT_TYPE_FIRE_CLEARED,    //DONE     
    ALERT_TYPE_MULTIPLE_FIRES, //DONE
    ALERT_TYPE_WATER_LOCKOUT, //DONE
    ALERT_TYPE_DOOR_STATUS_OPEN, //DONE
    ALERT_TYPE_DOOR_STATUS_CLOSE,
    ALERT_TYPE_WIFI_UPDATE,  //DONE    
    ALERT_TYPE_SYSTEM_ERROR,
    ALERT_TYPE_SENSOR_FAULT,
    ALERT_TYPE_CONTINUOUS_FEED, 
    ALERT_TYPE_CURRENT_SENSOR_FAULT,
    ALERT_TYPE_IR_SENSOR_FAULT,
    ALERT_TYPE_HARDWARE_CONTROL_FAIL,
    ALERT_TYPE_ADC_INIT_FAIL,
    ALERT_TYPE_PCA9555_FAIL,
        
    
    
    // Power Faults
    ALERT_TYPE_BATTERY_LOW,
    ALERT_TYPE_BATTERY_CRITICAL,
    ALERT_TYPE_SOLAR_FAULT,
    
    // System Integrity
    ALERT_TYPE_STATE_CORRUPTION,
    ALERT_TYPE_TASK_FAILURE
} alert_type_t;

// Pump state strings for alerts
typedef enum {
    PUMP_STATE_OFF = 0,
    PUMP_STATE_AUTO_ACTIVE,
    PUMP_STATE_MANUAL_ACTIVE,
    PUMP_STATE_COOLDOWN,
    PUMP_STATE_DISABLED
} pump_state_alert_t;

// Sector identifiers
typedef enum {
    SECTOR_NORTH,
    SECTOR_SOUTH,
    SECTOR_EAST,
    SECTOR_WEST,
    SECTOR_UNKNOWN
} fire_sector_t;


// ========================================
// PROVISIONING STATE MACHINE
// ========================================
typedef enum {
    PROV_STATE_IDLE,
    PROV_STATE_CONNECTING,
    PROV_STATE_REQUESTING_CERT,
    PROV_STATE_CERT_RECEIVED,
    PROV_STATE_PROVISIONING,
    PROV_STATE_COMPLETE,
    PROV_STATE_ERROR
} aws_prov_state_t;

// ========================================
// SYSTEM STATE MACHINE
// ========================================
typedef enum {
    STATE_INIT,
    STATE_WIFI_CONNECTING,
    STATE_GSM_CONNECTING,     
    STATE_CHECK_PROVISION,
    STATE_PROVISIONING,
    STATE_REGISTERING,
    STATE_OPERATIONAL,
    STATE_ERROR
} system_state_t;

typedef enum {
    ACTIVE_NET_NONE = 0,
    ACTIVE_NET_WIFI,
    ACTIVE_NET_GSM
} active_network_t;


// MQTT Publish Queue
typedef struct {
    char topic[128];
    char payload[MAX_JSON_PAYLOAD_SIZE];  // Reduced from 2048
} mqtt_publish_message_t;



// Alert structure
typedef struct {
    alert_type_t type;
    alert_severity_t severity;
    char message[128];  // Reduced from 256
    char timestamp[30];
    bool acknowledged;
    uint32_t id;
    
    // Structured data for different alert types
    union {
        // Profile change data
        struct {
            int previousProfile;
            int currentProfile;
            char profileName[64];
        } profile;
        
        // Emergency stop data
        struct {
            bool activated;
            int affectedPumpCount;
            struct {
                int pumpId;
                char pumpName[16];
                int previousState;
            } affectedPumps[4];
        } emergencyStop;
        
        // System reset data
        struct {
            char resetType[16];
            char defaultProfile[64];
            bool allPumpsReset;
            bool emergencyStopCleared;
        } systemReset;
        
        // Start all pumps data
        struct {
            bool activated;
            int duration;
            int activatedPumpCount;
            bool waterLockout;
            char reason[32];
            int totalRuntime;
        } startAllPumps;
        
        // Pump state change data
        struct {
            int pumpId;
            char pumpName[16];
            int previousState;
            int currentState;
            char activationMode[16];
            char activationSource[32];
            char trigger[32];
            char stopReason[32];
            int totalRuntime;
            int cooldownDuration;
            int previousRuntime;
            bool autoEnabled;      // TRUE if pump is in auto mode
   			bool manualEnabled;
        } pump;
        
        // Fire detection data
        struct {
            char sector[16];
            int sensorId;
            float temperature;
            float threshold;
            bool pumpActivated;
            int pumpId;
            char pumpName[16];
            float currentTemperature;
            int duration;
            int totalActiveSectors;          // Total number of sectors with fire
            char allActiveSectors[32];       // List of all active sectors e.g. "N,S,E"
        	int confirmationDurationMs;      // How long flame persisted before confirmed
        	char flameDetectionTime[30];
        	char estimatedRuntime[40];
        } fire;
        
        // Multiple fires data
        struct {
            int activeFireCount;
            struct {
                char sector[16];
                float temperature;
                bool pumpActive;
                char flameDetectionTime[30];
                int confirmationDurationMs;
                char estimatedRuntime[40];
            } affectedSectors[4];
            float waterLevel;
            float estimatedRuntime;
        } multipleFires;
        
        // Water lockout data
        struct {
            bool activated;
            float currentWaterLevel;
            bool allPumpsDisabled;
            char systemStatus[32];
        } waterLockout;
        
        // Door status data
        struct {
            bool opened;
            char action[16];
            bool doorState;
            int wasOpenDuration;
        } door;
        
         // Auto activation data
        struct {
            char trigger[32];
            int activatedPumpCount;
            struct {
                int pumpId;
                char pumpName[16];
                char sector[16];
                float temperature;
                char state[16];
            } activatedPumps[4];
            char currentProfile[64];
            float waterLevel;
            float estimatedRuntime;
        } autoActivation;
        
        // WiFi update data
        struct {
            char action[32];
            char newSSID[32];
            char previousSSID[32];
            bool requiresReboot;
            bool stored;
            char errorType[32];
            char errorCode[16];
            int ssidLength;
            int passwordLength;
            char reason[64];
        } wifi;
        
        // System error data
        struct {
            char errorType[32];
            char errorCode[16];
            char details[128];
        } systemError;
        
        // Sensor fault data
        struct {
            char sensorType[32];
            int sensorId;
            char sectorAffected[16];
            char errorCode[16];
            float lastValidReading;
        } sensorFault;
        
        // Continuous feed data
        struct {
            bool activated;
            char profile[64];
            bool waterLockoutDisabled;
            bool unlimitedWaterSupply;
            unsigned long runCapFullMs;      // Profile max run cap full (ms), 0 = lifted
            unsigned long runCapSectorMs;    // Profile max run cap sector (ms), 0 = lifted
        } continuousFeed;
        
               
		// Hardware fault data
        struct {
            char hardwareType[32];      // "PCA9555", "ADC", "PUMP_CONTROL"
            int componentId;             // Which component failed
            char errorCode[16];          // Error code from ESP-IDF
            char errorMessage[64];       // Human readable error
            bool systemCritical;         // Does this halt operations?
            int affectedPumpCount;       // How many pumps affected
            char affectedPumps[64];      // "North,South"
        } hardwareFault;
        
               
        // Battery/Power data
        struct {
            float batteryVoltage;
            float solarVoltage;
            float threshold;
            char powerState[32];         // "CRITICAL", "LOW", "CHARGING"
            char estimatedRuntime;        // Minutes remaining
            bool chargingActive;
        } powerStatus;
        
        // System integrity data
        struct {
            char integrityType[32];      // "MEMORY", "STATE", "TASK"
            char componentName[32];      // Task name or component
            int errorValue;              // Corrupted value or memory
            int expectedValue;           // What it should be
            char action[32];             // "REBOOTING", "RESETTING"
        } integrity;
        
        // Timer override data
        struct {
            int pumpId;
            char pumpName[16];
            char overrideReason[32];     // "EMERGENCY_STOP", "WATER_LOCKOUT"
            int remainingTime;           // Seconds left on timer
            int originalDuration;        // What timer was set for
        } timerOverride;
        
        // Grace period data
        struct {
            float waterLevel;
            float threshold;
            int gracePeriodDuration;     // Seconds
            bool continuousFeed;
            char outcome[32];            // "LOCKOUT_ACTIVATED", "WATER_RESTORED"
        } gracePeriod;	    
    
    
    } data;
    
    
    
} Alert;

#define printf(...) do { if(0) { (void)printf(__VA_ARGS__); } } while(0)
#define DISPLAY_PRINT(...) fprintf(stdout, __VA_ARGS__)

// ========================================
// GLOBAL VARIABLES
// ========================================
static bool sensors_ready = false;
static TickType_t boot_time = 0;
static TickType_t provisioning_timeout = 0;
static int registration_attempts = 0;
static TickType_t registration_timeout = 0;
static char registration_cloud_topic[128];
static char registration_response_topic[128];


static int last_shadow_profile = -1;
static bool last_shadow_emergency_stop = false;
static bool last_shadow_start_all_pumps = false;
static bool last_shadow_pump_manual[4] = {false, false, false, false};
static bool last_shadow_stop_pump[4] = {false,false,false, false};
static bool last_shadow_manual_activate[4] = {false, false, false, false};
static bool last_reported_manual_activate[4] = {false, false, false, false};
// Duration (ms) each pump's most recent manual_activate run was started with.
// Defaults to the fixed MANUAL_SINGLE_PUMP_TIME (used only if the shadow
// omits the field); overwritten with the user-supplied value whenever the
// web page writes a custom "manual_single_operation_time" (seconds) into
// that pump's object in the AWS IoT device shadow.
static unsigned long pumpManualOperationConfiguredMs[4] = {
    MANUAL_SINGLE_PUMP_TIME, MANUAL_SINGLE_PUMP_TIME, MANUAL_SINGLE_PUMP_TIME, MANUAL_SINGLE_PUMP_TIME
};
static char last_shadow_mode[8] = "";

// Provisioning state
static aws_prov_state_t provisioning_state = PROV_STATE_IDLE;
static SemaphoreHandle_t provisioning_mutex = NULL;

// System State
static system_state_t current_state = STATE_INIT;
static bool is_provisioned = false;
static bool provisioning_complete = false;
static bool provisioning_in_progress = false;
static unsigned long last_state_change = 0;

// Device Information
char thing_name[64] = "Unprovisioned";
char mac_address[18] = "00:00:00:00:00:00";

bool startAllPumpsActive = false;
start_all_pumps_stop_reason_t startAllPumps_stop_reason = STOP_REASON_START_ALL_NONE;
// AWS Credentials

static bool secure_provision_response_received = false;
static bool secure_provision_approved = false;
static char secure_provision_rejection_reason[256] = {0};
static char received_certificate_pem[2048] = {0};
static char received_private_key[2048] = {0};
static char received_certificate_id[128] = {0};
// Dynamic topics for secure provisioning ONLY (MAC-based)
static char secure_provision_request_topic[128] = {0};
static char secure_provision_response_topic[128] = {0};
// Credentials
static char *device_cert_pem = NULL;
static char *device_private_key = NULL;
// Status Flags
static bool is_registered = false;
static bool device_activated = false;
static bool certs_created = false;

// MQTT Client
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Profile from Shadow
static int shadow_profile = 0;  // Profile received from shadow

// Alert tracking variables
static int last_profile = -1;
static PumpState last_pump_states[4] = {PUMP_OFF};
static bool last_door_state = false;
static bool last_water_lockout = false;
static bool last_continuous_feed = false;
static bool fire_alerts_active[4] = {false};
static int active_fire_count = 0;

// NEW: Start All Pumps tracking
static TickType_t startAllPumpsActivationTime = 0;
// Duration (ms) the currently-active (or most recently activated) startAllPumps
// run was started with. Defaults to the fixed 90s (used only if the shadow
// omits the field); overwritten with the user-supplied value whenever the
// web page writes a custom "_manual_start_all_pumps_duration" (seconds) to
// the AWS IoT device shadow.
static unsigned long startAllPumpsConfiguredDurationMs = MANUAL_ALL_PUMPS_TIME;

static active_network_t current_active_network = ACTIVE_NET_NONE;
static int wifi_consecutive_failures = 0;
static TickType_t last_wifi_retry_on_gsm = 0;

// ========================================
// EXTERN DECLARATIONS
// ========================================
extern SystemProfile currentProfile;
extern ProfileConfig profiles[5];
extern PumpControl pumps[4];
extern CurrentSensor currentSensors[4];
extern bool doorOpen;
extern bool waterLockout;
extern float level_s, bat_v, sol_v;
extern float ir_s1, ir_s2, ir_s3, ir_s4;
extern float volt1, volt2, volt3, volt4;   // Inverter voltages (adc_array2[2..5] * 360)
extern bool emergencyStopActive;
// Add with other extern declarations (around line 280)
extern start_all_pumps_stop_reason_t startAllPumps_stop_reason;
// ========================================
// HELPER FUNCTIONS - ENUM TO STRING
// ========================================

static const char* get_alert_type_string(alert_type_t type) {
    switch(type) {
        case ALERT_TYPE_PROFILE_CHANGE: return "profileChange";
        case ALERT_TYPE_EMERGENCY_STOP_ACTIVATED: return "emergencyStopActivated";
        case ALERT_TYPE_EMERGENCY_STOP_DEACTIVATED: return "emergencyStopDeactivated";
        case ALERT_TYPE_SYSTEM_RESET: return "systemReset";
         // ✅ NEW: Four separate strings
        case ALERT_TYPE_START_ALL_PUMPS: return "startAllPumps";
        case ALERT_TYPE_START_ALL_PUMPS_TIMER_EXPIRED: return "startAllPumpsTimerExpired";
        case ALERT_TYPE_START_ALL_PUMPS_EMERGENCY_STOP: return "startAllPumpsEmergencyStop";
        case ALERT_TYPE_START_ALL_PUMPS_WATER_LOCKOUT: return "startAllPumpsWaterLockout";
        case ALERT_TYPE_PUMP_STATE_CHANGE: return "pumpStateChange";
        case ALERT_TYPE_FIRE_DETECTED: return "fireDetected";
        case ALERT_TYPE_FIRE_CLEARED: return "fireCleared";
        case ALERT_TYPE_MULTIPLE_FIRES: return "fireDetected";
        case ALERT_TYPE_WATER_LOCKOUT: return "waterLockout";
        case ALERT_TYPE_DOOR_STATUS_OPEN: return "doorStatusOpen";    
        case ALERT_TYPE_DOOR_STATUS_CLOSE: return "doorStatusClose"; 
        case ALERT_TYPE_WIFI_UPDATE: return "wifiUpdate";
        case ALERT_TYPE_SYSTEM_ERROR: return "systemError";
        case ALERT_TYPE_SENSOR_FAULT: return "sensorFault";
        case ALERT_TYPE_CONTINUOUS_FEED: return "continuousFeed";
        // Hardware faults
        case ALERT_TYPE_CURRENT_SENSOR_FAULT: return "currentSensorFault";
        case ALERT_TYPE_IR_SENSOR_FAULT: return "irSensorFault";
        case ALERT_TYPE_HARDWARE_CONTROL_FAIL: return "hardwareControlFail";
        case ALERT_TYPE_ADC_INIT_FAIL: return "adcInitFail";
        case ALERT_TYPE_PCA9555_FAIL: return "pca9555Fail";
                
        // Power
        case ALERT_TYPE_BATTERY_LOW: return "batteryLow";
        case ALERT_TYPE_BATTERY_CRITICAL: return "batteryCritical";
        case ALERT_TYPE_SOLAR_FAULT: return "solarFault";
        
        // System integrity
        case ALERT_TYPE_STATE_CORRUPTION: return "stateCorruption";
        case ALERT_TYPE_TASK_FAILURE: return "taskFailure";
        
        default: return "unknown";
    }
}

static const char* get_severity_string(alert_severity_t severity) {
    switch(severity) {
        case ALERT_SEVERITY_INFO: return "INFO";
        case ALERT_SEVERITY_WARNING: return "WARNING";
        case ALERT_SEVERITY_CRITICAL: return "CRITICAL";
        case ALERT_SEVERITY_EMERGENCY: return "EMERGENCY";
        default: return "UNKNOWN";
    }
}

static const char* get_pump_state_string_for_alert(int state) {
    switch(state) {
        case 0: return "OFF";
        case 1: return "AUTO_ACTIVE";
        case 2: return "MANUAL_ACTIVE";
        case 3: return "COOLDOWN";
        case 4: return "DISABLED";
        default: return "UNKNOWN";
    }
}

static const char* get_sector_name_string(fire_sector_t sector) {
    switch(sector) {
        case SECTOR_NORTH: return "NORTH";
        case SECTOR_SOUTH: return "SOUTH";
        case SECTOR_EAST: return "EAST";
        case SECTOR_WEST: return "WEST";
        default: return "UNKNOWN";
    }
}

static SystemProfile convert_profile_number_to_enum(int profile_num) {
    switch(profile_num) {
        case 0: return WILDLAND_STANDARD;
        case 1: return WILDLAND_HIGH_WIND;
        case 2: return INDUSTRIAL_HYDROCARBON;
        case 3: return CRITICAL_ASSET;
        case 4: return CONTINUOUS_FEED;
        default: return WILDLAND_STANDARD;
    }
}

static int convert_profile_enum_to_number(SystemProfile profile) {
    switch(profile) {
        case WILDLAND_STANDARD: return 0;
        case WILDLAND_HIGH_WIND: return 1;
        case INDUSTRIAL_HYDROCARBON: return 2;
        case CRITICAL_ASSET: return 3;
        case CONTINUOUS_FEED: return 4;
        default: return 0;
    }
}

// FreeRTOS Task Handles
TaskHandle_t taskSensorHandle = NULL;
TaskHandle_t taskFireDetectionHandle = NULL;
TaskHandle_t taskPumpManagementHandle = NULL;
TaskHandle_t taskWaterLockoutHandle = NULL;
TaskHandle_t taskMonitorHandle = NULL;
TaskHandle_t taskDoorHandle = NULL;
TaskHandle_t taskCommandHandle = NULL;
TaskHandle_t taskMqttPublishHandle = NULL;
TaskHandle_t taskStateMachineHandle = NULL;
TaskHandle_t taskAlertHandle = NULL;

// FreeRTOS Mutexes
SemaphoreHandle_t mutexSensorData = NULL;
SemaphoreHandle_t mutexPumpState = NULL;
SemaphoreHandle_t mutexWaterState = NULL;
SemaphoreHandle_t mutexSystemState = NULL;
SemaphoreHandle_t alert_mutex = NULL;

// Queues
QueueHandle_t commandQueue = NULL;
QueueHandle_t alert_queue = NULL;
QueueHandle_t mqtt_publish_queue = NULL;

// ==========================================
// FORWARD DECLARATIONS
// ==========================================
static esp_err_t mqtt_connect(const char *client_id, const char *cert, const char *key);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                      int32_t event_id, void *event_data);
static void subscribe_to_topics(void);
static void send_registration(void);
static void send_heartbeat(void);
static void send_system_status(void);
bool enqueue_mqtt_publish(const char *topic, const char *payload);
static void check_provisioning_status(void);
static esp_err_t start_provisioning(void);
static void get_mac_address(void);
void display_system_status(void);
static void update_shadow_state(void);
static SystemProfile convert_profile_number_to_enum(int profile_num);
static int convert_profile_enum_to_number(SystemProfile profile);
static void perform_periodic_tasks(void);

// NEW FUNCTIONS
static bool process_shadow_delta(cJSON *state);
static void check_and_reset_start_all_pumps(void);
static void check_battery_status(void);

// Alert System Functions
static void init_alert_system(void);
static void check_state_changes(void);
static void monitor_fire_sectors(void);
static void check_manual_auto_modes(void);
static void process_alerts(void);
static void alert_task(void *parameter);
static fire_sector_t get_sector_from_index(int sensor_index);
static void get_estimated_runtime_string(bool isFullSystem, char* outStr, size_t outSize);
static void get_manual_estimated_runtime_string(int pumpIndex, char* outStr, size_t outSize);
// ==========================================
// ALERT FUNCTION FORWARD DECLARATIONS
// ==========================================
static void send_alert_wifi_invalid(int ssidLen, int passLen, const char* reason);
static void send_alert_wifi_updated(const char* newSSID, const char* previousSSID);
static void send_alert_profile_change(int previousProfile, int currentProfile, const char* profileName);
static void send_alert_emergency_stop_activated(void);
static void send_alert_emergency_stop_deactivated(void);
static void send_alert_system_reset(void);
static void send_alert_start_all_pumps_activated(void);
static void send_alert_start_all_pumps_deactivated(const char* reason, int totalRuntime);
static void send_alert_pump_state_change(int pumpIndex, int previousState, int currentState,
                                         const char* activationSource, const char* trigger,
                                        float sensorTemp, const char* stopReason,
                                         int runtime, int cooldownDuration);
static void send_alert_fire_detected(int sensorIndex, const char* sectorName,
                                    float temperature, bool pumpActivated, int confirmationDurationMs);
static void send_alert_fire_cleared(int sensorIndex, const char* sectorName,
                                   float currentTemp);
static void send_alert_multiple_fires(int fireCount, float sensorValues[4],
                                     bool pumpStates[4]);
static void send_alert_water_lockout(bool activated, float currentLevel);
static void send_alert_continuous_feed(bool activated);
static void send_alert_door_opened(void);  
static void send_alert_door_closed(int openDuration);  
// Hardware fault alerts
void send_alert_pca9555_fail(const char* errorCode, const char* errorMsg);
void send_alert_adc_init_fail(const char* errorCode, const char* errorMsg);
void send_alert_hardware_control_fail(int pumpIndex, const char* errorCode);
void send_alert_current_sensor_fault(int sensorIndex, float currentValue);
void send_alert_ir_sensor_fault(int sensorIndex, float irValue);

// System state alerts
void send_alert_grace_period_expired(float waterLevel, int graceDuration);
void send_alert_pump_cooldown(int pumpIndex, int cooldownDuration);
void send_alert_timer_override(int pumpIndex, const char* reason, int remainingTime);

// Power alerts
void send_alert_battery_low(float batteryVoltage, float threshold);
void send_alert_battery_critical(float batteryVoltage, char estimatedRuntime);
void send_alert_solar_fault(float solarVoltage, const char* reason);

// System integrity alerts
void send_alert_provisioning_failed(const char* reason, int retryCount);
void send_alert_state_corruption(int pumpIndex, int corruptValue);
void send_alert_task_failure(const char* taskName, const char* reason);
// Memory optimization functions
static void clear_mqtt_outbox(void);
static char* create_compact_json_string(cJSON *json);

// System Tasks
void task_serial_monitor(void *parameter);
void task_sensor_reading(void *parameter);
void task_fire_detection(void *parameter);
void task_pump_management(void *parameter);
void task_water_lockout(void *parameter);
void task_door_monitoring(void *parameter);
void task_command_processor(void *parameter);
void task_mqtt_publish(void *parameter);
void task_state_machine(void *parameter);
static void store_alert_to_spiffs(const char* topic, const char* payload);
static void send_pending_alerts_from_storage(void);
static void check_and_send_pending_alerts(bool force_check);

void debug_wifi_status(void);

// ========================================
// NEW: GSM HELPER FUNCTIONS
// ========================================

#if GSM_ENABLED

static bool try_gsm_connection(void) {
    // Check if GSM is already initialized
    if (!gsm_active) {
        printf("\n[GSM] ERROR: GSM manager not initialized!");
        
        // Try to initialize now as fallback
        printf("\n[GSM] Attempting late initialization...");
        if (gsm_manager_init() != ESP_OK) {
            printf("\n[GSM] GSM init failed");
            return false;
        }
        printf("\n[GSM]  Late init succeeded (should init in app_main)");
    }
    printf("\n[GSM] Attempting GSM connection...");
    if (gsm_manager_connect() == ESP_OK) {
        printf("\n[GSM] GSM connected successfully");
        // time_manager is notified inside gsm_manager's IP event handler
        return true;
    }
    
    printf("\n[GSM] GSM connection failed");
    return false;
}


static void handle_gsm_disconnect(void) {
    printf("\n[GSM] Handling GSM disconnection...");
    // time_manager is notified inside gsm_manager's IP event handler
    // via time_manager_notify_network(false, TIME_NET_GSM)
}
#endif

//---- TESTING CERTIFICATES ----
static esp_err_t validate_certificates(void) {
    printf("\n[CERT] Validating certificates...");
    
    // Check certificate format
    if (strstr(AWS_CLAIM_CERT, "-----BEGIN CERTIFICATE-----") == NULL) {
        printf("\n[CERT] ERROR: Invalid certificate format - missing BEGIN marker");
        return ESP_FAIL;
    }
    
    if (strstr(AWS_CLAIM_CERT, "-----END CERTIFICATE-----") == NULL) {
        printf("\n[CERT] ERROR: Invalid certificate format - missing END marker");
        return ESP_FAIL;
    }
    
    // Check private key format
    if (strstr(AWS_CLAIM_PRIVATE_KEY, "-----BEGIN RSA PRIVATE KEY-----") == NULL &&
        strstr(AWS_CLAIM_PRIVATE_KEY, "-----BEGIN PRIVATE KEY-----") == NULL) {
        printf("\n[CERT] ERROR: Invalid private key format");
        return ESP_FAIL;
    }
    
    printf("\n[CERT] Certificate length: %d bytes", strlen(AWS_CLAIM_CERT));
    printf("\n[CERT] Private key length: %d bytes", strlen(AWS_CLAIM_PRIVATE_KEY));
    printf("\n[CERT] Validation passed");
    
    return ESP_OK;
}


///--------TESTING 

void handle_cloud_response(const char* topic, const char* payload) {
    // Check if this is a cloud registration response
    if (strstr(topic, "RegistrationDevice") != NULL) {
        cJSON *json = cJSON_Parse(payload);
        if (json) {
            printf("\n[CLOUD] Received registration response:");
            printf("\n[CLOUD] %s", payload);
            
            cJSON *message = cJSON_GetObjectItem(json, "message");
            if (message && strcmp(cJSON_GetStringValue(message), "DeviceActivated") == 0) {
                printf("\n[CLOUD] Device activated by cloud! (new format)");
                device_activated = true;
                
                // Also get thingName if provided
                cJSON *thingNameObj = cJSON_GetObjectItem(json, "thingName");
                if (thingNameObj) {
                    const char* receivedThingName = cJSON_GetStringValue(thingNameObj);
                    if (receivedThingName && strlen(receivedThingName) > 0) {
                        printf("\n[CLOUD] Thing name from cloud: %s", receivedThingName);
                        
                        // Update our thing_name if different
                        if (strcmp(thing_name, receivedThingName) != 0) {
                            strncpy(thing_name, receivedThingName, sizeof(thing_name) - 1);
                            printf("\n[CLOUD] Updated thing name to: %s", thing_name);
                            
                            // Save to SPIFFS
                        }
                    }
                }
            }         
           cJSON_Delete(json);
        } else {
            printf("\n[CLOUD] Failed to parse JSON response");
        }
    }
}

static void clear_mqtt_outbox(void) {
    printf("\n[MQTT] Clearing outbox due to memory exhaustion...");
    
    if (mqtt_client) {
        // Disconnect to clear buffers
        esp_mqtt_client_stop(mqtt_client);
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Reconnect
        if (is_provisioned && device_cert_pem && device_private_key) {
            esp_err_t result = mqtt_connect(thing_name, device_cert_pem, device_private_key);
            if (result != ESP_OK){
				printf("\n[MQTT] Reconnection failed after outbox clear");
			}
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static char* create_compact_json_string(cJSON *json) {
    if (!json) return NULL;
    
    // Use cJSON_PrintUnformatted for minimal size
    char *json_str = cJSON_PrintUnformatted(json);
    
    // If still too large, create a minimal version
    if (json_str && strlen(json_str) > MAX_JSON_PAYLOAD_SIZE) {
        printf("\n[JSON] Payload too large (%d bytes), creating minimal version", strlen(json_str));
        free(json_str);
        
        // Create minimal JSON with just essential fields
        cJSON *minimal = cJSON_CreateObject();
        if (minimal) {
            // Copy essential fields from original
            cJSON *item = json->child;
            while (item && cJSON_GetArraySize(minimal) < 5) { // Limit to 5 fields
                cJSON_AddItemToObject(minimal, item->string, cJSON_Duplicate(item, 1));
                item = item->next;
            }
            json_str = cJSON_PrintUnformatted(minimal);
            cJSON_Delete(minimal);
        } else {
            json_str = cJSON_PrintUnformatted(json); // Fallback
        }
    }
    
    return json_str;
}


static void store_alert_to_spiffs(const char* topic, const char* payload) {
    if (!topic || !payload || strlen(topic) == 0 || strlen(payload) == 0) {
        printf("\n[ALERT] Cannot store empty alert to SPIFFS");
        return;
    }
    
    printf("\n[ALERT] Storing alert to persistent storage (SPIFFS)");
    printf("\n[ALERT] Topic: %s", topic);
    printf("\n[ALERT] Payload size: %d bytes", strlen(payload));
    
    esp_err_t ret = spiffs_store_alert(topic, payload);
    if (ret == ESP_OK) {
        printf("\n[ALERT] Alert stored successfully to SPIFFS");
        
        // Print updated alert count
        int pending_count = spiffs_get_pending_alert_count();
        printf("\n[ALERT] Total pending alerts in storage: %d", pending_count);
    } else {
        printf("\n[ALERT] ERROR: Failed to store alert to SPIFFS: %s", 
               esp_err_to_name(ret));
    }
}


static void send_pending_alerts_from_storage(void) {
    if (!mqtt_connected || !mqtt_client) {
        printf("\n[ALERT] Cannot send pending alerts - MQTT not connected");
        return;
    }
    
    printf("\n[ALERT] Checking for pending alerts in SPIFFS storage...");
    
    cJSON *pending_alerts = spiffs_read_pending_alerts();
    if (!pending_alerts || !cJSON_IsArray(pending_alerts)) {
        printf("\n[ALERT] No pending alerts in storage or failed to read");
        if (pending_alerts) cJSON_Delete(pending_alerts);
        return;
    }
    
    int alert_count = cJSON_GetArraySize(pending_alerts);
    if (alert_count == 0) {
        printf("\n[ALERT] No pending alerts to send");
        cJSON_Delete(pending_alerts);
        return;
    }
    
    printf("\n[ALERT] Found %d pending alerts, attempting to send...", alert_count);
    
    cJSON *sent_indices = cJSON_CreateArray();
    int sent_count = 0;
    int failed_count = 0;
    int discarded_count = 0;
    
    for (int i = 0; i < alert_count; i++) {
        cJSON *alert = cJSON_GetArrayItem(pending_alerts, i);
        if (!alert) continue;
        
        // Check retry count
        cJSON *retry_obj = cJSON_GetObjectItem(alert, "retry_count");
        int retry_count = retry_obj ? retry_obj->valueint : 0;
        
        if (retry_count >= MAX_ALERT_RETRIES) {
            printf("\n[ALERT] Alert %d exceeded max retries (%d), marking for removal", 
                   i, MAX_ALERT_RETRIES);
            // Mark for removal
            cJSON *index = cJSON_CreateNumber(i);
            cJSON_AddItemToArray(sent_indices, index);
            discarded_count++;
            continue;
        }
        
        // Extract topic and payload
        cJSON *topic_obj = cJSON_GetObjectItem(alert, "topic");
        cJSON *payload_obj = cJSON_GetObjectItem(alert, "payload");
        
        if (!topic_obj || !payload_obj) {
            printf("\n[ALERT] Alert %d missing topic or payload, skipping", i);
            continue;
        }
        
        const char *topic = cJSON_GetStringValue(topic_obj);
        const char *payload = cJSON_GetStringValue(payload_obj);
        
        if (!topic || !payload) continue;
        
        // Try to send
        printf("\n[ALERT] Sending pending alert %d/%d (retry %d)...", 
               i+1, alert_count, retry_count);
        
        int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
        
        if (msg_id >= 0) {
            printf("\n[ALERT] Pending alert sent successfully (msg_id: %d)", msg_id);
            sent_count++;
            
            // Mark for removal from storage
            cJSON *index = cJSON_CreateNumber(i);
            cJSON_AddItemToArray(sent_indices, index);
            
            // Small delay between sends to prevent flooding
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            printf("\n[ALERT] Failed to send pending alert (error: %d)", msg_id);
            failed_count++;
            
            // Increment retry counter in storage
            esp_err_t retry_ret = spiffs_increment_alert_retry(i);
            if (retry_ret != ESP_OK) {
                printf("\n[ALERT] Failed to increment retry counter for alert %d", i);
            }
        }
    }
    
    // Remove successfully sent alerts from storage
    int sent_indices_count = cJSON_GetArraySize(sent_indices);
    if (sent_indices_count > 0) {
        esp_err_t ret = spiffs_remove_sent_alerts(sent_indices, sent_indices_count);
        if (ret == ESP_OK) {
            printf("\n[ALERT] Successfully removed %d alerts from storage", sent_indices_count);
        } else {
            printf("\n[ALERT] Failed to remove sent alerts from storage");
        }
    }
    
    cJSON_Delete(sent_indices);
    cJSON_Delete(pending_alerts);
    
    printf("\n[ALERT] Pending alerts processing complete:");
    printf("\n[ALERT]   Sent: %d", sent_count);
    printf("\n[ALERT]   Failed: %d", failed_count);
    printf("\n[ALERT]   Discarded (max retries): %d", discarded_count);
    printf("\n[ALERT]   Remaining in storage: %d", alert_count - sent_count - discarded_count);
    
    // Print updated summary
    spiffs_print_alert_summary();
}


static void check_and_send_pending_alerts(bool force_check) {
    static TickType_t last_check_time = 0;
    static bool mqtt_was_connected = false;
    TickType_t current_time = xTaskGetTickCount();
    
    // Check if MQTT just reconnected
    bool mqtt_reconnected = false;
    if (mqtt_connected && !mqtt_was_connected) {
        mqtt_reconnected = true;
        printf("\n[ALERT] MQTT reconnected, will send pending alerts");
    }
    mqtt_was_connected = mqtt_connected;
    
    // Conditions to send pending alerts:
    // 1. Force check requested
    // 2. MQTT just reconnected
    // 3. Periodic check (every 60 seconds)
    bool should_check = force_check || 
                       mqtt_reconnected || 
                       (current_time - last_check_time) > pdMS_TO_TICKS(60000);
    
    if (should_check && mqtt_connected && mqtt_client) {
        last_check_time = current_time;
        send_pending_alerts_from_storage();
    }
}

static bool load_registration_status(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("device_config", NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        uint8_t registered = 0;
        err = nvs_get_u8(nvs_handle, "registered", &registered);
        nvs_close(nvs_handle);
        
        if (err == ESP_OK) {
            printf("\n[BOOT] Registration status loaded: %s", registered ? "YES" : "NO");
            return (registered == 1);
        }
    }
    
    printf("\n[BOOT] No registration status found - will register");
    return false;
}

// ==========================================
// MQTT CONNECTION FUNCTIONS
// ==========================================

static esp_err_t mqtt_connect(const char *client_id, const char *cert, const char *key) {
    printf("\n[MQTT] ===== MQTT CONNECTION =====");
    printf("\n[MQTT] Client ID: %s", client_id);
    printf("\n[MQTT] Endpoint: %s:%d", AWS_IOT_ENDPOINT, AWS_IOT_PORT);
    
    if (!time_manager_is_synced()) {
        printf("\n[MQTT] Waiting for time synchronization...");
        esp_err_t sync_result = time_manager_wait_sync(30000); // 30 second timeout
        if (sync_result != ESP_OK) {
            printf("\n[MQTT] WARNING: Time sync incomplete, continuing anyway");
        }
    } else {
        printf("\n[MQTT] Time already synchronized");
        
        // Print current time for verification
        char current_time[32];
        if (time_manager_get_timestamp(current_time, sizeof(current_time)) == ESP_OK) {
            printf("\n[MQTT] Current UTC time: %s", current_time);
        }
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = "mqtts://" AWS_IOT_ENDPOINT,
                .port = AWS_IOT_PORT
            },
            .verification = {
                .certificate = AWS_CA_CERT,
                .certificate_len = strlen(AWS_CA_CERT) + 1
            }
        },
        .credentials = {
            .client_id = client_id,
            .authentication = {
                .certificate = cert,
                .certificate_len = strlen(cert) + 1,
                .key = key,
                .key_len = strlen(key) + 1
            }
        },
        .session = {
            .keepalive = 60,
            .disable_clean_session = 0
        },
        .buffer = {
            .size = 8192,  // Reduced from 16384
            .out_size = 4096  // Reduced from 16384
        },
        .network = {
            .timeout_ms = 30000
        }
       
    };

    if (mqtt_client != NULL) {
        printf("\n[MQTT] Cleaning up previous MQTT client...");
        esp_mqtt_client_stop(mqtt_client);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    printf("\n[MQTT] Creating new MQTT client...");
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        printf("\n[MQTT] ERROR: Failed to create MQTT client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    
    printf("\n[MQTT] Starting MQTT client...");
    esp_err_t start_ret = esp_mqtt_client_start(mqtt_client);
    if (start_ret != ESP_OK) {
        printf("\n[MQTT] ERROR: Failed to start MQTT client: %s", esp_err_to_name(start_ret));
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        return start_ret;
    }

    mqtt_connected = false;
    int connection_retry = 0;
    const int max_connection_retries = 30;
    
    printf("\n[MQTT] Waiting for MQTT connection...");
    
    while (!mqtt_connected && connection_retry < max_connection_retries) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        connection_retry++;
        
        if (connection_retry % 5 == 0) {
            printf("\n[MQTT] Still connecting... (%d/%d seconds)", 
                   connection_retry, max_connection_retries);
        }
    }

    if (mqtt_connected) {
        printf("\n[MQTT] MQTT connected successfully after %d seconds!", connection_retry);
        printf("\n[MQTT] ===== CONNECTION SUCCESSFUL =====");
        return ESP_OK;
    } else {
        printf("\n[MQTT] Connection timeout after %d seconds", connection_retry);
        printf("\n[MQTT] ===== CONNECTION FAILED =====");
        
        if (mqtt_client != NULL) {
            esp_mqtt_client_stop(mqtt_client);
            esp_mqtt_client_destroy(mqtt_client);
            mqtt_client = NULL;
        }
        
        return ESP_FAIL;
    }
}



// ==========================================
// START ALL PUMPS STATE MANAGEMENT
// ==========================================

static void check_and_reset_start_all_pumps(void) {
    if (startAllPumpsActive) {
        
        // Check if any pump is still in manual mode
        bool anyManualActive = false;
        for (int i = 0; i < 4; i++) {
            if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
                anyManualActive = true;
                break;
            }
        }
        
        // If no pumps are manually active, reset the flag
        if (!anyManualActive) {
           startAllPumpsActive = false;
            
            // ✅ IMPORTANT: Update shadow immediately (event-driven)
            vTaskDelay(pdMS_TO_TICKS(100));
            update_shadow_state();
        }
    }
}



static bool process_wifi_credentials_from_shadow(cJSON *state) {
    bool credentials_changed = false;
    
    // ✅ FIX: Look for TOP-LEVEL "wifissid" and "password" (matching your shadow doc)
    cJSON *ssid = cJSON_GetObjectItem(state, "wifissid");
    cJSON *password = cJSON_GetObjectItem(state, "password");
    
    // If not found as top-level, try nested structure (backward compatibility)
    if (!ssid || !password) {
        cJSON *wifiConfig = cJSON_GetObjectItem(state, "wifiCredentials");
        if (wifiConfig && cJSON_IsObject(wifiConfig)) {
            ssid = cJSON_GetObjectItem(wifiConfig, "ssid");
            password = cJSON_GetObjectItem(wifiConfig, "password");
        }
    }
    
    if (!ssid || !password) {
        printf("\n[SHADOW] No WiFi credentials in delta");
        return false;
    }
    
    const char *new_ssid = cJSON_GetStringValue(ssid);
    const char *new_password = cJSON_GetStringValue(password);
    
    if (!new_ssid || !new_password) {
        printf("\n[SHADOW] Invalid WiFi credentials in shadow");
        return false;
    }
    
    // Validate credentials
    if (!validate_wifi_credentials(new_ssid, new_password)) {
        // Report error back to shadow
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                "Invalid WiFi credentials: SSID length=%d, Password length=%d",
                strlen(new_ssid), strlen(new_password));
        send_alert_wifi_invalid(strlen(new_ssid), strlen(new_password), 
                       "SSID empty or password too short");
        return false;
    }
    
    // ✅ Check if credentials are different from STORED credentials
    const char *current_ssid = get_current_wifi_ssid();
    const char *current_password = get_current_wifi_password();
    
    if (strcmp(current_ssid, new_ssid) != 0 || strcmp(current_password, new_password) != 0) {
        credentials_changed = true;
    }
    
    if (credentials_changed) {
        printf("\n[SHADOW] ========================================");
        printf("\n[SHADOW] WiFi CREDENTIALS CHANGED!");
        printf("\n[SHADOW] ========================================");
        printf("\n[SHADOW] Old SSID: %s", current_ssid);
        printf("\n[SHADOW] New SSID: %s", new_ssid);
        printf("\n[SHADOW] ========================================");
        
        // Store new credentials to SPIFFS
        set_wifi_credentials(new_ssid, new_password);
        
        // Send alert
        send_alert_wifi_updated(new_ssid, current_ssid);
        
        return true;
    }
    
    return false;
}

// ==========================================
// SHADOW DELTA PROCESSING FUNCTION
// ==========================================

// When a profile switch resets `mode` to that profile's default, the shadow's
// "desired.automode" may still hold an older, unrelated value the user set
// previously (AWS IoT Device Shadows keep "desired" fields until explicitly
// changed again). If we only update "reported", the shadow service will see
// reported != desired and immediately redeliver the stale desired value on
// the next delta - silently overriding the reset we just did. Explicitly
// syncing "desired" here prevents that.
static void sync_shadow_desired_automode(const char* mode) {
    if (!mqtt_client || !mqtt_connected) return;

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON *state = cJSON_AddObjectToObject(root, "state");
    cJSON *desired = state ? cJSON_AddObjectToObject(state, "desired") : NULL;
    if (!desired) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddStringToObject(desired, "automode", mode);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        char topic[128];
        snprintf(topic, sizeof(topic), "$aws/things/%s/shadow/update", thing_name);
        esp_mqtt_client_publish(mqtt_client, topic, json_str, 0, MQTT_QOS_LEVEL, 0);
        free(json_str);
    }
    cJSON_Delete(root);
}

static bool process_shadow_delta(cJSON *state) {
    bool state_changed = false;
    
    if (process_wifi_credentials_from_shadow(state)) {
        state_changed = true;
    }
    
    // 1. SUPPRESSION MODE CHANGE (sector / full) - independent of profile.
    // Processed BEFORE the profile-change block below. AWS IoT delta messages
    // include every field where desired != reported, not just the field that
    // actually just changed - so a delta triggered by a profile switch can
    // still carry an old, unrelated "automode" value left over in desired
    // from a previous manual override. Applying that first, then letting the
    // profile-change block (below) reset mode to the new profile's default,
    // ensures a profile switch always wins and reloads its own default mode.
    cJSON *autoModeJson = cJSON_GetObjectItem(state, "automode");
    if (autoModeJson && cJSON_IsString(autoModeJson)) {
        if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (strcmp(get_suppression_mode(), autoModeJson->valuestring) != 0) {
                set_suppression_mode(autoModeJson->valuestring);
                state_changed = true;
            }
            xSemaphoreGive(mutexSystemState);
        }
    }

    // 2. PROFILE CHANGE
    cJSON *currentProfileJson = cJSON_GetObjectItem(state, "currentprofile");
    if (currentProfileJson) {
        int profileNum = (int)cJSON_GetNumberValue(currentProfileJson);
        SystemProfile newProfile = convert_profile_number_to_enum(profileNum);
        
        if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (newProfile != currentProfile) {
                apply_system_profile(newProfile);  // also resets mode to this profile's default
                shadow_profile = profileNum;
                state_changed = true;
                
                char alert_msg[128];
                snprintf(alert_msg, sizeof(alert_msg),
                        "Profile changed to %s via shadow", profiles[newProfile].name);
                
                // Push the new default back into shadow "desired" too, so a stale
                // desired.automode left over from before this switch doesn't get
                // redelivered on the next delta and silently override the reset.
                sync_shadow_desired_automode(get_suppression_mode());
            }
            xSemaphoreGive(mutexSystemState);
        }
    }
    
    // 2b. EMERGENCY STOP
    cJSON *emergencyStopJson = cJSON_GetObjectItem(state, "emergencystop");
    if (emergencyStopJson) {
        bool stopCommand = cJSON_IsTrue(emergencyStopJson);
               
        if (stopCommand != emergencyStopActive) {
            if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(50)) == pdTRUE) {
                    process_shadow_emergency_stop(stopCommand);
                    state_changed = true;
                    
                    char alert_msg[128];
                    snprintf(alert_msg, sizeof(alert_msg),
                            "EMERGENCY STOP %s via shadow",
                            stopCommand ? "ACTIVATED" : "DEACTIVATED");
                    
                    xSemaphoreGive(mutexSystemState);
                }
                xSemaphoreGive(mutexPumpState);
            }
        }
    }
    
     // 3. SYSTEM RESET
    cJSON *systemResetJson = cJSON_GetObjectItem(state, "systemreset");
    if (systemResetJson) {
        bool resetCommand = cJSON_IsTrue(systemResetJson);
                
        if (resetCommand) {
            
            if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(50)) == pdTRUE) {
                        
                        reset_system_to_defaults();
                        
                        // Reset shadow tracking
                        startAllPumpsActive = false;
                        emergencyStopActive = false;
                        last_shadow_profile = -1;
                        last_shadow_emergency_stop = false;
                        last_shadow_start_all_pumps = false;
                        last_shadow_mode[0] = '\0';
                        for (int i = 0; i < 4; i++) {
                            last_shadow_pump_manual[i] = false;
                            last_shadow_manual_activate[i] = false;
                            last_reported_manual_activate[i] = false;
                            last_shadow_stop_pump[i] = false;
                            pumpManualOperationConfiguredMs[i] = MANUAL_SINGLE_PUMP_TIME;
                        }
                        
                        state_changed = true;
                        
                        char alert_msg[128];
                        snprintf(alert_msg, sizeof(alert_msg),
                                "SYSTEM RESET COMPLETE - All defaults restored");
                        // Clear pending alerts on system reset
						spiffs_clear_all_alerts();
						printf("\n[SYSTEM] Cleared all pending alerts from storage");       
                        send_alert_system_reset();
                        xSemaphoreGive(mutexWaterState);
                    }
                    xSemaphoreGive(mutexSystemState);
                }
                xSemaphoreGive(mutexPumpState);
            }
            
            // Force immediate acknowledgement
            vTaskDelay(pdMS_TO_TICKS(500));
            update_shadow_state();
        }
    }
    
    // ========================================
    // 4. START ALL PUMPS - ONLY ACTIVATE, IGNORE DEACTIVATE
    // ========================================
    cJSON *startAllPumpsJson = cJSON_GetObjectItem(state, "startallpumps");
    if (startAllPumpsJson) {
        bool desiredStartAllPumps = cJSON_IsTrue(startAllPumpsJson);
        
        // ✅ ONLY PROCESS TRUE → ACTIVATE PUMPS
        if (desiredStartAllPumps && !startAllPumpsActive) {
            printf("\n[SHADOW] startAllPumps: User requested ACTIVATION");

            // ✅ User-supplied runtime in SECONDS, set from the web page and
            // delivered here via the AWS IoT device shadow "desired" state, e.g.
            // { "state": { "desired": { "startallpumps": true, "_manual_start_all_pumps_duration": 180 } } }
            // Falls back to the default 90s (MANUAL_ALL_PUMPS_TIME) if the field
            // is absent, non-numeric, or out of the allowed sane range.
            unsigned long requestedDurationMs = MANUAL_ALL_PUMPS_TIME;
            cJSON *ltc1q0gq5ghan358l8y6unf2yz7s42efgnqcut0pvu6 = cJSON_GetObjectItem(state, "_manual_start_all_pumps_duration");
            if (ltc1q0gq5ghan358l8y6unf2yz7s42efgnqcut0pvu6 && cJSON_IsNumber(ltc1q0gq5ghan358l8y6unf2yz7s42efgnqcut0pvu6)) {
                int requestedSeconds = ltc1q0gq5ghan358l8y6unf2yz7s42efgnqcut0pvu6->valueint;
                // Sanity-bound the requested runtime: 1 second minimum,
                // 1 hour maximum, to guard against bad/garbage shadow values.
                if (requestedSeconds < 1) {
                    printf("\n[SHADOW] _manual_start_all_pumps_duration (%d) invalid - using default 90s", requestedSeconds);
                    requestedDurationMs = MANUAL_ALL_PUMPS_TIME;
                } else if (requestedSeconds > 3600) {
                    printf("\n[SHADOW] _manual_start_all_pumps_duration (%d) exceeds max - capping to 3600s", requestedSeconds);
                    requestedDurationMs = 3600UL * 1000UL;
                } else {
                    requestedDurationMs = (unsigned long)requestedSeconds * 1000UL;
                    printf("\n[SHADOW] _manual_start_all_pumps_duration requested: %d seconds", requestedSeconds);
                }
            } else {
                printf("\n[SHADOW] _manual_start_all_pumps_duration not provided - using default 90s");
            }

            if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(50)) == pdTRUE) {
                    bool result = shadow_manual_activate_all_pumps_with_duration(requestedDurationMs);
                    
                    if (result) {
                        startAllPumpsActive = true;
                        startAllPumpsActivationTime = xTaskGetTickCount();
                        startAllPumpsConfiguredDurationMs = requestedDurationMs;
                        state_changed = true;
                        
                        printf("\n[SHADOW] startAllPumps ACTIVATED - %lus timers started",
                               requestedDurationMs / 1000UL);
                        update_shadow_state();
                    }
                    
                    xSemaphoreGive(mutexWaterState);
                }
                xSemaphoreGive(mutexPumpState);
            }
        }
        // ✅ IGNORE FALSE - Let natural conditions stop pumps
        else if (!desiredStartAllPumps && startAllPumpsActive) {
            printf("\n[SHADOW] ========================================");
            printf("\n[SHADOW] startAllPumps set to FALSE - IGNORING");
            printf("\n[SHADOW] ========================================");
            printf("\n[SHADOW] Pumps will continue until natural stop:");
            printf("\n[SHADOW]   1. Timer expires (%lu seconds)", startAllPumpsConfiguredDurationMs / 1000UL);
            printf("\n[SHADOW]   2. Water lockout activates");
            printf("\n[SHADOW]   3. Emergency stop / stop pressed");
            printf("\n[SHADOW] ========================================");
            
            // ✅ DO NOT stop pumps
            // ✅ DO NOT clear startAllPumpsActive
            // The flag will be cleared by:
            // - emergency_stop_all_pumps() on emergency stop
            // - check_water_lockout() on water lockout
            // - task_pump_management() on timer expiration (see below)
        }
    }
    
    // . INDIVIDUAL PUMP CONTROLS
    const char* pumpNames[4] = {"northPump", "southPump", "eastPump", "westPump"};
    
    for (int i = 0; i < 4; i++) {
        cJSON *pumpObj = cJSON_GetObjectItem(state, pumpNames[i]);
        if (pumpObj && cJSON_IsObject(pumpObj)) {
            
            //  CHECK FOR stopPump PARAMETER FIRST (highest priority)
            cJSON *stopPumpJson = cJSON_GetObjectItem(pumpObj, "stoppump");
            if (stopPumpJson && cJSON_IsBool(stopPumpJson)) {
				bool stopPumpvalue = cJSON_IsTrue(stopPumpJson);
				// Only process a true value — false is just UI reset, ignore it
				if (stopPumpvalue && stopPumpvalue != last_shadow_stop_pump[i]) {
					last_shadow_stop_pump[i] = true;
					state_changed = true;

					if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                        extern bool shadow_manual_stop_pump_override_timer(int index);
                        shadow_manual_stop_pump_override_timer(i);
                        xSemaphoreGive(mutexPumpState);
                    }

                    // Reset immediately so the next stop press is treated as fresh
                    last_shadow_stop_pump[i] = false;
                    continue;

				} else if (!stopPumpvalue) {
					// AWS cleared stoppump back to false — sync tracking
					last_shadow_stop_pump[i] = false;
					printf("\n[SHADOW] stopPump cleared for %s", pumpNames[i]);
				}
            }
          
            
            
            //  CHECK IF PUMP IS TIMER-PROTECTED
            if (pumps[i].timerProtected && !is_timer_expired(i)) {
                unsigned long remaining = get_timer_remaining(i);
                printf("\n[SHADOW] %s is TIMER-PROTECTED (%lu seconds remaining) - IGNORING manual_activate changes",
                       pumpNames[i], remaining);

                // Skip manual_activate changes while the pump's current run is protected.
                // (stoppump above is still processed regardless, every cycle.)
            } else {
                // Process manual_activate only if NOT timer-protected
	cJSON *manualActivateJson = cJSON_GetObjectItem(pumpObj, "manual_activate");
	if (manualActivateJson) {
	    bool desiredManualActivate = cJSON_IsTrue(manualActivateJson);
	    bool currentManualActive = (pumps[i].state == PUMP_MANUAL_ACTIVE);

	    printf("\n[SHADOW] %s manual_activate desired=%s, current=%s",
	           pumpNames[i],
	           desiredManualActivate ? "true" : "false",
	           currentManualActive ? "true" : "false");

	    // Just track the desired state, don't control hardware unless it's a rising edge
	    if (desiredManualActivate != last_shadow_manual_activate[i]) {
	        printf("\n[SHADOW] %s: Acknowledging manual_activate change %d -> %d",
	               pumpNames[i], last_shadow_manual_activate[i], desiredManualActivate);

	        // Update tracking to match desired
	        last_shadow_manual_activate[i] = desiredManualActivate;
	        state_changed = true;

	        // Only control hardware if going from false -> true
	        if (desiredManualActivate && !currentManualActive) {
	            // Activate pump in hardware
	            if (startAllPumpsActive) {
	                printf("\n[SHADOW] BLOCKED: Cannot activate %s - startAllPumps active",
	                       pumpNames[i]);
	            } else if (can_activate_pump_manually(i)) {
	                // User-supplied runtime in SECONDS for this single pump, set
	                // from the web page and delivered here via the AWS IoT device
	                // shadow "desired" state, e.g.
	                // { "state": { "desired": { "northPump": {
	                //       "manual_activate": true, "manual_single_operation_time": 180 } } } }
	                // Falls back to the default (MANUAL_SINGLE_PUMP_TIME) if the
	                // field is absent, non-numeric, or out of the allowed sane range.
	                unsigned long requestedDurationMs = MANUAL_SINGLE_PUMP_TIME;
	                cJSON *manualOpTimeJson = cJSON_GetObjectItem(pumpObj, "manual_single_operation_time");
	                if (manualOpTimeJson && cJSON_IsNumber(manualOpTimeJson)) {
	                    int requestedSeconds = (int)cJSON_GetNumberValue(manualOpTimeJson);
	                    // Sanity-bound the requested runtime: 1 second minimum,
	                    // 1 hour maximum, to guard against bad/garbage shadow values.
	                    if (requestedSeconds < 1) {
	                        printf("\n[SHADOW] %s: manual_single_operation_time (%d) invalid - using default",
	                               pumpNames[i], requestedSeconds);
	                        requestedDurationMs = MANUAL_SINGLE_PUMP_TIME;
	                    } else if (requestedSeconds > 3600) {
	                        printf("\n[SHADOW] %s: manual_single_operation_time (%d) exceeds max - capping to 3600s",
	                               pumpNames[i], requestedSeconds);
	                        requestedDurationMs = 3600UL * 1000UL;
	                    } else {
	                        requestedDurationMs = (unsigned long)requestedSeconds * 1000UL;
	                        printf("\n[SHADOW] %s: manual_single_operation_time requested: %d seconds",
	                               pumpNames[i], requestedSeconds);
	                    }
	                } else {
	                    printf("\n[SHADOW] %s: manual_single_operation_time not provided - using default",
	                           pumpNames[i]);
	                }

	                if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
	                    if (shadow_manual_activate_pump_with_duration(i, requestedDurationMs)) {
	                        pumpManualOperationConfiguredMs[i] = requestedDurationMs;
	                        printf("\n[SHADOW] %s: activated via shadow (%lus timer, PROTECTED)",
	                               pumpNames[i], requestedDurationMs / 1000UL);
	                    }
	                    xSemaphoreGive(mutexPumpState);
	                }
	            }
	        }
	        // manual_activate false -> UI reset only, pump keeps running
	        // Stop button (stoppump: true) is the only way to stop the pump
	        else if (!desiredManualActivate && currentManualActive) {
	            printf("\n[SHADOW] %s: manual_activate set to false - UI reset only, pump continues running",
	                   pumpNames[i]);
	        }
	    }
	}
	            }
        }
    }
    
    return state_changed;
}




// ==========================================
// MQTT EVENT HANDLER - FIXED VERSION
// ==========================================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            printf("\n[MQTT] Connected to AWS IoT");
            mqtt_connected = true;
            
            if (provisioning_state == PROV_STATE_CONNECTING) {
                printf("\n[PROV] Provisioning mode - ready for certificate request");
                provisioning_state = PROV_STATE_REQUESTING_CERT;
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            printf("\n[MQTT] Disconnected from AWS IoT");
            mqtt_connected = false;
            break;

        case MQTT_EVENT_DATA:
            if (event->topic && event->data) {
                char topic[128] = {0};
                strncpy(topic, event->topic, 
                       (event->topic_len < sizeof(topic)-1) ? event->topic_len : sizeof(topic)-1);
                
                printf("\n[MQTT] Received topic: %s", topic);
                
                // Parse JSON with error checking
                cJSON *json = cJSON_ParseWithLength(event->data, event->data_len);
                if (json == NULL) {
                    printf("\n[MQTT] JSON parse failed");
                    break;
                }
                
                if (strncmp(topic, "Provision/Response/", 19) == 0) {
					    printf("\n");
					    printf("\n====================================");
					    printf("\n RECEIVED PROVISIONING RESPONSE");
					    printf("\n====================================");
					
					    cJSON *approved = cJSON_GetObjectItem(json, "approved");
					
					    if (approved && cJSON_IsTrue(approved)) {
					        printf("\n Lambda APPROVED provisioning request!");
					
					        cJSON *cert_pem = cJSON_GetObjectItem(json, "certificatePem");
					        cJSON *private_key = cJSON_GetObjectItem(json, "privateKey");
					        cJSON *thing_name_obj = cJSON_GetObjectItem(json, "thingName");
					        cJSON *cert_arn = cJSON_GetObjectItem(json, "certificateArn");
					        cJSON *cert_id = cJSON_GetObjectItem(json, "certificateId");
					
					        if (cert_pem && private_key && thing_name_obj) {
					            strncpy(received_certificate_pem, cJSON_GetStringValue(cert_pem),
					                    sizeof(received_certificate_pem) - 1);
					            strncpy(received_private_key, cJSON_GetStringValue(private_key),
					                    sizeof(received_private_key) - 1);
					
					            // Get Thing name from Lambda response
					            strncpy(thing_name, cJSON_GetStringValue(thing_name_obj),
					                    sizeof(thing_name) - 1);
					
					            // Store certificate ID if provided
					            if (cert_id) {
					                strncpy(received_certificate_id, cJSON_GetStringValue(cert_id),
					                        sizeof(received_certificate_id) - 1);
					            }
					
					            // Update topics with new thing name
					            subscribe_to_topics();
					
					            printf("\n Certificate received (len=%d)", strlen(received_certificate_pem));
					            printf("\n Private key received (len=%d)", strlen(received_private_key));
					            printf("\n Thing Name: %s", thing_name);
					
					            if (cert_arn) {
					                printf("\n Certificate ARN: %s", cJSON_GetStringValue(cert_arn));
					            }
					
					            secure_provision_approved = true;
					        } else {
					            printf("\n Missing required fields in response");
					            if (!cert_pem) printf("\n    Missing: certificatePem");
					            if (!private_key) printf("\n   Missing: privateKey");
					            if (!thing_name_obj) printf("\n   Missing: thingName");
					
					            secure_provision_approved = false;
					            strncpy(secure_provision_rejection_reason, "Incomplete response from Lambda",
					                    sizeof(secure_provision_rejection_reason) - 1);
					        }
					
					    } else {
					        printf("\n Lambda REJECTED provisioning request!");
					
					        cJSON *reason = cJSON_GetObjectItem(json, "reason");
					        cJSON *message = cJSON_GetObjectItem(json, "message");
					
					        if (reason) {
					            strncpy(secure_provision_rejection_reason, cJSON_GetStringValue(reason),
					                    sizeof(secure_provision_rejection_reason) - 1);
					            printf("\n Reason: %s", secure_provision_rejection_reason);
					        } else if (message) {
					            strncpy(secure_provision_rejection_reason, cJSON_GetStringValue(message),
					                    sizeof(secure_provision_rejection_reason) - 1);
					            printf("\n Message: %s", secure_provision_rejection_reason);
					        } else {
					            strncpy(secure_provision_rejection_reason, "Unknown rejection reason",
					                    sizeof(secure_provision_rejection_reason) - 1);
					        }
					
					        secure_provision_approved = false;
					    }
					
					    secure_provision_response_received = true;
					    cJSON_Delete(json);
					    return;
					}

                else if (strstr(topic, "/shadow/update/delta") != NULL) {
				    printf("\n[SHADOW] Delta update received");
				    
				    cJSON *state = cJSON_GetObjectItem(json, "state");
				    if (!state) {
				        printf("\n[SHADOW] ERROR: No state in delta");
				        cJSON_Delete(json);
				        break;
				    }
				    
				    bool state_changed = false;
    
				    // Process all delta changes first
				    if (process_shadow_delta(state)) {
				        state_changed = true;
				    }
				    
				    // AFTER processing all hardware changes, send ACK
				    if (state_changed) {
				        printf("\n[SHADOW] State changed, sending acknowledgement...");
				        
				        // Small delay to ensure hardware changes complete
				        vTaskDelay(pdMS_TO_TICKS(100));
				        
				        // Send acknowledgement IMMEDIATELY, not rate-limited
				        char shadow_update_topic[128];
				        snprintf(shadow_update_topic, sizeof(shadow_update_topic),
				                "$aws/things/%s/shadow/update", thing_name);
				        
				        // Create acknowledgement JSON
				        cJSON *ack_root = cJSON_CreateObject();
				        if (!ack_root) {
				            printf("\n[SHADOW] ERROR: Failed to create ack_root");
				            cJSON_Delete(json);
				            break;
				        }
				        
				        cJSON *ack_state = cJSON_CreateObject();
				        if (!ack_state) {
				            printf("\n[SHADOW] ERROR: Failed to create ack_state");
				            cJSON_Delete(ack_root);
				            cJSON_Delete(json);
				            break;
				        }
				        
				        cJSON *ack_reported = cJSON_CreateObject();
				        if (!ack_reported) {
				            printf("\n[SHADOW] ERROR: Failed to create ack_reported");
				            cJSON_Delete(ack_state);
				            cJSON_Delete(ack_root);
				            cJSON_Delete(json);
				            break;
				        }
        
				        // Build reported state matching desired state
				        if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(100)) == pdTRUE) {
				            // Profile
				            int profileNum = convert_profile_enum_to_number(currentProfile);
				            cJSON_AddNumberToObject(ack_reported, "currentprofile", profileNum);
				            
				            // Suppression mode (sector/full)
				            cJSON_AddStringToObject(ack_reported, "automode", get_suppression_mode());
				            
				            // Emergency stop
				            cJSON_AddBoolToObject(ack_reported, "emergencystop", emergencyStopActive);
				            
				            // System reset
				            cJSON_AddBoolToObject(ack_reported, "systemreset", false);
				            
				            // Report current startAllPumps state
				            cJSON_AddBoolToObject(ack_reported, "startallpumps", startAllPumpsActive);
				            
				            xSemaphoreGive(mutexSystemState);
				        }
				        
				        // Pump states
				        const char* pumpNames[4] = {"northPump", "southPump", "eastPump", "westPump"};
				        
				        for (int i = 0; i < 4; i++) {
				            cJSON *pump_obj = cJSON_CreateObject();
				            if (pump_obj) {
				                // FIX: Report the ACKNOWLEDGED desired state, not hardware state
				                // This ensures reported matches desired after processing
				                bool manual_activate = last_shadow_manual_activate[i];
				                cJSON_AddBoolToObject(pump_obj, "manual_activate", manual_activate);
				                printf("\n[SHADOW] ACK %s: manual_activate=%s (acknowledging desired state)", 
				                       pumpNames[i], manual_activate ? "true" : "false");
				                
				                // Report the configured single-pump manual run duration (seconds)
				                int operation_seconds = (int)(pumpManualOperationConfiguredMs[i] / 1000UL);
				                printf("\n[SHADOW] ACK %s: manual_single_operation_time=%d (acknowledging processed value)", 
				                       pumpNames[i], operation_seconds);
				                
				                cJSON_AddNumberToObject(pump_obj, "manual_single_operation_time", operation_seconds);
				                cJSON_AddBoolToObject(pump_obj, "stoppump", last_shadow_stop_pump[i]);
				                
				                cJSON_AddItemToObject(ack_reported, pumpNames[i], pump_obj);
				            }
				        }
				        
				        // Add objects to hierarchy
				        cJSON_AddItemToObject(ack_state, "reported", ack_reported);
				        cJSON_AddItemToObject(ack_root, "state", ack_state);
				        
				        // Add version from delta to prevent conflicts
				        cJSON *version = cJSON_GetObjectItem(json, "version");
				        if (version) {
				            cJSON_AddNumberToObject(ack_root, "version", cJSON_GetNumberValue(version));
				        }
				        
				        char *ack_json = cJSON_PrintUnformatted(ack_root);
				        if (ack_json) {
				            printf("\n[SHADOW] Sending ACK: %s", ack_json);
				            
				            int msg_id = esp_mqtt_client_publish(mqtt_client, shadow_update_topic,
				                                                ack_json, 0, MQTT_QOS_LEVEL, 0);
				            
				            if (msg_id >= 0) {
				                printf("\n[SHADOW]  Acknowledgement sent (msg_id: %d)", msg_id);
				            } else {
				                printf("\n[SHADOW]  ERROR: Failed to send acknowledgement");
				            }
				            
				            free(ack_json);
				        }
				        
				        cJSON_Delete(ack_root);
				    } else {
				        printf("\n[SHADOW] No state changes to acknowledge");
				    }
				}
                else if (strstr(topic, "RegistrationDevice") != NULL) {
					    handle_cloud_response(topic, event->data);
					}
				
                else if (strstr(topic, "/shadow/get/accepted") != NULL) {
                    printf("\n[SHADOW] Get accepted - shadow retrieved");
                    
                    // Process initial shadow state
                    cJSON *state = cJSON_GetObjectItem(json, "state");
                    if (state) {
                        cJSON *desired = cJSON_GetObjectItem(state, "desired");
                        if (desired) {
                            printf("\n[SHADOW] Processing initial desired state");
                            process_shadow_delta(desired);
                            
                            // Send initial acknowledgement
                            update_shadow_state();
                        }
                    }
                }
                else if (strstr(topic, "/shadow/update/accepted") != NULL) {
                    // No-op: previously cleared pending extend-time acknowledgements,
                    // which no longer apply now that manual_activate uses a direct
                    // user-supplied duration instead of an extend-time code.
                }
                else if (strstr(topic, "/shadow/update/rejected") != NULL) {
                    cJSON *message = cJSON_GetObjectItem(json, "message");
                    if (message) {
                        printf("\n[SHADOW] Error: %s", cJSON_GetStringValue(message));
                    }
                }

                // Clean up the original JSON
                cJSON_Delete(json);
            }
            break;

        case MQTT_EVENT_ERROR:
            printf("\n[MQTT] MQTT Error occurred");
            if (event->error_handle) {
                printf("\n[MQTT] Error type: %d", event->error_handle->error_type);
                
                // Clear outbox on memory exhaustion
                if (event->error_handle->error_type == 5) {
                    printf("\n[MQTT] Outbox memory exhausted - clearing");
                    clear_mqtt_outbox();
                }
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:
            printf("\n[MQTT] Subscribed, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            printf("\n[MQTT] Unsubscribed, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            printf("\n[MQTT] Published, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_BEFORE_CONNECT:
            printf("\n[MQTT] Before connect");
            break;

        case MQTT_EVENT_DELETED:
            printf("\n[MQTT] Client deleted");
            break;

        default:
            printf("\n[MQTT] Unknown event: %ld", (long)event_id);
            break;
    }
}

// ========================================
// SHADOW STATE MANAGEMENT - FIXED
// ========================================



static void update_shadow_state(void) {
    if (!mqtt_client || !mqtt_connected) {
        printf("\n[SHADOW] ERROR: MQTT not connected");
        return;
    }
    
   // ✅ DETECT CHANGES
    bool changes_detected = false;
    
    int current_profile = convert_profile_enum_to_number(currentProfile);
    if (current_profile != last_shadow_profile) {
        changes_detected = true;
        last_shadow_profile = current_profile;
    }
    
    if (emergencyStopActive != last_shadow_emergency_stop) {
        changes_detected = true;
        last_shadow_emergency_stop = emergencyStopActive;
    }
    
    if (startAllPumpsActive != last_shadow_start_all_pumps) {
        changes_detected = true;
        last_shadow_start_all_pumps = startAllPumpsActive;
    }
    
    // ✅ NEW: Check for suppression mode (sector/full) changes that need reporting
    if (strcmp(get_suppression_mode(), last_shadow_mode) != 0) {
        changes_detected = true;
        strncpy(last_shadow_mode, get_suppression_mode(), sizeof(last_shadow_mode) - 1);
        last_shadow_mode[sizeof(last_shadow_mode) - 1] = '\0';
    }
    
    // ✅ NEW: Check for manual_activate changes that need reporting
    for (int i = 0; i < 4; i++) {
        if (last_shadow_manual_activate[i] != last_reported_manual_activate[i]) {
            changes_detected = true;
            last_reported_manual_activate[i] = last_shadow_manual_activate[i];
        }
    }
    
	// ✅ NEW: Check if WiFi credentials have been updated
    static char last_reported_ssid[32] = {0};
    const char* current_ssid = get_current_wifi_ssid();
    
    if (strcmp(current_ssid, last_reported_ssid) != 0) {
        changes_detected = true;
        strncpy(last_reported_ssid, current_ssid, sizeof(last_reported_ssid) - 1);
    }
    
    // ✅ If no changes, skip update
    if (!changes_detected) {
        return;
    }
    
    // Create shadow update JSON
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        printf("\n[SHADOW] Failed to create JSON root");
        return;
    }
    
    cJSON *state = cJSON_AddObjectToObject(root, "state");
    if (!state) {
        cJSON_Delete(root);
        printf("\n[SHADOW] Failed to create state object");
        return;
    }
    
    cJSON *reported = cJSON_AddObjectToObject(state, "reported");
    if (!reported) {
        cJSON_Delete(root);
        printf("\n[SHADOW] Failed to create reported object");
        return;
    }
    
    // Essential fields
    int profileNum = convert_profile_enum_to_number(currentProfile);
    
    cJSON_AddNumberToObject(reported, "currentprofile", profileNum);
    cJSON_AddStringToObject(reported, "automode", get_suppression_mode());
    cJSON_AddBoolToObject(reported, "emergencystop", emergencyStopActive);
    cJSON_AddBoolToObject(reported, "systemreset", false);
    cJSON_AddBoolToObject(reported, "startallpumps", startAllPumpsActive);
    cJSON_AddNumberToObject(reported, "_manual_start_all_pumps_duration",
                             (int)(startAllPumpsConfiguredDurationMs / 1000UL));
    
    // ALWAYS report WiFi credentials as top-level fields
	const char* current_password = get_current_wifi_password();
	
	if (current_ssid && current_password) {
	    // Add as top-level fields (not nested object)
	    cJSON_AddStringToObject(reported, "wifissid", current_ssid);
	    cJSON_AddStringToObject(reported, "password", current_password);
	    // Don't log the actual password for security
	} else {
	    printf("\n[SHADOW] Warning: Cannot get WiFi credentials (ssid: %s, password: %s)", 
	           current_ssid ? current_ssid : "NULL", 
	           current_password ? current_password : "NULL");
	}
   // ✅ PUMP OBJECTS - Report what user wants to see, not hardware state
const char* pumpNames[4] = {"northpump", "southpump", "eastpump", "westpump"};

for (int i = 0; i < 4; i++) {
    cJSON *pumpObj = cJSON_CreateObject();
    if (pumpObj) {
        // ✅ MANUAL ACTIVATE - Report the last acknowledged desired state
        bool reportedManualActivate = last_shadow_manual_activate[i];
        
        cJSON_AddBoolToObject(pumpObj, "manual_activate", reportedManualActivate);
        
        // ✅ MANUAL SINGLE OPERATION TIME - Report the configured run duration (seconds)
        int reportedOperationSeconds = (int)(pumpManualOperationConfiguredMs[i] / 1000UL);
        
        cJSON_AddNumberToObject(pumpObj, "manual_single_operation_time", reportedOperationSeconds);
        
        // ✅ STOP PUMP
        cJSON_AddBoolToObject(pumpObj, "stoppump", last_shadow_stop_pump[i]);
        
        cJSON_AddItemToObject(reported, pumpNames[i], pumpObj);
    }
}
    
    // Create compact JSON string
    char *json_str = create_compact_json_string(root);
    if (json_str) {
        char shadow_update_topic[128];
        snprintf(shadow_update_topic, sizeof(shadow_update_topic),
                 "$aws/things/%s/shadow/update", thing_name);
        
        int msg_id = esp_mqtt_client_publish(mqtt_client, shadow_update_topic, 
                                            json_str, 0, MQTT_QOS_LEVEL, 0);
        
        if (msg_id >= 0) {
            printf("\nShadow update sent");
        } else {
            printf("\n[SHADOW] Failed to send shadow update (error: %d)", msg_id);
        }
        
        free(json_str);
    }
    
    cJSON_Delete(root);
  }

// ========================================
// MQTT FUNCTIONS - OPTIMIZED
// ========================================

bool enqueue_mqtt_publish(const char *topic, const char *payload) {

    
    if (mqtt_publish_queue == NULL) {
        printf("\n[MQTT] Publish queue not initialized");
        return false;
    }
    
    // Check payload size
    size_t payload_len = strlen(payload);
    if (payload_len >= MAX_JSON_PAYLOAD_SIZE) {
        printf("\n[MQTT] Payload too large (%d bytes)", payload_len);
        return false;
    }
    
    mqtt_publish_message_t msg;
    strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
    msg.topic[sizeof(msg.topic) - 1] = '\0';
    strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    msg.payload[sizeof(msg.payload) - 1] = '\0';
    
    if (xQueueSend(mqtt_publish_queue, &msg, pdMS_TO_TICKS(10)) != pdPASS) {
        printf("\n[MQTT] Publish queue full");
        return false;
    }
    
    return true;
}

static void subscribe_to_topics(void) {
    printf("\n[MQTT] ===== SUBSCRIBING TO TOPICS =====");
    
    if (!mqtt_client || !mqtt_connected) {
        printf("\n[MQTT] Not connected");
        return;
    }
    snprintf(registration_cloud_topic, sizeof(registration_cloud_topic),
                 "Request/%s/RegistrationCloud", mac_address);
    // ✅ Only subscribe to operational topics if fully registered
  
        char shadow_update_delta[128];
        char shadow_get_accepted[128];
        char shadow_update_accepted[128];
        char shadow_update_rejected[128];
        
        snprintf(registration_response_topic, sizeof(registration_response_topic),
                 "Response/%s/RegistrationDevice", mac_address);
                 
        snprintf(shadow_update_delta, sizeof(shadow_update_delta),
                 "$aws/things/%s/shadow/update/delta", thing_name);
        snprintf(shadow_get_accepted, sizeof(shadow_get_accepted),
                 "$aws/things/%s/shadow/get/accepted", thing_name);
        snprintf(shadow_update_accepted, sizeof(shadow_update_accepted),
                 "$aws/things/%s/shadow/update/accepted", thing_name);
        snprintf(shadow_update_rejected, sizeof(shadow_update_rejected),
                 "$aws/things/%s/shadow/update/rejected", thing_name);
        
        printf("\n[MQTT] Subscribing to operational topics:");
        printf("\n %s", shadow_update_delta);
        printf("\n %s", shadow_get_accepted);
        printf("\n %s", shadow_update_accepted);
        printf("\n %s", shadow_update_rejected);
        printf("\n %s", registration_response_topic);
       
        // Subscribe to topics
        esp_mqtt_client_subscribe(mqtt_client, shadow_update_delta, 1);
        esp_mqtt_client_subscribe(mqtt_client, shadow_get_accepted, 1);
        esp_mqtt_client_subscribe(mqtt_client, shadow_update_accepted, 1);
        esp_mqtt_client_subscribe(mqtt_client, shadow_update_rejected, 1);
        esp_mqtt_client_subscribe(mqtt_client, registration_response_topic, 1);
        
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Request current shadow state (only if operational)

            printf("\n[MQTT] Requesting device shadow state...");
            char shadow_get_topic[128];
            snprintf(shadow_get_topic, sizeof(shadow_get_topic),
                     "$aws/things/%s/shadow/get", thing_name);
            esp_mqtt_client_publish(mqtt_client, shadow_get_topic, "{}", 0, 1, 0);
        
        printf("\n[MQTT] ===== SUBSCRIPTIONS COMPLETE =====");
   
}
static void send_registration(void)
{
	  if (!mqtt_connected || !mqtt_client) {
        printf("\n[REGISTRATION] ERROR: MQTT not connected! Not Sending Reg request");
        return;
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "macAddress", mac_address);
    cJSON_AddStringToObject(root, "event", "registration");
    cJSON_AddStringToObject(root, "devicetype", DEVICE_TYPE);
    cJSON_AddStringToObject(root, "wifissid", get_current_wifi_ssid());
    cJSON_AddStringToObject(root, "password", get_current_wifi_password());

    char *payload = cJSON_PrintUnformatted(root);
    
    // DEBUG LOG
    printf("\n[REGISTRATION] Sending registration request:");
    printf("\n  Topic: %s", registration_cloud_topic);
    printf("\n  Payload: %s", payload);
    printf("\n  Listening on: %s", registration_response_topic);
    
    enqueue_mqtt_publish(registration_cloud_topic, payload);
    free(payload);
    cJSON_Delete(root);
}

static void send_heartbeat(void) {        
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "macAddress", mac_address);
        cJSON_AddStringToObject(root, "event", "heartbeat");
        cJSON_AddStringToObject(root, "devicetype", "G");
        cJSON_AddStringToObject(root, "timestamp", get_custom_timestamp());
        
        char *json_str = create_compact_json_string(root);
        if (json_str) {
            char topic[128];
            snprintf(topic, sizeof(topic), "Request/%s/heartBeatUpdate", mac_address);
            enqueue_mqtt_publish(topic, json_str);
            free(json_str);
        }
        cJSON_Delete(root);
    }
}

// =======================
// SYSTEM STATUS FUNCTION 
// =======================


static void send_system_status(void) {

    // ── Data collection ───────────────────────────────────────────────────
    const char* profileName       = "Unknown";
    bool        lockout           = false;
    float       ir_values[4]      = {0};
    float       current_values[4] = {0};
    bool        pump_running[4]   = {false};
     bool camera_enabled = is_camera_active();
    PumpState   pump_states[4]    = {PUMP_OFF, PUMP_OFF, PUMP_OFF, PUMP_OFF};

    // ✅ Read profile AND its caps atomically under the same mutex
    unsigned long mcrc_full_ms   = 0;
    unsigned long mcrc_sector_ms = 0;

    if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (currentProfile >= WILDLAND_STANDARD && currentProfile <= CONTINUOUS_FEED) {
            profileName      = profiles[currentProfile].name;
            mcrc_full_ms     = profiles[currentProfile].maxRunCapFull;
            mcrc_sector_ms   = profiles[currentProfile].maxRunCapSector;
        }
        xSemaphoreGive(mutexSystemState);
    }

    if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(100)) == pdTRUE) {
        lockout = waterLockout;
        xSemaphoreGive(mutexWaterState);
    }

    if (xSemaphoreTake(mutexSensorData, pdMS_TO_TICKS(100)) == pdTRUE) {
        ir_values[0] = ir_s1;
        ir_values[1] = ir_s2;
        ir_values[2] = ir_s3;
        ir_values[3] = ir_s4;
        for (int i = 0; i < 4; i++) {
            current_values[i] = currentSensors[i].averageValue;
        }
        xSemaphoreGive(mutexSensorData);
    }

    if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < 4; i++) {
            pump_states[i]  = pumps[i].state;
            pump_running[i] = pumps[i].isRunning;
        }
        xSemaphoreGive(mutexPumpState);
    }

    // ── Active pump count ─────────────────────────────────────────────────
    int active_pump_count = 0;
    for (int i = 0; i < 4; i++) {
        if (pump_states[i] == PUMP_AUTO_ACTIVE ||
            pump_states[i] == PUMP_MANUAL_ACTIVE) {
            active_pump_count++;
        }
    }

    // ── suppressionMode ───────────────────────────────────────────────────
    const char* suppression_mode;
    if      (active_pump_count == 0) suppression_mode = "NONE";
    else if (active_pump_count == 4) suppression_mode = "FULL";
    else                             suppression_mode = "SECTOR";

    // ── Network type & signal strength ────────────────────────────────────
    const char* network_type    = "NONE";
    const char* signal_strength = "NONE";

    if (current_active_network == ACTIVE_NET_WIFI && is_wifi_connected()) {
        network_type = "WiFi";
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            int rssi = ap_info.rssi;
            if      (rssi >= -60) signal_strength = "Excellent";
            else if (rssi >= -70) signal_strength = "Good";
            else                  signal_strength = "Weak";
        } else {
            signal_strength = "Good";
        }
    }
#if GSM_ENABLED
    else if (current_active_network == ACTIVE_NET_GSM && gsm_manager_is_connected()) {
        network_type = "GSM";
        int csq = gsm_manager_get_signal_quality();
        if      (csq == 99 || csq < 0) signal_strength = "Weak";
        else if (csq >= 20)             signal_strength = "Excellent";
        else if (csq >= 10)             signal_strength = "Good";
        else                            signal_strength = "Weak";
    }
#endif

    // ── MCRC string ───────────────────────────────────────────────────────
    char mcrc_string[16];

    if (continuousWaterFeed) {
        snprintf(mcrc_string, sizeof(mcrc_string), "INDEFINITE");
    } else {
        unsigned long cap_ms = (active_pump_count == 4)
                               ? mcrc_full_ms
                               : mcrc_sector_ms;
        if (cap_ms == 0) {
            snprintf(mcrc_string, sizeof(mcrc_string), "INDEFINITE");
        } else {
            int minutes = (int)(cap_ms / 60000UL);
            snprintf(mcrc_string, sizeof(mcrc_string), "%d mins", minutes);
        }
    }

    // ── Build JSON ────────────────────────────────────────────────────────
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "macAddress", mac_address);
    cJSON_AddStringToObject(root, "event",      "periodicupdate");
    cJSON_AddStringToObject(root, "devicetype", "G");
    cJSON_AddStringToObject(root, "timestamp",  get_custom_timestamp());

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "payload", payload);

    // ── Top-level fields ──────────────────────────────────────────────────
    cJSON_AddBoolToObject  (payload, "waterlockout",         lockout);
    cJSON_AddBoolToObject  (payload, "dooropen",             doorOpen);
    cJSON_AddStringToObject(payload, "profilename",          profileName);

    cJSON_AddNumberToObject(payload, "waterlevel",
        round(level_s * 100.0) / 100.0);
    cJSON_AddNumberToObject(payload, "batterylevel",
        round(bat_v   * 100.0) / 100.0);
    cJSON_AddNumberToObject(payload, "solarvoltage",
        round(sol_v   * 100.0) / 100.0);

    // Inverter voltages (4 inverters, adc_array2[2..5] * 360)
    cJSON_AddNumberToObject(payload, "inverter1voltage",
        round(volt1 * 100.0) / 100.0);
    cJSON_AddNumberToObject(payload, "inverter2voltage",
        round(volt2 * 100.0) / 100.0);
    cJSON_AddNumberToObject(payload, "inverter3voltage",
        round(volt3 * 100.0) / 100.0);
    cJSON_AddNumberToObject(payload, "inverter4voltage",
        round(volt4 * 100.0) / 100.0);

    cJSON_AddBoolToObject  (payload, "emergencystopactive",  emergencyStopActive);
    cJSON_AddStringToObject(payload, "suppressionmode",      suppression_mode);
    cJSON_AddStringToObject(payload, "automode",             get_suppression_mode());
	 // Camera status
    cJSON_AddBoolToObject  (payload, "cameraenabled",        camera_enabled);
    // Network
    cJSON_AddStringToObject(payload, "networktype",          network_type);
    cJSON_AddStringToObject(payload, "signalstrength",       signal_strength);

    // Continuous feed
    cJSON_AddBoolToObject  (payload, "continuousfeedactive", continuousWaterFeed);

    // ── Flame detection - top level, one field per sector ─────────────────
    // "Detected" = IR value above FIRE_THRESHOLD
    // "Normal"   = IR value below FIRE_THRESHOLD
    cJSON_AddStringToObject(payload, "northflame",
        (ir_values[0] > FIRE_THRESHOLD) ? "detected" : "normal");
    cJSON_AddStringToObject(payload, "southflame",
        (ir_values[1] > FIRE_THRESHOLD) ? "detected" : "normal");
    cJSON_AddStringToObject(payload, "eastflame",
        (ir_values[2] > FIRE_THRESHOLD) ? "detected" : "normal");
    cJSON_AddStringToObject(payload, "westflame",
        (ir_values[3] > FIRE_THRESHOLD) ? "detected" : "normal");

    // ── Pump objects ──────────────────────────────────────────────────────
    const char* pumpNames[4] = {"northpump", "southpump", "eastpump", "westpump"};

    for (int i = 0; i < 4; i++) {
        cJSON *pumpObj = cJSON_CreateObject();
        if (!pumpObj) continue;

        cJSON_AddNumberToObject(pumpObj, "irvalue",
            round(ir_values[i]      * 100.0) / 100.0);
        cJSON_AddNumberToObject(pumpObj, "currentdraw",
            round(current_values[i] * 100.0) / 100.0);
        cJSON_AddStringToObject(pumpObj, "pumpstate",
            get_pump_state_string(i));
        cJSON_AddBoolToObject  (pumpObj, "pumprunning", pump_running[i]);
        cJSON_AddStringToObject(pumpObj, "mcrcminutes", mcrc_string);

        cJSON_AddItemToObject(payload, pumpNames[i], pumpObj);
    }
    cJSON_AddStringToObject(payload, "currentfirmwareversion",          "1.1.0");

    // ── Serialise & publish ───────────────────────────────────────────────
    char *json_str = create_compact_json_string(root);
    if (json_str) {
        char topic[128];
        snprintf(topic, sizeof(topic), "Request/%s/PeriodicUpdate", mac_address);
        enqueue_mqtt_publish(topic, json_str);
        free(json_str);
    }
    cJSON_Delete(root);
}
// ========================================
// PROVISIONING FUNCTIONS
// ========================================

static void check_provisioning_status(void) {
    printf("\n[PROV] === PROVISIONING STATUS CHECK ===");
    
    if (spiffs_credentials_exist()) {
        char *cert_data = NULL;
        char *key_data = NULL;
        size_t cert_size = 0, key_size = 0;
        
        esp_err_t cert_ret = spiffs_read_file(SPIFFS_CERT_PATH, &cert_data, &cert_size);
        esp_err_t key_ret = spiffs_read_file(SPIFFS_KEY_PATH, &key_data, &key_size);
        
        if (cert_ret == ESP_OK && key_ret == ESP_OK && cert_data && key_data) {
            if (strstr(cert_data, "-----BEGIN CERTIFICATE-----") != NULL &&
                strstr(key_data, "-----BEGIN") != NULL) {
                
                // Free existing certificates if any
                if (device_cert_pem) {
					free(device_cert_pem);
					device_cert_pem = NULL;
					}
                if (device_private_key) {
					free(device_private_key);
					device_private_key = NULL;
					}
                
                device_cert_pem = strdup(cert_data);
                device_private_key = strdup(key_data);
                is_provisioned = true;
                printf("\n[PROV] Device is properly provisioned");
            } else {
                printf("\n[PROV] Certificates exist but are invalid");
                is_provisioned = false;
                
                spiffs_delete_file(SPIFFS_CERT_PATH);
                spiffs_delete_file(SPIFFS_KEY_PATH);
                spiffs_delete_file(SPIFFS_THING_NAME_PATH);
            }
            
            free(cert_data);
            free(key_data);
        } else {
            printf("\n[PROV] Failed to read certificates");
            is_provisioned = false;
        }
    } else {
        printf("\n[PROV] No certificates found - device not provisioned");
        is_provisioned = false;
        strcpy(thing_name, "Unprovisioned");
    }
    
    printf("\n[PROV] ====================================");
}


static esp_err_t start_provisioning(void)
{
    printf("\n====================================");
    printf("\nSECURE FLEET PROVISIONING (Lambda-Only Flow)");
    printf("\nLambda validates, creates cert, Thing & policy");
    printf("\n====================================");

    // Reset provisioning state
    secure_provision_response_received = false;
    secure_provision_approved = false;
    memset(secure_provision_rejection_reason, 0, sizeof(secure_provision_rejection_reason));
    memset(received_certificate_pem, 0, sizeof(received_certificate_pem));
    memset(received_private_key, 0, sizeof(received_private_key));
    memset(received_certificate_id, 0, sizeof(received_certificate_id));

    // Build dynamic topics (MAC-based)
    snprintf(secure_provision_request_topic, sizeof(secure_provision_request_topic),
             SECURE_PROVISION_REQUEST_TOPIC, mac_address);
    snprintf(secure_provision_response_topic, sizeof(secure_provision_response_topic),
             SECURE_PROVISION_RESPONSE_TOPIC, mac_address);

    printf("\nProvisioning Topics:");
    printf("\nRequest:  %s", secure_provision_request_topic);
    printf("\nResponse: %s", secure_provision_response_topic);

    // ========== STEP 1: CONNECT WITH CLAIM CERTIFICATE ==========
    printf("\n====================================");
    printf("\n STEP 1: CONNECTING WITH CLAIM CERT");
    printf("\n====================================");

    if (mqtt_connect(CLAIM_THING_NAME, AWS_CLAIM_CERT, AWS_CLAIM_PRIVATE_KEY) != ESP_OK) {
        printf("\nFailed to connect with claim certificate");
        provisioning_in_progress = false;
        return ESP_FAIL;
    }

    printf("\nConnected with claim certificate");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // ========== STEP 2: SUBSCRIBE TO RESPONSE TOPIC ==========
    printf("\n====================================");
    printf("\nSTEP 2: SUBSCRIBING TO RESPONSE");
    printf("\n====================================");

    int msg_id = esp_mqtt_client_subscribe(mqtt_client, secure_provision_response_topic, 1);
    printf("\nSubscribed to %s (msg_id=%d)",
                     secure_provision_response_topic, msg_id);

    vTaskDelay(pdMS_TO_TICKS(1000));

    // ========== STEP 3: REQUEST PROVISIONING FROM LAMBDA ==========
    printf("\n====================================");
    printf("\nSTEP 3: REQUESTING PROVISIONING");
    printf("\n====================================\n");
    printf("\n[PROV] MAC: %s", mac_address);
    printf("\n[PROV] Type: %s", DEVICE_TYPE);
    printf("\n====================================\n");

    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "macAddress", mac_address);
    cJSON_AddStringToObject(request, "deviceType", DEVICE_TYPE);

    char *payload = cJSON_PrintUnformatted(request);

    printf("\nPublishing to: %s", secure_provision_request_topic);
    printf("\nPayload: %s", payload);

    msg_id = esp_mqtt_client_publish(mqtt_client, secure_provision_request_topic,
                                      payload, 0, 1, 0);

    printf("\n   Request published (msg_id=%d)", msg_id);
    printf("\n   Waiting for Lambda response...");
    printf("\n   Lambda will:");
    printf("\n   1. Validate device in DynamoDB");
    printf("\n   2. Check if already provisioned");
    printf("\n   3. Create certificate");
    printf("\n   4. Create Thing: FD_%s_%s", DEVICE_TYPE, mac_address);
    printf("\n   5. Attach policy to certificate");
    printf("\n   6. Return credentials to device");

    free(payload);
    cJSON_Delete(request);

    // ========== STEP 4: WAIT FOR LAMBDA RESPONSE ==========
    TickType_t start_time = xTaskGetTickCount();
    while (!secure_provision_response_received &&
           (xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(SECURE_PROVISION_TIMEOUT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!secure_provision_response_received) {
        printf("\n Timeout waiting for Lambda response");
        printf("\n   Possible causes:");
        printf("\n   1. Device not in DynamoDB");
        printf("\n   2. IoT Rule not triggering Lambda");
        printf("\n   3. Network issues");
        provisioning_in_progress = false;
        return ESP_FAIL;
    }

    if (!secure_provision_approved) {
        printf("\n====================================");
        printf("\nPROVISIONING REJECTED BY LAMBDA");
        printf("\n====================================");
        printf("\nReason: %s", secure_provision_rejection_reason);
        printf("\n");
        printf("\n   Common rejection reasons:");
        printf("\n   - Device not found in DynamoDB");
        printf("\n   - ActivationPermission = false");
        printf("\n   - CurrentStatus != 'pending'");
        printf("\n   - Device type mismatch");
        printf("\n   - Already provisioned (has CertificateArn)");

        provisioning_in_progress = false;
        return ESP_FAIL;
    }

    // ========== STEP 5: SAVE CERTIFICATE TO SPIFFS ==========
    printf("\n====================================");
    printf("\nLAMBDA APPROVED - SAVING CERTS");
    printf("\n====================================");
    printf("\n Saving certificate to SPIFFS...");

    if (spiffs_store_credentials(received_certificate_pem, received_private_key) != ESP_OK) {
        printf("\nFailed to save certificates to SPIFFS");
        provisioning_in_progress = false;
        return ESP_FAIL;
    }

    printf("\nCertificates saved to SPIFFS");

    // Load certificates into memory
    size_t size;
    if (device_cert_pem != NULL) {
        free(device_cert_pem);
        device_cert_pem = NULL;
    }
    if (device_private_key != NULL) {
        free(device_private_key);
        device_private_key = NULL;
    }

    spiffs_read_file(SPIFFS_CERT_PATH, &device_cert_pem, &size);
    spiffs_read_file(SPIFFS_KEY_PATH, &device_private_key, &size);

    printf("\nCertificates loaded into memory");

    // ========== SUCCESS! ==========
    printf("\n====================================");
    printf("\nSECURE PROVISIONING COMPLETE!");
    printf("\n====================================");
    printf("\n Thing Name: %s ", thing_name);
    printf("\n MAC Address: %s ", mac_address);
    printf("\n Certificate saved to SPIFFS");
    printf("\n Thing created by Lambda");
    printf("\n Policy attached by Lambda");
    printf("\n NO Register Thing needed - Lambda did everything!");
    printf("\n====================================");

    provisioning_complete = true;
    certs_created = true;
    is_provisioned = true;

    printf("\nDisconnecting claim certificate connection...");

    // Disconnect MQTT
    if (mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    printf("\nReady to connect with device certificate");
    printf("\nNext: Device will reconnect and register with cloud");
    
    return ESP_OK;
}

// ========================================
// ALERT SYSTEM FUNCTIONS - OPTIMIZED
// ========================================
static void init_alert_system(void) {
    printf("\n[ALERT] Initializing alert system...");
    
    alert_queue = xQueueCreate(10, sizeof(Alert));  // Reduced from 20
    alert_mutex = xSemaphoreCreateMutex();
    
    if (alert_queue && alert_mutex) {
        xTaskCreate(alert_task, "AlertTask", TASK_ALERT_STACK_SIZE, NULL, TASK_PRIORITY_ALERT, &taskAlertHandle);
        
        last_profile = convert_profile_enum_to_number(currentProfile);
        last_door_state = doorOpen;
        last_water_lockout = waterLockout;
        
        for (int i = 0; i < 4; i++) {
            last_pump_states[i] = pumps[i].state;
            fire_alerts_active[i] = false;
        }
        
        active_fire_count = 0;
        
        printf("\n[ALERT] Alert system initialized successfully");
    } else {
        printf("\n[ALERT] ERROR: Failed to initialize alert system");
    }
}

// ========================================
// ALERT SYSTEM FUNCTIONS
// ========================================



static fire_sector_t get_sector_from_index(int sensor_index) {
    switch(sensor_index) {
        case 0: return SECTOR_NORTH;   // ir_s1
        case 1: return SECTOR_SOUTH;   // ir_s2
        case 2: return SECTOR_EAST;    // ir_s3
        case 3: return SECTOR_WEST;    // ir_s4
        default: return SECTOR_UNKNOWN;
    }
}


// ========================================
// UPDATED check_state_changes
// ========================================

static void check_state_changes(void) {
    if (!ALERT_SYSTEM_ENABLED) return;
    
    // ========================================
    // TRACK startAllPumps WITH REASON DETECTION
    // ========================================
    static bool last_start_all_pumps = false;
    static TickType_t start_all_activation_time = 0;
    static bool alert_activation_sent = false;
    
    // ACTIVATION: User sets startAllPumps to TRUE
    if (startAllPumpsActive && !last_start_all_pumps) {
        printf("\n[ALERT] startAllPumps ACTIVATED - sending alert");
        send_alert_start_all_pumps_activated();
        start_all_activation_time = xTaskGetTickCount();
        alert_activation_sent = true;
        last_start_all_pumps = true;
        
        // ✅ Reset stop reason on activation
        startAllPumps_stop_reason = STOP_REASON_START_ALL_NONE;
    }
    
    // ✅ DEACTIVATION: Use captured stop reason
    if (!startAllPumpsActive && last_start_all_pumps && alert_activation_sent) {
        printf("\n[ALERT] startAllPumps flag cleared by natural condition");
        
        const char* stop_reason = NULL;
        int actual_runtime = 0;
        
        // Calculate actual runtime
        if (start_all_activation_time > 0) {
            TickType_t elapsed = xTaskGetTickCount() - start_all_activation_time;
            actual_runtime = (int)(elapsed * portTICK_PERIOD_MS / 1000);
        }
        
        // ✅ Use the captured stop reason from fire_system.c
        switch(startAllPumps_stop_reason) {
            case STOP_REASON_START_ALL_EMERGENCY_STOP:
                stop_reason = "EMERGENCY_STOP";
                printf("\n[ALERT] Stop reason: Emergency Stop (captured in fire_system.c)");
                break;
                
            case STOP_REASON_START_ALL_WATER_LOCKOUT:
                stop_reason = "WATER_LOCKOUT";
                printf("\n[ALERT] Stop reason: Water Lockout (captured in fire_system.c)");
                break;
                
            case STOP_REASON_START_ALL_TIMER_EXPIRED:
                stop_reason = "TIMER_EXPIRED";
                printf("\n[ALERT] Stop reason: Timer Expired (captured in task_pump_management)");
                break;
                
            default: {
                // Fallback detection
                printf("\n[ALERT] No captured reason, using fallback detection");
                if (emergencyStopActive) {
                    stop_reason = "EMERGENCY_STOP";
                } else if (waterLockout) {
                    stop_reason = "WATER_LOCKOUT";
                } else {
                    // Window is centered on whatever duration this run was
                    // actually started with (default 90s, or the shadow-supplied
                    // _manual_start_all_pumps_duration), not a hardcoded 90s.
                    int configuredSeconds = (int)(startAllPumpsConfiguredDurationMs / 1000UL);
                    if (actual_runtime >= (configuredSeconds - 5) && actual_runtime <= (configuredSeconds + 5)) {
                        stop_reason = "TIMER_EXPIRED";
                    } else {
                        stop_reason = "TIMER_EXPIRED"; // Default
                    }
                }
                break;
            }
        }
        
        // Send alert
        if (stop_reason != NULL) {
            send_alert_start_all_pumps_deactivated(stop_reason, actual_runtime);
        }
        
        // Reset tracking
        last_start_all_pumps = false;
        alert_activation_sent = false;
        start_all_activation_time = 0;
        startAllPumps_stop_reason = STOP_REASON_START_ALL_NONE;
    }
    
    // Check profile change
    int current_profile_num = convert_profile_enum_to_number(currentProfile);
    if (current_profile_num != last_profile) {
        const char* profile_name = "Unknown";
        if (currentProfile >= WILDLAND_STANDARD && currentProfile <= CONTINUOUS_FEED) {
            profile_name = profiles[currentProfile].name;
        }
        
        send_alert_profile_change(last_profile, current_profile_num, profile_name);
        last_profile = current_profile_num;
    }
    
    // Check emergency stop status
    static bool last_emergency_stop = false;
    if (emergencyStopActive != last_emergency_stop) {
        if (emergencyStopActive) {
            send_alert_emergency_stop_activated();
        } else {
            send_alert_emergency_stop_deactivated();
        }
        last_emergency_stop = emergencyStopActive;
    }
    
    // Check pump state changes
    for (int i = 0; i < 4; i++) {
        PumpState current_state = pumps[i].state;
        
        if (current_state != last_pump_states[i]) {
            send_system_status();
            last_pump_states[i] = current_state;
        }
    }
    
    // Check door status
   static TickType_t door_open_start_time = 0;
    if (doorOpen != last_door_state) {
        if (doorOpen) {
            door_open_start_time = xTaskGetTickCount();
            send_alert_door_opened();  // UPDATED - Call new function
        } else {
            int openDuration = (int)((xTaskGetTickCount() - door_open_start_time) * portTICK_PERIOD_MS / 1000);
            send_alert_door_closed(openDuration);  // UPDATED - Call new function
        }
        last_door_state = doorOpen;
    }
    
    // Check water lockout
    if (waterLockout != last_water_lockout) {
        send_alert_water_lockout(waterLockout, level_s);
        last_water_lockout = waterLockout;
    }

    // Check continuous feed
    if (continuousWaterFeed != last_continuous_feed) {
        send_alert_continuous_feed(continuousWaterFeed);
        last_continuous_feed = continuousWaterFeed;
    }
}
// ==========================================
// FIXED: monitor_fire_sectors 
// ==========================================
static void monitor_fire_sectors(void) {
    if (!ALERT_SYSTEM_ENABLED) return;
    
    float sensor_values[4] = {0, 0, 0, 0};
    bool fire_detected[4] = {false};
    int current_fire_count = 0;
    
    // Read sensor values
    if (xSemaphoreTake(mutexSensorData, pdMS_TO_TICKS(100)) == pdTRUE) {
        sensor_values[0] = ir_s1;
        sensor_values[1] = ir_s2;
        sensor_values[2] = ir_s3;
        sensor_values[3] = ir_s4;
        xSemaphoreGive(mutexSensorData);
    } else {
        printf("[FIRE] Warning: Could not get sensor mutex\n");
        return;
    }
    
    // Check for CONFIRMED fires only (after 2s confirmation in fire_system.c)
    for (int i = 0; i < 4; i++) {
        fire_detected[i] = pumps[i].flameConfirmed;   // ← use confirmed flag, not raw sensor
        if (fire_detected[i]) {
            current_fire_count++;
        }
    }
    update_fire_detection_info();
    // ✅ CRITICAL FIX: Track fire count changes
    static int last_fire_count = 0;
    bool fire_count_changed = (current_fire_count != last_fire_count);
    
    // ✅ DECISION LOGIC: Which alert to send?
    if (fire_count_changed && current_fire_count > 0) {
        // Fire count changed - send appropriate alert
        
        if (current_fire_count == 1) {
            // ✅ SINGLE SECTOR - Find which one and send individual alert
            for (int i = 0; i < 4; i++) {
			    if (fire_detected[i] && (last_fire_count > 1 || last_fire_count == 0)) {
			        fire_sector_t sector = get_sector_from_index(i);
			        const char* sectorName = get_sector_name_string(sector);
			        bool pumpActivated = (pumps[i].state == PUMP_AUTO_ACTIVE);
			
			        send_alert_fire_detected(i, sectorName, sensor_values[i], pumpActivated,
			                                 pumps[i].flameConfirmationDurationMs);
			        fire_alerts_active[i] = true;
			        break;
			    }
			}
        } else {
            // ✅ MULTIPLE SECTORS (2-4) - Send multiple fires alert
            bool pumpStates[4] = {
                pumps[0].state == PUMP_AUTO_ACTIVE,
                pumps[1].state == PUMP_AUTO_ACTIVE,
                pumps[2].state == PUMP_AUTO_ACTIVE,
                pumps[3].state == PUMP_AUTO_ACTIVE
            };
            
            send_alert_multiple_fires(current_fire_count, sensor_values, pumpStates);
            
            // Mark all active fires as alerted
            for (int i = 0; i < 4; i++) {
                if (fire_detected[i]) {
                    fire_alerts_active[i] = true;
                }
            }
        }
    }
    
    // ✅ Check for fire CLEARED events (separate from fire detected)
    for (int i = 0; i < 4; i++) {
        if (!fire_detected[i] && fire_alerts_active[i]) {
            // Fire cleared in this sector
            fire_sector_t sector = get_sector_from_index(i);
            const char* sectorName = get_sector_name_string(sector);
            
            send_alert_fire_cleared(i, sectorName, sensor_values[i]);
            fire_alerts_active[i] = false;
        }
    }
    
    last_fire_count = current_fire_count;
    active_fire_count = current_fire_count;
}

// ========================================
// UPDATED check_manual_auto_modes()
// ========================================

static void check_manual_auto_modes(void) {
    if (!ALERT_SYSTEM_ENABLED) return;
    
    static bool manual_override_active = false;
    bool current_manual_override = false;
    
    // Check if any pump is in manual mode
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
            current_manual_override = true;
            break;
        }
    }
    
    // Manual override alert (existing functionality)
    if (current_manual_override && !manual_override_active) {
        manual_override_active = true;
    }
    else if (!current_manual_override && manual_override_active) {
        manual_override_active = false;
    }
    
}

// ========================================
// COMPLETE PROCESS_ALERTS() FUNCTION
// ========================================

static void process_alerts(void) {
    Alert alert;
    
    while (xQueueReceive(alert_queue, &alert, 0) == pdTRUE) {
        // Create root JSON object
        cJSON *root = cJSON_CreateObject();
        if (!root) continue;
        
        // Add common fields (same for ALL alerts)
        cJSON_AddStringToObject(root, "macAddress", mac_address);
        cJSON_AddStringToObject(root, "event", "alert");
        cJSON_AddStringToObject(root, "devicetype", "G");
        cJSON_AddStringToObject(root, "timestamp", alert.timestamp);
        
        // Create payload object
        cJSON *payload = cJSON_CreateObject();
        if (!payload) {
            cJSON_Delete(root);
            continue;
        }
        
        // Add common payload fields
        cJSON_AddStringToObject(payload, "alerttype", get_alert_type_string(alert.type));
        cJSON_AddStringToObject(payload, "severity", get_severity_string(alert.severity));
        cJSON_AddStringToObject(payload, "message", alert.message);
        
        // Add type-specific payload fields
        switch(alert.type) {
            
            // ==========================================
            // ALERT #1: PROFILE CHANGE
            // ==========================================
            case ALERT_TYPE_PROFILE_CHANGE:
                cJSON_AddNumberToObject(payload, "previousprofile", alert.data.profile.previousProfile);
                cJSON_AddNumberToObject(payload, "currentprofile", alert.data.profile.currentProfile);
                cJSON_AddStringToObject(payload, "profilename", alert.data.profile.profileName);
                cJSON_AddBoolToObject(payload, "acknowledgement", false); 
                
                break;
            
            // ==========================================
            // ALERT #2-3: EMERGENCY STOP ACTIVATED /DEACTIVATED
            // ==========================================
            case ALERT_TYPE_EMERGENCY_STOP_ACTIVATED:
                cJSON_AddStringToObject(payload, "action", "ACTIVATED");
                cJSON_AddBoolToObject(payload, "allpumpsstopped", true);
                
                cJSON *affectedPumpsActivated = cJSON_CreateArray();
                for (int i = 0; i < alert.data.emergencyStop.affectedPumpCount; i++) {
                    cJSON_AddItemToArray(affectedPumpsActivated, 
                        cJSON_CreateString(alert.data.emergencyStop.affectedPumps[i].pumpName));
                }
                cJSON_AddItemToObject(payload, "affectedpumps", affectedPumpsActivated);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            case ALERT_TYPE_EMERGENCY_STOP_DEACTIVATED:
                cJSON_AddStringToObject(payload, "action", "DEACTIVATED");
                cJSON_AddBoolToObject(payload, "allpumpsstopped", false);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            // ==========================================
            // ALERT #3: SYSTEM RESET
            // ==========================================
            case ALERT_TYPE_SYSTEM_RESET:
                cJSON_AddStringToObject(payload, "resettype", alert.data.systemReset.resetType);
                cJSON_AddStringToObject(payload, "defaultprofile", alert.data.systemReset.defaultProfile);
                cJSON_AddBoolToObject(payload, "allpumpsreset", alert.data.systemReset.allPumpsReset);
                cJSON_AddBoolToObject(payload, "emergencystopcleared", 
                    alert.data.systemReset.emergencyStopCleared);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            // ==========================================
// ALERT: START ALL PUMPS - ACTIVATED
// ==========================================
case ALERT_TYPE_START_ALL_PUMPS:
    cJSON_AddStringToObject(payload, "action", "ACTIVATED");
    cJSON_AddNumberToObject(payload, "duration", alert.data.startAllPumps.duration);
    
    // Add activated pumps array
    cJSON *activatedPumps = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON *pump = cJSON_CreateObject();
        cJSON_AddNumberToObject(pump, "pumpid", i + 1);
        
        // Get pump name
        char pumpName[16];
        switch(i) {
            case 0: strcpy(pumpName, "North"); break;
            case 1: strcpy(pumpName, "South"); break;
            case 2: strcpy(pumpName, "East"); break;
            case 3: strcpy(pumpName, "West"); break;
        }
        cJSON_AddStringToObject(pump, "pumpname", pumpName);
        cJSON_AddItemToArray(activatedPumps, pump);
    }
    cJSON_AddItemToObject(payload, "activatedpumps", activatedPumps);
    cJSON_AddBoolToObject(payload, "waterlockout", alert.data.startAllPumps.waterLockout);
    cJSON_AddBoolToObject(payload, "acknowledgement", false);
    break;

			// ==========================================
			// ALERT: START ALL PUMPS - TIMER EXPIRED
			// ==========================================
			case ALERT_TYPE_START_ALL_PUMPS_TIMER_EXPIRED:
			    cJSON_AddStringToObject(payload, "action", "DEACTIVATED");
			    cJSON_AddStringToObject(payload, "reason", "TIMER_EXPIRED");
			    cJSON_AddNumberToObject(payload, "totalruntime", alert.data.startAllPumps.totalRuntime);
			    cJSON_AddBoolToObject(payload, "acknowledgement", false);
			    break;
			
			// ==========================================
			// ALERT: START ALL PUMPS - EMERGENCY STOP
			// ==========================================
			case ALERT_TYPE_START_ALL_PUMPS_EMERGENCY_STOP:
			    cJSON_AddStringToObject(payload, "action", "DEACTIVATED");
			    cJSON_AddStringToObject(payload, "reason", "EMERGENCY_STOP");
			    cJSON_AddNumberToObject(payload, "totalruntime", alert.data.startAllPumps.totalRuntime);
			    cJSON_AddBoolToObject(payload, "acknowledgement", false);
			    break;
			
			// ==========================================
			// ALERT: START ALL PUMPS - WATER LOCKOUT
			// ==========================================
			case ALERT_TYPE_START_ALL_PUMPS_WATER_LOCKOUT:
			    cJSON_AddStringToObject(payload, "action", "DEACTIVATED");
			    cJSON_AddStringToObject(payload, "reason", "WATER_LOCKOUT");
			    cJSON_AddNumberToObject(payload, "totalruntime", alert.data.startAllPumps.totalRuntime);
			    cJSON_AddBoolToObject(payload, "acknowledgement", false);
			    break;
			        
            // ==========================================
            // ALERT #6-9: PUMP STATE CHANGE
            // ==========================================
            case ALERT_TYPE_PUMP_STATE_CHANGE:
		    cJSON_AddNumberToObject(payload, "pumpid", alert.data.pump.pumpId);
		    cJSON_AddStringToObject(payload, "pumpname", alert.data.pump.pumpName);
		    cJSON_AddStringToObject(payload, "previousstate",
		        get_pump_state_string_for_alert(alert.data.pump.previousState));
		    cJSON_AddStringToObject(payload, "currentstate",
		        get_pump_state_string_for_alert(alert.data.pump.currentState));

		    // Always present - why the state changed (all 12 cases)
		    cJSON_AddStringToObject(payload, "statechangereason", alert.data.pump.stopReason);

		    // Only when pump goes to OFF - specific stop cause
		    if (alert.data.pump.currentState == 0) {
		        cJSON_AddStringToObject(payload, "stopreason", alert.data.pump.stopReason);
		    }

		    // estimatedruntime: only when pump is actively running
		    // Auto mode  -> fixed profile cap ("6 min", "3 min")
		    // Manual mode -> live remaining time ("1 min 45 sec")
		    if (alert.data.pump.currentState == 1 ||   // AUTO_ACTIVE
		        alert.data.pump.currentState == 2) {   // MANUAL_ACTIVE
		        char estRuntime[40] = {0};
		        int pumpIdx = alert.data.pump.pumpId - 1;
		        if (alert.data.pump.currentState == 2) {
		            get_manual_estimated_runtime_string(pumpIdx, estRuntime, sizeof(estRuntime));
		        } else {
		            bool isFullSystem = (pumps[pumpIdx].activatedInFullSystemMode);
		            get_estimated_runtime_string(isFullSystem, estRuntime, sizeof(estRuntime));
		        }
		        cJSON_AddStringToObject(payload, "estimatedruntime", estRuntime);
		    }

		    // totalruntime for OFF and COOLDOWN
		    if (alert.data.pump.currentState == 0 ||   // OFF
		        alert.data.pump.currentState == 3) {   // COOLDOWN
		        if (alert.data.pump.totalRuntime > 0) {
		            cJSON_AddNumberToObject(payload, "totalruntime",
		                alert.data.pump.totalRuntime);
		        }
		    }
		    // cooldownduration for COOLDOWN
		    if (alert.data.pump.currentState == 3) {
		        cJSON_AddNumberToObject(payload, "cooldownduration",
		            alert.data.pump.cooldownDuration);
		    }
		    cJSON_AddBoolToObject(payload, "acknowledgement", false);
		    break;
            
            // ==========================================
            // ALERT #11: FIRE DETECTED
            // ==========================================
            case ALERT_TYPE_FIRE_DETECTED:
		    {
		        // Derive fire-extent flags directly from the detected sector count
		        int totalSectors = alert.data.fire.totalActiveSectors;
		        bool isSingleSector = (totalSectors == 1);
		        bool isMultipleSectors = (totalSectors >= 2 && totalSectors <= 3);
		        bool isFullSector = (totalSectors == 4);
		        
		        // Add fire type boolean flags
		        cJSON_AddBoolToObject(payload, "singlesector", isSingleSector);
		        cJSON_AddBoolToObject(payload, "multiplesectors", isMultipleSectors);
		        cJSON_AddBoolToObject(payload, "fullsector", isFullSector);
		        
		        // Create affectedSectors array with single sector
		        cJSON *affectedSectors = cJSON_CreateArray();
		        cJSON *sector = cJSON_CreateObject();
		        cJSON_AddStringToObject(sector, "sector", alert.data.fire.sector);
		        cJSON_AddNumberToObject(sector, "temperature", alert.data.fire.temperature);
				cJSON_AddStringToObject(sector, "flamedetectiontime", alert.data.fire.flameDetectionTime);
		        cJSON_AddBoolToObject(sector, "pumpactive", alert.data.fire.pumpActivated);
		        cJSON_AddItemToArray(affectedSectors, sector);
		        cJSON_AddItemToObject(payload, "affectedsectors", affectedSectors);
		        
		        //cJSON_AddNumberToObject(payload, "estimatedRuntime", 0);
		        cJSON_AddNumberToObject(payload, "waterlevel", level_s);
		        cJSON_AddNumberToObject(payload, "confirmationduration", 
		                                alert.data.fire.confirmationDurationMs / 1000.0f);
     	  		cJSON_AddStringToObject(payload, "estimatedruntime", alert.data.fire.estimatedRuntime);
        		cJSON_AddBoolToObject(payload, "acknowledgement", false);
		    }
		    break;
		            
            // ==========================================
            // ALERT #12: FIRE CLEARED
            // ==========================================
            case ALERT_TYPE_FIRE_CLEARED:
                cJSON_AddStringToObject(payload, "sector", alert.data.fire.sector);
                cJSON_AddNumberToObject(payload, "sensorid", alert.data.fire.sensorId);
                cJSON_AddNumberToObject(payload, "currenttemperature", alert.data.fire.currentTemperature);
                if (alert.data.fire.duration > 0) {
                    cJSON_AddNumberToObject(payload, "duration", alert.data.fire.duration);
                }
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            // ==========================================
			// ALERT #13: MULTIPLE FIRES (2-4 SECTORS)
			// ==========================================
			case ALERT_TYPE_MULTIPLE_FIRES:
			    {
			        // Derive fire-extent flags directly from the active fire count
			        int fireCount = alert.data.multipleFires.activeFireCount;
			        bool isSingleSector = false;  // Never true for multiple fires
			        bool isMultipleSectors = (fireCount >= 2 && fireCount <= 3);
			        bool isFullSector = (fireCount == 4);
			        
			        // Add fire type boolean flags
			        cJSON_AddBoolToObject(payload, "singlesector", isSingleSector);
			        cJSON_AddBoolToObject(payload, "multiplesectors", isMultipleSectors);
			        cJSON_AddBoolToObject(payload, "fullsector", isFullSector);
			        
			        // Add affected sectors array
			        cJSON *affectedSectors = cJSON_CreateArray();
			        for (int i = 0; i < alert.data.multipleFires.activeFireCount && i < 4; i++) {
			            cJSON *sector = cJSON_CreateObject();
			            cJSON_AddStringToObject(sector, "sector", 
			                alert.data.multipleFires.affectedSectors[i].sector);
			            cJSON_AddNumberToObject(sector, "temperature", 
			                alert.data.multipleFires.affectedSectors[i].temperature);
			            cJSON_AddStringToObject(sector, "flamedetectiontime", alert.data.multipleFires.affectedSectors[i].flameDetectionTime);
			            cJSON_AddNumberToObject(sector, "confirmationduration",
                alert.data.multipleFires.affectedSectors[i].confirmationDurationMs / 1000.0f);
			            cJSON_AddStringToObject(sector, "estimatedruntime",
                alert.data.multipleFires.affectedSectors[i].estimatedRuntime);
			            cJSON_AddBoolToObject(sector, "pumpactive", 
			                alert.data.multipleFires.affectedSectors[i].pumpActive);
			            cJSON_AddItemToArray(affectedSectors, sector);
			        }
			        cJSON_AddItemToObject(payload, "affectedsectors", affectedSectors);
			        
			        cJSON_AddNumberToObject(payload, "waterlevel", alert.data.multipleFires.waterLevel);
			        cJSON_AddBoolToObject(payload, "acknowledgement", false);
			    }
			    break;
            
            // ==========================================
            // ALERT #14-15: WATER LOCKOUT
            // ==========================================
            case ALERT_TYPE_WATER_LOCKOUT:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.waterLockout.activated ? "ACTIVATED" : "DEACTIVATED");
                    double level = alert.data.waterLockout.currentWaterLevel;
					double rounded = round(level * 100.0) / 100.0;
                cJSON_AddNumberToObject(payload, "currentwaterlevel", 
                    rounded);
                
                if (alert.data.waterLockout.activated) {
                    cJSON_AddBoolToObject(payload, "allpumpsdisabled", 
                        alert.data.waterLockout.allPumpsDisabled);
                   
                } else {
                     cJSON_AddBoolToObject(payload, "allpumpsdisabled", 
                        alert.data.waterLockout.allPumpsDisabled);
                   
                }
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            // ==========================================
            // ALERT: DOOR OPENED
            // ==========================================
            case ALERT_TYPE_DOOR_STATUS_OPEN:
                cJSON_AddStringToObject(payload, "action", "OPENED");
                cJSON_AddBoolToObject(payload, "doorstate", true);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            // ==========================================
            // ALERT: DOOR CLOSED
            // ==========================================
            case ALERT_TYPE_DOOR_STATUS_CLOSE:
                cJSON_AddStringToObject(payload, "action", "CLOSED");
                cJSON_AddBoolToObject(payload, "doorstate", false);
                cJSON_AddNumberToObject(payload, "wasopenduration", 
                    alert.data.door.wasOpenDuration);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
         
            // ==========================================
            // ALERT #21-22: WIFI UPDATE
            // ==========================================
            case ALERT_TYPE_WIFI_UPDATE:
                cJSON_AddStringToObject(payload, "action", alert.data.wifi.action);
                
                if (strcmp(alert.data.wifi.action, "CREDENTIALS_UPDATED") == 0) {
                    cJSON_AddStringToObject(payload, "newssid", alert.data.wifi.newSSID);
                    if (strlen(alert.data.wifi.previousSSID) > 0) {
                        cJSON_AddStringToObject(payload, "previousssid", alert.data.wifi.previousSSID);
                    }
                    cJSON_AddBoolToObject(payload, "requirereboot", alert.data.wifi.requiresReboot);
                    cJSON_AddBoolToObject(payload, "stored", alert.data.wifi.stored);
                } else {
                    // Invalid credentials
                    cJSON_AddStringToObject(payload, "errortype", alert.data.wifi.errorType);
                    cJSON_AddStringToObject(payload, "errorcode", alert.data.wifi.errorCode);
                    
                    cJSON *details = cJSON_CreateObject();
                    cJSON_AddNumberToObject(details, "ssidlength", alert.data.wifi.ssidLength);
                    cJSON_AddNumberToObject(details, "passwordlength", alert.data.wifi.passwordLength);
                    cJSON_AddStringToObject(details, "reason", alert.data.wifi.reason);
                    cJSON_AddItemToObject(payload, "details", details);
                }
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            // ==========================================
            // ALERT #23: SENSOR FAULT
            // ==========================================
            case ALERT_TYPE_SENSOR_FAULT:
                cJSON_AddStringToObject(payload, "sensortype", alert.data.sensorFault.sensorType);
                cJSON_AddNumberToObject(payload, "sensorid", alert.data.sensorFault.sensorId);
                cJSON_AddStringToObject(payload, "sectoraffected", alert.data.sensorFault.sectorAffected);
                cJSON_AddStringToObject(payload, "errorcode", alert.data.sensorFault.errorCode);
                cJSON_AddNumberToObject(payload, "lastvalidreading", 
                    alert.data.sensorFault.lastValidReading);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
          /*
            // ==========================================
            // ALERT #24: SYSTEM ERROR (GENERIC)
            // ==========================================
            case ALERT_TYPE_SYSTEM_ERROR:
                cJSON_AddStringToObject(payload, "errortype", alert.data.systemError.errorType);
                cJSON_AddStringToObject(payload, "errorcode", alert.data.systemError.errorCode);
                if (strlen(alert.data.systemError.details) > 0) {
                    cJSON_AddStringToObject(payload, "details", alert.data.systemError.details);
                }
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            */
            
            // ==========================================
            // ALERT #25: CONTINUOUS FEED
            // ==========================================
            case ALERT_TYPE_CONTINUOUS_FEED:
                cJSON_AddStringToObject(payload, "action",
                    alert.data.continuousFeed.activated ? "ACTIVATED" : "DEACTIVATED");
                cJSON_AddStringToObject(payload, "profile", alert.data.continuousFeed.profile);
                cJSON_AddBoolToObject(payload, "unlimitedwatersupply",
                    alert.data.continuousFeed.unlimitedWaterSupply);
                if (alert.data.continuousFeed.activated) {
                    // Feed detected: caps lifted → report as indefinite
                    cJSON_AddStringToObject(payload, "estimatedruntime", "Indefinite");
                    cJSON_AddStringToObject(payload, "mcrcsector",       "Indefinite");
                    cJSON_AddStringToObject(payload, "mcrcsectorunit",   "Indefinite");
                    cJSON_AddStringToObject(payload, "mcrcsectorstatus", "LIFTED");
                    cJSON_AddStringToObject(payload, "mcrcsectorunit",   "Indefinite");
                    cJSON_AddStringToObject(payload, "mcrcsectorstatus", "LIFTED");
                    cJSON_AddStringToObject(payload, "mcrccapfullstatus","LIFTED");
                } else {
                    // Feed lost: caps restored → report actual profile values
                    unsigned long capFull   = alert.data.continuousFeed.runCapFullMs;
                    unsigned long capSector = alert.data.continuousFeed.runCapSectorMs;
                    char capBuf[32];

                    snprintf(capBuf, sizeof(capBuf), "%lu min", capFull / 60000UL);
                    cJSON_AddStringToObject(payload, "mcrccapfull", capBuf);

                    snprintf(capBuf, sizeof(capBuf), "%lu min", capSector / 60000UL);
                    cJSON_AddStringToObject(payload, "mcrccapsector", capBuf);

                    cJSON_AddStringToObject(payload, "mcrccapfullstatus",   "RESTORED");
                    cJSON_AddStringToObject(payload, "mcrccapsectorstatus", "RESTORED");
                }
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            // ==========================================
            // ALERT #26: HARDWARE FAULT ALERTS
            // ==========================================
            case ALERT_TYPE_PCA9555_FAIL:
                cJSON_AddStringToObject(payload, "hardwaretype", 
                    alert.data.hardwareFault.hardwareType);
                cJSON_AddNumberToObject(payload, "componentid", 
                    alert.data.hardwareFault.componentId);
                cJSON_AddStringToObject(payload, "errorcode", 
                    alert.data.hardwareFault.errorCode);
                cJSON_AddStringToObject(payload, "errormessage", 
                    alert.data.hardwareFault.errorMessage);
                cJSON_AddBoolToObject(payload, "systemcritical", 
                    alert.data.hardwareFault.systemCritical);
                cJSON_AddNumberToObject(payload, "affectepumpcount", 
                    alert.data.hardwareFault.affectedPumpCount);
                cJSON_AddStringToObject(payload, "affectedpumps", 
                    alert.data.hardwareFault.affectedPumps);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            case ALERT_TYPE_HARDWARE_CONTROL_FAIL:
            case ALERT_TYPE_ADC_INIT_FAIL:
            case ALERT_TYPE_CURRENT_SENSOR_FAULT:
            case ALERT_TYPE_IR_SENSOR_FAULT:
                // Same structure as PCA9555_FAIL
                cJSON_AddStringToObject(payload, "hardwaretype", 
                    alert.data.hardwareFault.hardwareType);
                cJSON_AddNumberToObject(payload, "componentid", 
                    alert.data.hardwareFault.componentId);
                cJSON_AddStringToObject(payload, "errorcode", 
                    alert.data.hardwareFault.errorCode);
                cJSON_AddStringToObject(payload, "errormessage", 
                    alert.data.hardwareFault.errorMessage);
                cJSON_AddBoolToObject(payload, "systemCritical", 
                    alert.data.hardwareFault.systemCritical);
                if (alert.data.hardwareFault.affectedPumpCount > 0) {
                    cJSON_AddNumberToObject(payload, "affectedpumpcount", 
                        alert.data.hardwareFault.affectedPumpCount);
                    cJSON_AddStringToObject(payload, "affectedpumps", 
                        alert.data.hardwareFault.affectedPumps);
                }
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            
            // ==========================================
            // ALERT #27: POWER ALERTS
            // ==========================================
            case ALERT_TYPE_BATTERY_CRITICAL:
            case ALERT_TYPE_BATTERY_LOW:
            case ALERT_TYPE_SOLAR_FAULT:
                cJSON_AddNumberToObject(payload, "batteryvoltage", 
                    alert.data.powerStatus.batteryVoltage);
                cJSON_AddNumberToObject(payload, "solarvoltage", 
                    alert.data.powerStatus.solarVoltage);
                cJSON_AddNumberToObject(payload, "threshold", 
                    alert.data.powerStatus.threshold);
                cJSON_AddStringToObject(payload, "powerstate", 
                    alert.data.powerStatus.powerState);
                if (alert.data.powerStatus.estimatedRuntime > 0) {
                    cJSON_AddNumberToObject(payload, "estimatedruntime", 
                        alert.data.powerStatus.estimatedRuntime);
                }
                cJSON_AddBoolToObject(payload, "chargingactive", 
                    alert.data.powerStatus.chargingActive);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            /*
            // ==========================================
            // ALERT #28: SYSTEM INTEGRITY ALERTS
            // ==========================================
            case ALERT_TYPE_STATE_CORRUPTION:
            case ALERT_TYPE_TASK_FAILURE:
                cJSON_AddStringToObject(payload, "integrityType", 
                    alert.data.integrity.integrityType);
                cJSON_AddStringToObject(payload, "componentName", 
                    alert.data.integrity.componentName);
                cJSON_AddNumberToObject(payload, "errorValue", 
                    alert.data.integrity.errorValue);
                if (alert.data.integrity.expectedValue != 0) {
                    cJSON_AddNumberToObject(payload, "expectedValue", 
                        alert.data.integrity.expectedValue);
                }
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.integrity.action);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
            */
            default:
                printf("\n[ALERT] Unknown alert type: %d", alert.type);
                cJSON_AddBoolToObject(payload, "acknowledgement", false);
                break;
        }
        
        // Add payload to root
        cJSON_AddItemToObject(root, "payload", payload);
        
        // Convert to JSON string
        char *json_str = create_compact_json_string(root);
        if (json_str) {
            // Publish to AWS IoT
            char topic[128];
            snprintf(topic, sizeof(topic), "Request/%s/Alerts", mac_address);
            
            printf("\n[ALERT] Publishing alert  (%s) to: %s", 
       		get_alert_type_string(alert.type), topic);
            
            if (mqtt_connected && mqtt_client) {
                int msg_id = esp_mqtt_client_publish(mqtt_client, topic, json_str, 0, 1, 0);
                if (msg_id >= 0) {
                    printf("\n[ALERT] Published successfully (msg_id: %d)", msg_id);
                } else {
		            printf("\n[ALERT] Failed to publish to AWS IoT, storing persistently");
		            store_alert_to_spiffs(topic, json_str);
		            enqueue_mqtt_publish(topic, json_str);
		        }
            } else {
			        printf("\n[ALERT] MQTT not connected, storing alert persistently");
			        store_alert_to_spiffs(topic, json_str);
			        enqueue_mqtt_publish(topic, json_str);
			    }
            
            free(json_str);
        }
        
        cJSON_Delete(root);
        
        // ADDED: Send periodic update whenever an alert is sent
        send_system_status();
    }
}


// ========================================
// ALERT HELPER FUNCTIONS
// ========================================

static bool queue_alert(Alert *alert) {
    // Simple check: wait 15 seconds after boot before sending alerts
    TickType_t current_time = xTaskGetTickCount();
    TickType_t seconds_since_boot = (current_time - boot_time) * portTICK_PERIOD_MS / 1000;
    
    if (seconds_since_boot < SENSOR_WARMUP_SECONDS) {
        // Still in warmup period - only allow critical alerts
       printf("\n[ALERT] Blocked alert - Sensors warming up (%u/%d sec)",
       (unsigned int)seconds_since_boot, SENSOR_WARMUP_SECONDS);
       return false;
  
    } else if (!sensors_ready) {
        // Warmup period done, mark sensors as ready
        sensors_ready = true;
        printf("\n[ALERT] Sensor warmup complete - Alerts enabled");
    }
    
    if (!alert_queue) {
        printf("\n[ALERT] Alert queue not initialized");
        return false;
    }
    
    if (xQueueSend(alert_queue, alert, pdMS_TO_TICKS(100)) != pdPASS) {
        printf("\n[ALERT] Alert queue full");
        return false;
    }
    
    return true;
}

static void check_battery_status(void) {
    static bool battery_low_alert_sent = false;
    static bool battery_critical_alert_sent = false;
    
    // Check battery voltage
    if (bat_v < 10.5 && !battery_critical_alert_sent) {
        int estimated_runtime = (int)((bat_v - 10.0) * 30); // Rough estimate
        send_alert_battery_critical(bat_v, estimated_runtime);
        battery_critical_alert_sent = true;
    } else if (bat_v > 11.0) {
        battery_critical_alert_sent = false;
    }
    
    if (bat_v < 11.5 && bat_v >= 10.5 && !battery_low_alert_sent) {
        send_alert_battery_low(bat_v, 11.5);
        battery_low_alert_sent = true;
    } else if (bat_v > 12.0) {
        battery_low_alert_sent = false;
    }
}

void send_alert_battery_low(float batteryVoltage, float threshold) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_BATTERY_LOW;
    alert.severity = ALERT_SEVERITY_WARNING;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Battery voltage LOW (%.2fV) - Below %.2fV threshold",
            batteryVoltage, threshold);
    
    alert.data.powerStatus.batteryVoltage = batteryVoltage;
    alert.data.powerStatus.solarVoltage = sol_v;
    alert.data.powerStatus.threshold = threshold;
    strcpy(alert.data.powerStatus.powerState, "LOW");
    alert.data.powerStatus.chargingActive = (sol_v > 5.0);
    
    queue_alert(&alert);
}

// Build estimatedruntime string directly from profile cap values.
// - Continuous feed (any profile) → "Indefinite"
// - 4 sectors on fire            → maxRunCapFull  converted to whole minutes
// - 1-3 sectors on fire          → maxRunCapSector converted to whole minutes
// No countdown, no seconds — just the fixed profile value.
static void get_estimated_runtime_string(bool isFullSystem, char* outStr, size_t outSize) {
    // Continuous feed: caps are lifted
    if (continuousWaterFeed ||
        (profiles[currentProfile].maxRunCapFull == 0 &&
         profiles[currentProfile].maxRunCapSector == 0)) {
        snprintf(outStr, outSize, "Indefinite");
        return;
    }

    unsigned long capMs = isFullSystem
                          ? profiles[currentProfile].maxRunCapFull
                          : profiles[currentProfile].maxRunCapSector;

    int minutes = (int)(capMs / 60000UL);
    snprintf(outStr, outSize, "%d min", minutes);
}

// Build estimatedruntime string for MANUAL mode pumps.
// Shows remaining time as a live countdown including seconds,
// since extend time can change the duration mid-run.
// - "2 min 30 sec" if both minutes and seconds remain
// - "45 sec"       if under 1 minute remains
// - "0 sec"        if expired
static void get_manual_estimated_runtime_string(int pumpIndex, char* outStr, size_t outSize) {
    unsigned long remainingSec = get_timer_remaining(pumpIndex);

    if (remainingSec == 0) {
        snprintf(outStr, outSize, "0 sec");
        return;
    }

    int minutes = (int)(remainingSec / 60);
    int seconds = (int)(remainingSec % 60);

    if (minutes > 0 && seconds > 0) {
        snprintf(outStr, outSize, "%d min %d sec", minutes, seconds);
    } else if (minutes > 0) {
        snprintf(outStr, outSize, "%d min", minutes);
    } else {
        snprintf(outStr, outSize, "%d sec", seconds);
    }
}

// ==========================================
// ALERT #1: PROFILE CHANGE
// ==========================================
static void send_alert_profile_change(int previousProfile, int currentProfile, const char* profileName) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_PROFILE_CHANGE;
    alert.severity = ALERT_SEVERITY_INFO;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Profile changed from %d to %d (%s)", previousProfile, currentProfile, profileName);
    
    alert.data.profile.previousProfile = previousProfile;
    alert.data.profile.currentProfile = currentProfile;
    strncpy(alert.data.profile.profileName, profileName, sizeof(alert.data.profile.profileName) - 1);
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #2: EMERGENCY STOP ACTIVATED
// ==========================================
static void send_alert_emergency_stop_activated(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_EMERGENCY_STOP_ACTIVATED;
    alert.severity = ALERT_SEVERITY_CRITICAL;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    strcpy(alert.message, "EMERGENCY STOP ACTIVATED - All pumps stopped immediately");
    
    alert.data.emergencyStop.activated = true;
    alert.data.emergencyStop.affectedPumpCount = 0;
    
    // Capture pump states before emergency stop - use saved states from fire_system.c
	extern PumpState savedPumpStates[];
	
	for (int i = 0; i < 4; i++) {
	    // ✅ Use the saved states that were captured BEFORE emergency stop
	    if (savedPumpStates[i] == PUMP_MANUAL_ACTIVE || 
	        savedPumpStates[i] == PUMP_AUTO_ACTIVE) {
	        
	        int idx = alert.data.emergencyStop.affectedPumpCount++;
	        alert.data.emergencyStop.affectedPumps[idx].pumpId = i + 1;
	        strncpy(alert.data.emergencyStop.affectedPumps[idx].pumpName, 
	               pumps[i].name, 15);
	        alert.data.emergencyStop.affectedPumps[idx].previousState = savedPumpStates[i];
	    }
	}
    queue_alert(&alert);
}

// ==========================================
// ALERT #3: EMERGENCY STOP DEACTIVATED
// ==========================================
static void send_alert_emergency_stop_deactivated(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_EMERGENCY_STOP_DEACTIVATED;
    alert.severity = ALERT_SEVERITY_INFO;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    strcpy(alert.message, "Emergency stop DEACTIVATED - System restored to normal operation");
    
    alert.data.emergencyStop.activated = false;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #4: SYSTEM RESET
// ==========================================
static void send_alert_system_reset(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_SYSTEM_RESET;
    alert.severity = ALERT_SEVERITY_WARNING;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    strcpy(alert.message, "SYSTEM RESET COMPLETE - All defaults restored");
    
    strcpy(alert.data.systemReset.resetType, "FULL");
    strcpy(alert.data.systemReset.defaultProfile, "WILDLAND STANDARD");
    alert.data.systemReset.allPumpsReset = true;
    alert.data.systemReset.emergencyStopCleared = true;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #5: START ALL PUMPS ACTIVATED
// ==========================================
static void send_alert_start_all_pumps_activated(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_START_ALL_PUMPS;
    alert.severity = ALERT_SEVERITY_WARNING;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);

    int durationSeconds = (int)(startAllPumpsConfiguredDurationMs / 1000UL);

    snprintf(alert.message, sizeof(alert.message),
            "START ALL PUMPS ACTIVATED - All 4 pumps activated for %d seconds", durationSeconds);
    
    alert.data.startAllPumps.activated = true;
    alert.data.startAllPumps.duration = durationSeconds;
    alert.data.startAllPumps.activatedPumpCount = 4;
    alert.data.startAllPumps.waterLockout = waterLockout;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #6: START ALL PUMPS DEACTIVATED
// ==========================================
static void send_alert_start_all_pumps_deactivated(const char* reason, int totalRuntime) {
    Alert alert = {0};
    
    // ✅ NEW: Select the correct alert type based on reason
    if (strcmp(reason, "TIMER_EXPIRED") == 0) {
        alert.type = ALERT_TYPE_START_ALL_PUMPS_TIMER_EXPIRED;
    } else if (strcmp(reason, "EMERGENCY_STOP") == 0) {
        alert.type = ALERT_TYPE_START_ALL_PUMPS_EMERGENCY_STOP;
    } else if (strcmp(reason, "WATER_LOCKOUT") == 0) {
        alert.type = ALERT_TYPE_START_ALL_PUMPS_WATER_LOCKOUT;
    } else {
        // Default fallback
        alert.type = ALERT_TYPE_START_ALL_PUMPS_TIMER_EXPIRED;
    }
    
    alert.severity = ALERT_SEVERITY_INFO;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Start All Pumps DEACTIVATED - %s", reason);
    
    alert.data.startAllPumps.activated = false;
    strncpy(alert.data.startAllPumps.reason, reason, sizeof(alert.data.startAllPumps.reason) - 1);
    alert.data.startAllPumps.totalRuntime = totalRuntime;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #7-10: PUMP STATE CHANGE (Updated with mode flags)
// ==========================================
static void send_alert_pump_state_change(int pumpIndex, int previousState, int currentState,
                                        const char* activationSource, const char* trigger,
                                        float sensorTemp, const char* stopReason,
                                        int runtime, int cooldownDuration) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_PUMP_STATE_CHANGE;
    
    // Get timestamp
    char timestamp[30];
    if (time_manager_get_timestamp(timestamp, sizeof(timestamp)) == ESP_OK) {
        strncpy(alert.timestamp, timestamp, sizeof(alert.timestamp) - 1);
    } else {
        strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    }
    
    // Determine severity based on state
    if (currentState == PUMP_AUTO_ACTIVE) {
        alert.severity = ALERT_SEVERITY_CRITICAL;
    } else if (currentState == PUMP_MANUAL_ACTIVE) {
        alert.severity = ALERT_SEVERITY_WARNING;
    } else if (currentState == PUMP_COOLDOWN) {
        alert.severity = ALERT_SEVERITY_WARNING;
    } else {
        alert.severity = ALERT_SEVERITY_INFO;
    }
    
    // Create message
    const char* stateStr = get_pump_state_string_for_alert(currentState);
    snprintf(alert.message, sizeof(alert.message),
            "Pump %d (%s) state changed to %s", 
            pumpIndex + 1, pumps[pumpIndex].name, stateStr);
    
    // Fill data
    alert.data.pump.pumpId = pumpIndex + 1;
    strncpy(alert.data.pump.pumpName, pumps[pumpIndex].name, 
           sizeof(alert.data.pump.pumpName) - 1);
    alert.data.pump.previousState = previousState;
    alert.data.pump.currentState = currentState;
    
    if (activationSource) {
        strncpy(alert.data.pump.activationSource, activationSource, 
               sizeof(alert.data.pump.activationSource) - 1);
    }
    if (trigger) {
        strncpy(alert.data.pump.trigger, trigger, 
               sizeof(alert.data.pump.trigger) - 1);
    }
    if (stopReason) {
        strncpy(alert.data.pump.stopReason, stopReason, 
               sizeof(alert.data.pump.stopReason) - 1);
    }
    
    alert.data.pump.totalRuntime = runtime;
    alert.data.pump.cooldownDuration = cooldownDuration;
    
    // ✅ NEW: Add mode flags
    alert.data.pump.autoEnabled = (currentState == PUMP_AUTO_ACTIVE);
    alert.data.pump.manualEnabled = (currentState == PUMP_MANUAL_ACTIVE);
    
    // Queue the alert
    if (!queue_alert(&alert)) {
        printf("\n[ALERT] Failed to queue pump state change alert for pump %d", 
               pumpIndex + 1);
    } else {
        printf("\n[ALERT] Pump state change alert queued for pump %d (Auto: %s, Manual: %s)",
               pumpIndex + 1,
               alert.data.pump.autoEnabled ? "true" : "false",
               alert.data.pump.manualEnabled ? "true" : "false");
    }
}

// ==========================================
// ALERT #12: FIRE DETECTED
// ==========================================
static void send_alert_fire_detected(int sensorIndex, const char* sectorName, 
                                    float temperature, bool pumpActivated,int confirmationDurationMs) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_FIRE_DETECTED;
    alert.severity = ALERT_SEVERITY_EMERGENCY;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    // Get current detected-sector info
    update_fire_detection_info();
    int activeSectorCount = get_active_fire_sector_count();
    const char* activeSectorNames = get_active_sectors_string();
    
    // ✅ Create unified message
    snprintf(alert.message, sizeof(alert.message),
            "FIRE DETECTED! %d active fire sector(s): %s",
            activeSectorCount, activeSectorNames);
    
    alert.data.fire.sensorId = sensorIndex + 1; // actual sensor 1-4
    
    // Store the single affected sector info
    strncpy(alert.data.fire.sector, sectorName, sizeof(alert.data.fire.sector) - 1);
    alert.data.fire.temperature = temperature;
    alert.data.fire.pumpActivated = pumpActivated;
    alert.data.fire.confirmationDurationMs = confirmationDurationMs;
    strncpy(alert.data.fire.flameDetectionTime, pumps[sensorIndex].flameDetectionTime,
            sizeof(alert.data.fire.flameDetectionTime) - 1);
    // In FULL mode a single sector confirmation activates all 4 pumps, so the
    // estimated runtime must reflect the Full cap rather than always assuming Sector.
    bool isFullSystemActivation = (strcmp(get_suppression_mode(), "full") == 0);
    get_estimated_runtime_string(isFullSystemActivation, alert.data.fire.estimatedRuntime,
                                 sizeof(alert.data.fire.estimatedRuntime));
    alert.data.fire.totalActiveSectors = activeSectorCount;
    strncpy(alert.data.fire.allActiveSectors, activeSectorNames,
            sizeof(alert.data.fire.allActiveSectors) - 1);
    
    // Store pump info
    if (pumpActivated) {
        alert.data.fire.pumpId = sensorIndex + 1;
        strncpy(alert.data.fire.pumpName, pumps[sensorIndex].name, 
               sizeof(alert.data.fire.pumpName) - 1);
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #13: FIRE CLEARED
// ==========================================
static void send_alert_fire_cleared(int sensorIndex, const char* sectorName, 
                                   float currentTemp) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_FIRE_CLEARED;
    alert.severity = ALERT_SEVERITY_INFO;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Fire CLEARED in %s sector", sectorName);
    
    strncpy(alert.data.fire.sector, sectorName, sizeof(alert.data.fire.sector) - 1);
    alert.data.fire.sensorId = sensorIndex + 1;
    alert.data.fire.currentTemperature = currentTemp;
    queue_alert(&alert);
}

// ==========================================
// ALERT #14: MULTIPLE FIRES 
// ==========================================
static void send_alert_multiple_fires(int fireCount, float sensorValues[4], 
                                     bool pumpStates[4]) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_MULTIPLE_FIRES;
    alert.severity = (fireCount >= 3) ? ALERT_SEVERITY_EMERGENCY : ALERT_SEVERITY_CRITICAL;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "FIRE DETECTED! %d active fire sectors", fireCount);
    
    alert.data.multipleFires.activeFireCount = fireCount;
    
    const char* sectorNames[4] = {"NORTH", "SOUTH", "EAST", "WEST"};
    int sectorIdx = 0;
    for (int i = 0; i < 4 && sectorIdx < fireCount; i++) {
        if (sensorValues[i] > FIRE_THRESHOLD) {
            strncpy(alert.data.multipleFires.affectedSectors[sectorIdx].sector,
                   sectorNames[i], 15);
            alert.data.multipleFires.affectedSectors[sectorIdx].temperature = sensorValues[i];
            alert.data.multipleFires.affectedSectors[sectorIdx].pumpActive = pumpStates[i];
            // ← add this: copy stored detection time from pump struct
            strncpy(alert.data.multipleFires.affectedSectors[sectorIdx].flameDetectionTime,
                    pumps[i].flameDetectionTime,
                    sizeof(alert.data.multipleFires.affectedSectors[sectorIdx].flameDetectionTime) - 1);
            alert.data.multipleFires.affectedSectors[sectorIdx].confirmationDurationMs =
                    pumps[i].flameConfirmationDurationMs;
            // Use full cap if all 4 sectors burning, sector cap otherwise
            get_estimated_runtime_string((fireCount == 4),
                alert.data.multipleFires.affectedSectors[sectorIdx].estimatedRuntime,
                sizeof(alert.data.multipleFires.affectedSectors[sectorIdx].estimatedRuntime));

            sectorIdx++;
        }
    }
    
    alert.data.multipleFires.waterLevel = level_s;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #15-16: WATER LOCKOUT
// ==========================================
static void send_alert_water_lockout(bool activated, float currentLevel) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_WATER_LOCKOUT;
    alert.severity = activated ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_INFO;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    if (activated) {
        strcpy(alert.message, "Water lockout ACTIVATED - Level below minimum threshold");
    } else {
        strcpy(alert.message, "Water lockout DEACTIVATED - Water level restored");
    }
    
    alert.data.waterLockout.activated = activated;
    alert.data.waterLockout.currentWaterLevel = currentLevel;
    
    if (activated) {
		alert.data.waterLockout.allPumpsDisabled = true;
       
    } else {
        alert.data.waterLockout.allPumpsDisabled = false;
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT: CONTINUOUS FEED DETECTED / LOST
// ==========================================
static void send_alert_continuous_feed(bool activated) {
    Alert alert = {0};
    alert.type     = ALERT_TYPE_CONTINUOUS_FEED;
    alert.severity = activated ? ALERT_SEVERITY_INFO : ALERT_SEVERITY_WARNING;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);

    if (activated) {
        strcpy(alert.message, "Continuous water feed DETECTED - MCRC lifted to Indefinite");
    } else {
        snprintf(alert.message, sizeof(alert.message),
            "Continuous feed LOST - MCRC restored (Full:%lums Sector:%lums)",
            profiles[currentProfile].maxRunCapFull,
            profiles[currentProfile].maxRunCapSector);
    }

    alert.data.continuousFeed.activated          = activated;
    strncpy(alert.data.continuousFeed.profile,
            profiles[currentProfile].name,
            sizeof(alert.data.continuousFeed.profile) - 1);
    alert.data.continuousFeed.waterLockoutDisabled = activated;
    alert.data.continuousFeed.unlimitedWaterSupply = activated;
    alert.data.continuousFeed.runCapFullMs         = profiles[currentProfile].maxRunCapFull;
    alert.data.continuousFeed.runCapSectorMs       = profiles[currentProfile].maxRunCapSector;

    queue_alert(&alert);
}

// ==========================================
// ==========================================
static void send_alert_door_opened(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_DOOR_STATUS_OPEN;
    alert.severity = ALERT_SEVERITY_WARNING;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    strcpy(alert.message, "Door OPENED");
    strcpy(alert.data.door.action, "OPENED");
    alert.data.door.opened = true;
    alert.data.door.doorState = true;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT: DOOR CLOSED
// ==========================================
static void send_alert_door_closed(int openDuration) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_DOOR_STATUS_CLOSE;
    alert.severity = ALERT_SEVERITY_INFO;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Door CLOSED - Was open for %d seconds", openDuration);
    strcpy(alert.data.door.action, "CLOSED");
    alert.data.door.opened = false;
    alert.data.door.doorState = false;
    alert.data.door.wasOpenDuration = openDuration;
    
    queue_alert(&alert);
}


// ==========================================
// ALERT #24: WIFI CREDENTIALS UPDATED
// ==========================================
static void send_alert_wifi_updated(const char* newSSID, const char* previousSSID) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_WIFI_UPDATE;
    alert.severity = ALERT_SEVERITY_INFO;
    
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "WiFi credentials updated to SSID: %s (Apply after reset)", newSSID);
    
    strcpy(alert.data.wifi.action, "CREDENTIALS_UPDATED");
    strncpy(alert.data.wifi.newSSID, newSSID, sizeof(alert.data.wifi.newSSID) - 1);
    if (previousSSID) {
        strncpy(alert.data.wifi.previousSSID, previousSSID, 
               sizeof(alert.data.wifi.previousSSID) - 1);
    }
    alert.data.wifi.requiresReboot = true;
    alert.data.wifi.stored = true;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #25: WIFI CREDENTIALS INVALID
// ==========================================
static void send_alert_wifi_invalid(int ssidLen, int passLen, const char* reason) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_WIFI_UPDATE;
    alert.severity = ALERT_SEVERITY_WARNING;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Invalid WiFi credentials: SSID length=%d, Password length=%d", ssidLen, passLen);
    
    strcpy(alert.data.wifi.action, "INVALID_CREDENTIALS");
    strcpy(alert.data.wifi.errorType, "INVALID_WIFI_CREDENTIALS");
    strcpy(alert.data.wifi.errorCode, "WIFI_001");
    alert.data.wifi.ssidLength = ssidLen;
    alert.data.wifi.passwordLength = passLen;
    strncpy(alert.data.wifi.reason, reason, sizeof(alert.data.wifi.reason) - 1);
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #26: CRITICAL HARDWARE FAULT ALERTS
// ==========================================


void send_alert_pca9555_fail(const char* errorCode, const char* errorMsg) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_PCA9555_FAIL;
    alert.severity = ALERT_SEVERITY_EMERGENCY;  // System cannot operate!
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "CRITICAL: PCA9555 I/O Expander FAILED - All pump control disabled!");
    
    strcpy(alert.data.hardwareFault.hardwareType, "PCA9555");
    alert.data.hardwareFault.componentId = 1;
    strncpy(alert.data.hardwareFault.errorCode, errorCode, 15);
    strncpy(alert.data.hardwareFault.errorMessage, errorMsg, 63);
    alert.data.hardwareFault.systemCritical = true;
    alert.data.hardwareFault.affectedPumpCount = 4;
    strcpy(alert.data.hardwareFault.affectedPumps, "North,South,East,West");
    
    queue_alert(&alert);
}


void send_alert_hardware_control_fail(int pumpIndex, const char* errorCode) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_HARDWARE_CONTROL_FAIL;
    alert.severity = ALERT_SEVERITY_CRITICAL;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "CRITICAL: Pump %d (%s) hardware verification FAILED - State mismatch!",
            pumpIndex + 1, pumps[pumpIndex].name);
    
    strcpy(alert.data.hardwareFault.hardwareType, "PUMP_CONTROL");
    alert.data.hardwareFault.componentId = pumpIndex + 1;
    strncpy(alert.data.hardwareFault.errorCode, errorCode, 15);
    snprintf(alert.data.hardwareFault.errorMessage, 63,
            "Pump %s commanded state does not match actual hardware state",
            pumps[pumpIndex].name);
    alert.data.hardwareFault.systemCritical = true;
    alert.data.hardwareFault.affectedPumpCount = 1;
    strncpy(alert.data.hardwareFault.affectedPumps, pumps[pumpIndex].name, 63);
    
    queue_alert(&alert);
}


void send_alert_current_sensor_fault(int sensorIndex, float currentValue) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_CURRENT_SENSOR_FAULT;
    alert.severity = ALERT_SEVERITY_WARNING;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Current sensor CT%d fault - Cannot verify pump %s operation",
            sensorIndex + 1, currentSensors[sensorIndex].name);
    
    strcpy(alert.data.hardwareFault.hardwareType, "CURRENT_SENSOR");
    alert.data.hardwareFault.componentId = sensorIndex + 1;
    strcpy(alert.data.hardwareFault.errorCode, "CT_FAULT");
    snprintf(alert.data.hardwareFault.errorMessage, 63,
            "Sensor reading out of range: %.3fA", currentValue);
    alert.data.hardwareFault.systemCritical = false;
    alert.data.hardwareFault.affectedPumpCount = 1;
    strncpy(alert.data.hardwareFault.affectedPumps, pumps[sensorIndex].name, 63);
    
    queue_alert(&alert);
}

void send_alert_ir_sensor_fault(int sensorIndex, float irValue) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_IR_SENSOR_FAULT;
    alert.severity = ALERT_SEVERITY_CRITICAL;  // Critical because sensor cannot detect fire
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    // Determine fault type based on IR value
    const char* faultType = "UNKNOWN";
    if (irValue <= 2.5f) {
        faultType = "CRITICAL_FAULT_0mA";  // Cannot detect fire
    } else if (irValue >= 8.0f && irValue <= 12.0f) {
        faultType = "BIT_FAULT_2mA";  // Can still detect fire
        alert.severity = ALERT_SEVERITY_WARNING;  // Downgrade for BIT fault
    }
    
    snprintf(alert.message, sizeof(alert.message),
            "IR Sensor %d (%s) %s - IR Value: %.2f%%",
            sensorIndex + 1, pumps[sensorIndex].name, faultType, irValue);
    
    strcpy(alert.data.hardwareFault.hardwareType, "IR_SENSOR");
    alert.data.hardwareFault.componentId = sensorIndex + 1;
    strcpy(alert.data.hardwareFault.errorCode, faultType);
    snprintf(alert.data.hardwareFault.errorMessage, 63,
            "IR sensor reading: %.2f%% - %s", irValue,
            (irValue <= 2.5f) ? "CANNOT detect fire" : "CAN still detect fire");
    alert.data.hardwareFault.systemCritical = (irValue <= 2.5f);  // Only critical for 0mA fault
    alert.data.hardwareFault.affectedPumpCount = 1;
    strncpy(alert.data.hardwareFault.affectedPumps, pumps[sensorIndex].name, 63);
    
    queue_alert(&alert);
}

void send_alert_battery_critical(float batteryVoltage, char estimatedRuntime) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_BATTERY_CRITICAL;
    alert.severity = ALERT_SEVERITY_EMERGENCY;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "CRITICAL: Battery voltage critically low (%.2fV) - System may shutdown!",
            batteryVoltage);
    
    alert.data.powerStatus.batteryVoltage = batteryVoltage;
    alert.data.powerStatus.solarVoltage = sol_v;
    alert.data.powerStatus.threshold = 10.5;  // Critical threshold
    strcpy(alert.data.powerStatus.powerState, "CRITICAL");
    alert.data.powerStatus.estimatedRuntime = estimatedRuntime;
    alert.data.powerStatus.chargingActive = (sol_v > 5.0);
    
    queue_alert(&alert);
}


void send_alert_state_corruption(int pumpIndex, int corruptValue) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_STATE_CORRUPTION;
    alert.severity = ALERT_SEVERITY_CRITICAL;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "CRITICAL: Pump %d (%s) state corruption detected!",
            pumpIndex + 1, pumps[pumpIndex].name);
    
    strcpy(alert.data.integrity.integrityType, "STATE");
    strncpy(alert.data.integrity.componentName, pumps[pumpIndex].name, 31);
    alert.data.integrity.errorValue = corruptValue;
    alert.data.integrity.expectedValue = 0;  // Valid range: 0-4
    strcpy(alert.data.integrity.action, "RESETTING_PUMP");
    
    queue_alert(&alert);
}


static void alert_task(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    printf("\n[ALERT] Alert task started (sensors will be ready in %d seconds)", SENSOR_WARMUP_SECONDS);
    
    while (1) {
        // Process state changes
        check_state_changes();
        
        // Check fire sectors
        monitor_fire_sectors();
        
        // Check manual/auto modes
        check_manual_auto_modes();
        
        // Process queued alerts
        process_alerts();
        
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(2000));
    }
}

// ========================================
// SYSTEM TASKS - OPTIMIZED
// ========================================

void task_serial_monitor(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    for (;;) {
        display_system_status();
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(8000));
    }
}

void task_sensor_reading(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    static int battery_check_counter = 0;
    for (;;) {
		get_sensor_data();
        if (xSemaphoreTake(mutexSensorData, pdMS_TO_TICKS(10)) == pdTRUE) {
            
            xSemaphoreGive(mutexSensorData);
        }
        // 🆕 CHECK BATTERY STATUS
        
        if (++battery_check_counter >= 10) {  // Check every 10 seconds
            check_battery_status();
            battery_check_counter = 0;
        }
        
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(1000));
    }
}

void task_fire_detection(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    for (;;) {
        bool lockout = false;
        if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(100)) == pdTRUE) {
            lockout = waterLockout;
            xSemaphoreGive(mutexWaterState);
        }
        
        if (!lockout) {
            if (xSemaphoreTake(mutexSensorData, portMAX_DELAY) == pdTRUE) {
                if (xSemaphoreTake(mutexPumpState, portMAX_DELAY) == pdTRUE) {
                    check_automatic_activation();
                    xSemaphoreGive(mutexPumpState);
                }
                xSemaphoreGive(mutexSensorData);
            }
        }
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(100));
    }
}

//==========================================
// UPDATED: task_pump_management() - Detect startAllPumps timer expiration
// ==========================================

void task_pump_management(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    static PumpState prev_states[4] = {PUMP_OFF, PUMP_OFF, PUMP_OFF, PUMP_OFF};
    static bool prev_manual_mode[4] = {false, false, false, false};
    
    for (;;) {
        if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(10)) == pdTRUE) {
            update_pump_states();
            
            // ✅ CHECK: If startAllPumps is active, check if all pumps stopped naturally
            if (startAllPumpsActive) {
                bool any_pump_running = false;
                bool all_timers_expired = true;
                
                for (int i = 0; i < 4; i++) {
                    if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
                        any_pump_running = true;
                    }
                    
                    // Check if timer is still active
                    if (pumps[i].timerProtected && !is_timer_expired(i)) {
                        all_timers_expired = false;
                    }
                }
                
              // In task_pump_management() - around line where you detect timer expiration
if (!any_pump_running && all_timers_expired) {
    printf("\n[PUMP] ========================================");
    printf("\n[PUMP] ALL PUMPS STOPPED - Timers expired");
    
    // ✅ ONLY set reason if not already set by water lockout or emergency stop
    if (startAllPumps_stop_reason == STOP_REASON_START_ALL_NONE) {
        startAllPumps_stop_reason = STOP_REASON_START_ALL_TIMER_EXPIRED;
        printf("\n[PUMP] Captured stop reason: TIMER_EXPIRED");
    } else {
        printf("\n[PUMP] Stop reason already captured: %d (not overwriting)", 
               startAllPumps_stop_reason);
    }
    
    printf("\n[PUMP] Clearing startAllPumpsActive flag");
    printf("\n[PUMP] ========================================");
    startAllPumpsActive = false;
    
    // Trigger shadow update
    vTaskDelay(pdMS_TO_TICKS(100));
    update_shadow_state();
}
            }
            
            // Detect manual mode changes
            for (int i = 0; i < 4; i++) {
                bool current_manual_mode = false;
                if (pumps[i].state == PUMP_MANUAL_ACTIVE && !startAllPumpsActive) {
                    if (pumps[i].activationSource == ACTIVATION_SOURCE_SHADOW_SINGLE ||
                        pumps[i].activationSource == ACTIVATION_SOURCE_MANUAL_SINGLE) {
                        current_manual_mode = true;
                    }
                }
                
                if (current_manual_mode != prev_manual_mode[i]) {
                    last_shadow_manual_activate[i] = current_manual_mode;
                    prev_manual_mode[i] = current_manual_mode;
                    
                    vTaskDelay(pdMS_TO_TICKS(100));
                    update_shadow_state();
                }
                
                if (pumps[i].state != prev_states[i]) {
                    PumpState oldState = prev_states[i];
                    PumpState newState = pumps[i].state;
                    prev_states[i] = newState;

                    // ── Build unified reason string covering all 12 scenarios ──
                    const char* reason = "unknown";

                    if (newState == PUMP_AUTO_ACTIVE) {
                        // Case 1: Fire detected, pump auto-triggered
                        reason = "automatic";

                    } else if (newState == PUMP_MANUAL_ACTIVE) {
                        // Case 4: Manual activation (single, all, or remote/shadow)
                        ActivationSource src = pumps[i].activationSource;
                        if (src == ACTIVATION_SOURCE_SHADOW_ALL ||
                            src == ACTIVATION_SOURCE_MANUAL_ALL) {
                            // Case 12: startAllPumps from AWS or local all-pumps command
                            reason = "remoteactivated";
                        } else {
                            // Case 4: Single pump manual/shadow activation
                            reason = "manualactivated";
                        }

                    } else if (newState == PUMP_COOLDOWN) {
                        // Case 3: Pump stopped and entered cooldown (MCRC expired)
                        reason = "cooldown";

                    } else if (newState == PUMP_OFF) {
                        // Determine stop reason from lastStopReason
                        switch (pumps[i].lastStopReason) {

                            case STOP_REASON_AUTO_TIMEOUT:
                                // Case 2: Auto pump stopped — flame lost
                                reason = "automaticstop";
                                break;

                            case STOP_REASON_RUN_CAP:
                                // Case 5 & 8: Timer/run cap expired
                                // manual timer_expired → runtimecompleted
                                // auto  max_run_cap   → maxruncap
                                reason = (oldState == PUMP_MANUAL_ACTIVE)
                                         ? "runtimecompleted"
                                         : "maxruncap";
                                break;

                            case STOP_REASON_MANUAL:
                                // Case 6: Single pump stopped manually
                                reason = "manualstop";
                                break;

                            case STOP_REASON_EMERGENCY_STOP:
                            case STOP_REASON_SHADOW_COMMAND:
                                // Case 7: Emergency stop (local or remote)
                                reason = "emergencystop";
                                break;

                            case STOP_REASON_WATER_LOCKOUT:
                                // Case 9: Water lockout
                                reason = "waterlockout";
                                break;

                            case STOP_REASON_SENSOR_FAULT:
                                // Case 10: Sensor fault
                                reason = "sensorfault";
                                break;

                            case STOP_REASON_PROFILE_CHANGE:
                                // Case 11: Profile change stopped the pump
                                reason = "profilechange";
                                break;

                            default:
                                reason = "manualstop";
                                break;
                        }
                    }

                    int runtime = (newState == PUMP_OFF || newState == PUMP_COOLDOWN) ?
                                  (int)get_pump_running_time(i) : 0;
                    int cooldown = (newState == PUMP_COOLDOWN) ?
                                   (int)(pumps[i].cooldownDuration / 1000) : 0;

                    send_alert_pump_state_change(
                        i,
                        (int)oldState,
                        (int)newState,
                        NULL,
                        NULL,
                        pumps[i].currentIRValue,
                        reason,
                        runtime,
                        cooldown
                    );
                }
            }
            
            xSemaphoreGive(mutexPumpState);
        }
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(100));
    }
}

void task_water_lockout(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    for (;;) {
        if (xSemaphoreTake(mutexWaterState, portMAX_DELAY) == pdTRUE) {
            if (xSemaphoreTake(mutexSensorData, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(10)) == pdTRUE) {
                    check_water_lockout();
                    xSemaphoreGive(mutexPumpState);
                }
                xSemaphoreGive(mutexSensorData);
            }
            xSemaphoreGive(mutexWaterState);
        }
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
    }
}

void task_door_monitoring(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    for (;;) {
        check_door_status();
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
    }
}

void task_command_processor(void *parameter) {
    SystemCommand cmd;
    for (;;) {
        if (xQueueReceive(commandQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            // Check emergency stop before processing any pump commands
            if (emergencyStopActive && 
                (cmd.type == CMD_MANUAL_PUMP || 
                 cmd.type == CMD_MANUAL_ALL_PUMPS ||
                 cmd.type == CMD_EXTEND_TIME)) {
                printf("[CMD] Command blocked - Emergency stop active\n");
                continue;
            }
            
            switch (cmd.type) {
                case CMD_MANUAL_PUMP:
                    if (xSemaphoreTake(mutexPumpState, portMAX_DELAY) == pdTRUE) {
                        if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(10)) == pdTRUE) {
                            manual_activate_pump(cmd.pumpIndex);
                            xSemaphoreGive(mutexWaterState);
                        }
                        xSemaphoreGive(mutexPumpState);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        update_shadow_state();  // Send acknowledgement
                    }
                    break;
                case CMD_MANUAL_ALL_PUMPS:
                    // ✅ RETIRED: "start all pumps" is now driven solely by the
                    // AWS IoT device shadow. The web page writes the desired
                    // runtime (seconds) to "_manual_start_all_pumps_duration"
                    // alongside "startallpumps": true, which is handled in the
                    // shadow-delta handler above (see shadow_manual_activate_all_pumps_with_duration).
                    // This local/queue-based path is intentionally a no-op now
                    // so it can't fire pumps with a stale, undefined duration.
                    printf("\n[CMD] CMD_MANUAL_ALL_PUMPS is deprecated - use the device shadow "
                           "(\"startallpumps\" + \"_manual_start_all_pumps_duration\") instead");
                    break;
                case CMD_STOP_PUMP:
                    if (xSemaphoreTake(mutexPumpState, portMAX_DELAY) == pdTRUE) {
                        manual_stop_pump(cmd.pumpIndex);
                        
                        // Check if we should reset startAllPumpsActive
                        if (startAllPumpsActive) {
                            bool any_manual_active = false;
                            for (int i = 0; i < 4; i++) {
                                if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
                                    any_manual_active = true;
                                    break;
                                }
                            }
                            
                            if (!any_manual_active) {
                                startAllPumpsActive = false;
                                printf("\n[CMD] All pumps stopped, resetting startAllPumps to false");
                            }
                        }
                        
                        xSemaphoreGive(mutexPumpState);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        update_shadow_state();  // Send acknowledgement
                    }
                    break;
                case CMD_STOP_ALL_PUMPS:
                    if (xSemaphoreTake(mutexPumpState, portMAX_DELAY) == pdTRUE) {
                        emergency_stop_all_pumps(STOP_REASON_MANUAL);
                        
                        // Reset startAllPumps when all pumps are stopped
                        startAllPumpsActive = false;
                        
                        xSemaphoreGive(mutexPumpState);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        update_shadow_state();  // Send acknowledgement
                    }
                    break;
                case CMD_EXTEND_TIME:
                    if (xSemaphoreTake(mutexPumpState, portMAX_DELAY) == pdTRUE) {
                        extend_manual_runtime(cmd.pumpIndex, cmd.value);
                        xSemaphoreGive(mutexPumpState);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        update_shadow_state();  // Send acknowledgement
                    }
                    break;
                case CMD_CHANGE_PROFILE:
                    if (xSemaphoreTake(mutexSystemState, portMAX_DELAY) == pdTRUE) {
                        SystemProfile newProfile = convert_profile_number_to_enum(cmd.profileValue);
                        apply_system_profile(newProfile);
                        shadow_profile = cmd.profileValue;
                        printf("[SYSTEM] Profile changed to: %s\n", profiles[newProfile].name);
                        xSemaphoreGive(mutexSystemState);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        update_shadow_state();  // Send acknowledgement
                    }
                    break;
                case CMD_GET_STATUS:
                    display_system_status();
                    break;
                default: break;
            }
        }
    }
}

void task_mqtt_publish(void *parameter) {
    mqtt_publish_message_t msg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    printf("\n[MQTT] Publish task started");
    
    while (1) {
        if (xQueueReceive(mqtt_publish_queue, &msg, pdMS_TO_TICKS(100))) {
            
            if (mqtt_connected && mqtt_client) {
                printf("\n[MQTT] Publishing to: %s", msg.topic);
                
                int msg_id = esp_mqtt_client_publish(mqtt_client, msg.topic, 
                                                    msg.payload, 0, 1, 0);
                
                if (msg_id < 0) {
                    printf("\n[MQTT] Publish failed (error: %d)", msg_id);
                    
                    // Store to persistent storage on failure
                    store_alert_to_spiffs(msg.topic, msg.payload);
                    
                    // Requeue for retry (limited attempts)
                    static int requeue_count = 0;
                    if (requeue_count < 2) {
                        printf("\n[MQTT] Requeuing message (attempt %d/2)", requeue_count + 1);
                        xQueueSendToFront(mqtt_publish_queue, &msg, pdMS_TO_TICKS(10));
                        requeue_count++;
                    } else {
                        printf("\n[MQTT] Max requeue attempts reached, keeping in persistent storage");
                        requeue_count = 0;
                    }
                } else {
                    printf("\n[MQTT] Published successfully (msg_id=%d)", msg_id);
                }
            } else {
                // MQTT not connected, store to persistent storage
                printf("\n[MQTT] Not connected - storing alert to persistent storage");
                store_alert_to_spiffs(msg.topic, msg.payload);
            }
        }
        
        // Periodically check for pending alerts when online
        static TickType_t last_pending_check = 0;
        TickType_t current_time = xTaskGetTickCount();
        
        if (mqtt_connected && mqtt_client && 
            (current_time - last_pending_check) > pdMS_TO_TICKS(30000)) {
            last_pending_check = current_time;
            
            // Send pending alerts from storage
            int pending_count = spiffs_get_pending_alert_count();
            if (pending_count > 0) {
                printf("\n[MQTT] Found %d pending alerts in storage, sending...", pending_count);
                send_pending_alerts_from_storage();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
// ========================================
// PERIODIC TASKS FUNCTION
// ========================================

static void perform_periodic_tasks(void) {
    static TickType_t last_heartbeat = 0;
    static TickType_t last_system_status = 0;
    
    TickType_t current_time = xTaskGetTickCount();
    
    // CHECK AND RESET startAllPumps STATE (this now triggers shadow updates internally)
    check_and_reset_start_all_pumps();
    
    // Heartbeat (every 60 seconds)
    if ((current_time - last_heartbeat) > pdMS_TO_TICKS(HEARTBEAT_INTERVAL)) {
        send_heartbeat();
        last_heartbeat = current_time;
    }
    
    // System status (every 70 seconds)
    if ((current_time - last_system_status) > pdMS_TO_TICKS(SYSTEM_STATUS_INTERVAL)) {
        send_system_status();
        last_system_status = current_time;
    }
 
}

static void save_registration_status(bool registered)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("device_config", NVS_READWRITE, &nvs_handle);

    if (err == ESP_OK) {
        nvs_set_u8(nvs_handle, "registered", registered ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        printf("\n Registration status saved: %s", registered ? "YES" : "NO");
    } else {
        printf("\n Failed to save registration status");
    }
}

static bool is_any_network_connected(void) {
#if GSM_ENABLED
    return is_wifi_connected() || gsm_manager_is_connected();
#else
    return is_wifi_connected();
#endif
}


static const char* get_current_network_name(void) {
    if (is_wifi_connected()) return "WiFi";
#if GSM_ENABLED
    if (gsm_manager_is_connected()) return "GSM";
#endif
    return "None";
}

// ========================================
// STATE MACHINE TASK
// ========================================

void task_state_machine(void *parameter) {
    static TickType_t last_mqtt_check = 0;
    static TickType_t last_network_check = 0;
    static int wifi_reconnect_attempts = 0;
    static int gsm_reconnect_attempts = 0;
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    for (;;) {
        TickType_t current_time = xTaskGetTickCount();
        
        switch (current_state) {
            case STATE_INIT:
                printf("\n[STATE] INIT\n");
                wifi_consecutive_failures = 0;
                current_state = STATE_WIFI_CONNECTING;
                last_state_change = current_time;
                break;
                
            case STATE_WIFI_CONNECTING:
                if (is_wifi_connected()) {
                    printf("\n[STATE] WiFi Connecting");
                    
                    // FIXED: Use correct function signature
                    time_manager_notify_network(true, TIME_NET_WIFI);
                    
                    current_active_network = ACTIVE_NET_WIFI;
                    wifi_consecutive_failures = 0;
                    printf("\n[STATE] Time sync started in background");
                    printf("\n[STATE] -> CHECK_PROVISION");
                    current_state = STATE_CHECK_PROVISION;
                    last_state_change = current_time;
                    
                } else if ((current_time - last_state_change) > pdMS_TO_TICKS(15000)) {
				    wifi_consecutive_failures++;
				    printf("\n[STATE] ========================================");
				    printf("\n[STATE] WiFi Connection Failed");
				    printf("\n[STATE] Failure #%d/%d", wifi_consecutive_failures, WIFI_MAX_RETRY_BEFORE_GSM);
				    printf("\n[STATE] ========================================");
    
				    time_manager_notify_network(false, TIME_NET_WIFI);
				    
		    #if GSM_ENABLED
		    if (wifi_consecutive_failures >= WIFI_MAX_RETRY_BEFORE_GSM) {
		        printf("\n[STATE] Max WiFi failures reached!");
		        printf("\n[STATE] Switching to GSM fallback...");
		        printf("\n[STATE] Total WiFi downtime: %d seconds", wifi_consecutive_failures * 45);
		        current_state = STATE_GSM_CONNECTING;
		        last_state_change = current_time;
		    }else {
                        // Retry WiFi
                        printf("\n[STATE] Retrying WiFi...");
                        wifi_disconnect();
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        wifi_reconnect();
                        last_state_change = current_time;
                    }
                    
#else
                    // GSM disabled, just retry WiFi
                    printf("\n[STATE] Retrying WiFi...");
                    wifi_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    init_wifi();
                    last_state_change = current_time;
#endif
                }
                break;
                
#if GSM_ENABLED
            // ========================================
            // NEW: GSM CONNECTING STATE
            // ========================================
            case STATE_GSM_CONNECTING:
                printf("\n[STATE] GSM_CONNECTING");
                
                // Initialize GSM if not already done
                if (!gsm_active) {
                    printf("\n[STATE] Initializing GSM manager...");
                    if (gsm_manager_init() != ESP_OK) {
                        printf("\n[STATE] GSM init failed, retrying WiFi...");
                        wifi_consecutive_failures = 0;  // Reset to try WiFi again
                        current_state = STATE_WIFI_CONNECTING;
                        last_state_change = current_time;
                        break;
                    }
                }
                
                // Try to connect GSM
                printf("\n[STATE] Connecting GSM...");
                if (gsm_manager_connect() == ESP_OK) {
                    printf("\n[STATE] GSM Connected!");
                    
                    // Note: time_manager is notified inside gsm_manager's event handler
                    // via time_manager_notify_network(true, TIME_NET_GSM)
                    
                    current_active_network = ACTIVE_NET_GSM;
                    gsm_reconnect_attempts = 0;
                    printf("\n[STATE] -> CHECK_PROVISION (via GSM)");
                    current_state = STATE_CHECK_PROVISION;
                    last_state_change = current_time;
                    
                } else {
                    gsm_reconnect_attempts++;
                    printf("\n[STATE] GSM connection failed (attempt %d/3)", gsm_reconnect_attempts);
                    
                    if (gsm_reconnect_attempts >= 3) {
                        printf("\n[STATE] GSM failed after 3 attempts, going to ERROR state");
                        current_state = STATE_ERROR;
                    } else {
                        // Wait and retry GSM
                        printf("\n[STATE] Waiting 10s before GSM retry...");
                        vTaskDelay(pdMS_TO_TICKS(10000));
                    }
                    last_state_change = current_time;
                }
                break;
#endif
                
            case STATE_CHECK_PROVISION:
			    printf("\n[STATE] Checking provisioning status (Network: %s)...", 
			           get_current_network_name());
			    check_provisioning_status();
			    
			    if (is_provisioned) {
			        printf("\n[STATE] Device is provisioned");
			        printf("\n[STATE] Connecting with device certificate");
			        printf("\n[STATE] Thing Name: %s", thing_name);
			        
			        if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
			            subscribe_to_topics();
			            printf("\n[STATE] Device Type: %s", DEVICE_TYPE);
			            
			            // ✅ CHECK IF ALREADY REGISTERED
			            if (is_registered) {
			                printf("\n[STATE] Already registered - skipping to OPERATIONAL");
			                current_state = STATE_OPERATIONAL;
			            } else {
			                printf("\n[STATE] Not registered yet - going to REGISTERING");
			                current_state = STATE_REGISTERING;
			            }
			            
			            last_state_change = current_time;
			        } else {
			            printf("\n[STATE] MQTT connection failed");
			            vTaskDelay(pdMS_TO_TICKS(5000));
			        }
			    } else {
			        printf("\n[STATE] Device NOT provisioned");
		        printf("\n[STATE] -> PROVISIONING");
		        current_state = STATE_PROVISIONING;
		        last_state_change = current_time;
		    }
		    break;
                
            case STATE_PROVISIONING:
                printf("\n[STATE] PROVISIONING MODE");
                if (validate_certificates() != ESP_OK) {
                    printf("\n[STATE] Certificate validation failed!");
                    current_state = STATE_ERROR;
                    break;
                }
                
                if (!provisioning_in_progress) {
                    printf("\n[STATE] Starting provisioning process...");
                    esp_err_t prov_result = start_provisioning();
                    provisioning_in_progress = true;
                    provisioning_timeout = current_time;
                    
                    if (prov_result != ESP_OK) {
                        printf("\n[STATE] Provisioning failed: %s", esp_err_to_name(prov_result));
                        provisioning_in_progress = false;
                        current_state = STATE_ERROR;
                        last_state_change = current_time;
                        break;
                    }
                }
                
                if (provisioning_complete) {
                    printf("\n[STATE] Provisioning complete!");
                    check_provisioning_status();
                    provisioning_in_progress = false;
                    
                    printf("\n[STATE] Connecting with new device certificate");
                    if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
                        subscribe_to_topics();
                        printf("\n[STATE] REGISTERING");
                        current_state = STATE_REGISTERING;
                        last_state_change = current_time;
                    } else {
                        printf("\n[STATE] MQTT connection failed after provisioning");
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                } else if ((current_time - provisioning_timeout) > pdMS_TO_TICKS(60000)) {
                    printf("\n[STATE] Provisioning timeout (60 seconds)");
                    provisioning_in_progress = false;
                    current_state = STATE_ERROR;
                    last_state_change = current_time;
                }
                break;
                
            case STATE_REGISTERING:
                printf("\n[STATE] REGISTERING");
                if (is_registered) {
			        printf("\n[STATE] Already registered (safety check) - going OPERATIONAL");
			        current_state = STATE_OPERATIONAL;
			        last_state_change = current_time;
			        break;
			    }
                // Reset flags on entry
                if (registration_attempts == 0 && !is_registered) {
                    printf("\n[STATE] Sending registration request...");
                    send_registration();
                    registration_timeout = current_time;
                    registration_attempts++;
                }

                // Check if device was activated by cloud response
                if (device_activated) {
                    save_registration_status(true);
                    is_registered = true;
                    current_state = STATE_OPERATIONAL;
                    registration_attempts = 0;
                    last_state_change = current_time;

                    printf("\n====================================");
                    printf("\nDEVICE REGISTERED SUCCESSFULLY!");
                    printf("\n====================================");
                    printf("\n[STATE] OPERATIONAL");
                    
                } else if ((current_time - registration_timeout) > pdMS_TO_TICKS(30000)) {
                    if (registration_attempts < 3) {
                        printf("\n[STATE] Registration retry %d/3", registration_attempts + 1);
                        send_registration();
                        registration_timeout = current_time;
                        registration_attempts++;
                    } else {
                        printf("\n[STATE] Registration failed after 3 attempts");
                        current_state = STATE_ERROR;
                        last_state_change = current_time;
                    }
                }
                break;
                
            case STATE_OPERATIONAL:
                // ========================================
                // UPDATED: NETWORK MONITORING WITH GSM FALLBACK
                // ========================================
                if ((current_time - last_network_check) > pdMS_TO_TICKS(10000)) {
                    last_network_check = current_time;
                    
                    bool wifi_ok = is_wifi_connected();
                    bool gsm_ok = gsm_manager_is_connected();
                    
                    // Case 1: Currently on WiFi
                    if (current_active_network == ACTIVE_NET_WIFI) {
                        if (!wifi_ok) {
                            printf("\n[STATE] WiFi DISCONNECTED in operational state!");
                            time_manager_notify_network(false, TIME_NET_WIFI);
                            
                            wifi_reconnect_attempts++;
                            printf("\n[STATE] WiFi reconnection attempt %d/5", wifi_reconnect_attempts);
                            
                            // Try to reconnect WiFi
                            if (!wifi_reconnect()) {
#if GSM_ENABLED
                                // WiFi failed, switch to GSM
                                if (wifi_reconnect_attempts >= 5) {
                                    printf("\n[STATE] WiFi reconnection failed, switching to GSM...");
                                    
                                    // Try GSM connection
                                    if (try_gsm_connection()) {
                                        current_active_network = ACTIVE_NET_GSM;
                                        wifi_reconnect_attempts = 0;
                                        last_wifi_retry_on_gsm = current_time;
                                        
                                        // Reconnect MQTT over GSM
                                        if (mqtt_client) {
                                            esp_mqtt_client_stop(mqtt_client);
                                            vTaskDelay(pdMS_TO_TICKS(1000));
                                        }
                                        if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
                                            subscribe_to_topics();
                                            printf("\n[STATE] MQTT reconnected via GSM");
                                            send_pending_alerts_from_storage();
                                        }
                                    } else {
                                        printf("\n[STATE] GSM also failed, going to ERROR state");
                                        current_state = STATE_ERROR;
                                        last_state_change = current_time;
                                    }
                                }
#else
                                // GSM disabled, go to error after max retries
                                if (wifi_reconnect_attempts >= 10) {
                                    printf("\n[STATE] WiFi reconnection failed after 10 attempts");
                                    current_state = STATE_ERROR;
                                    last_state_change = current_time;
                                }
#endif
                            } else {
                                // WiFi reconnected successfully
                                printf("\n[STATE] WiFi RECONNECTED successfully!");
                                time_manager_notify_network(true, TIME_NET_WIFI);
                                wifi_reconnect_attempts = 0;
                                
                                // Reconnect MQTT
                                if (mqtt_client) {
                                    esp_mqtt_client_stop(mqtt_client);
                                    vTaskDelay(pdMS_TO_TICKS(1000));
                                }
                                if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
                                    subscribe_to_topics();
                                    printf("\n[STATE] MQTT reconnected after WiFi recovery");
                                    send_pending_alerts_from_storage();
                                }
                            }
                        } else {
                            // WiFi is connected, reset counter
                            if (wifi_reconnect_attempts > 0) {
                                wifi_reconnect_attempts = 0;
                            }
                        }
                    }
#if GSM_ENABLED
                    // Case 2: Currently on GSM - periodically try to switch back to WiFi
                    else if (current_active_network == ACTIVE_NET_GSM) {
                        if (!gsm_ok) {
                            printf("\n[STATE]  GSM DISCONNECTED!");
                            handle_gsm_disconnect();
                            
                            // Try WiFi first
                            if (wifi_reconnect()) {
                                current_active_network = ACTIVE_NET_WIFI;
                                time_manager_notify_network(true, TIME_NET_WIFI);
                            } else if (try_gsm_connection()) {
                                // GSM reconnected
                                printf("\n[STATE] GSM reconnected");
                            } else {
                                printf("\n[STATE] All networks failed, going to ERROR");
                                current_state = STATE_ERROR;
                                last_state_change = current_time;
                            }
                        } else {
                            // GSM is connected - try WiFi periodically (prefer WiFi over GSM)
                            if ((current_time - last_wifi_retry_on_gsm) > pdMS_TO_TICKS(WIFI_RETRY_WHEN_ON_GSM_MS)) {
                                last_wifi_retry_on_gsm = current_time;
                                printf("\n[STATE] Checking if WiFi is available (prefer WiFi over GSM)...");
                                
                                if (wifi_reconnect()) {
                                    printf("\n[STATE] WiFi available! Switching from GSM to WiFi...");
                                    
                                    // Disconnect GSM
                                    gsm_manager_disconnect();
                                    
                                    current_active_network = ACTIVE_NET_WIFI;
                                    time_manager_notify_network(true, TIME_NET_WIFI);
                                    
                                    // Reconnect MQTT over WiFi
                                    if (mqtt_client) {
                                        esp_mqtt_client_stop(mqtt_client);
                                        vTaskDelay(pdMS_TO_TICKS(1000));
                                    }
                                    if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
                                        subscribe_to_topics();
                                        printf("\n[STATE] MQTT reconnected via WiFi");
                                    }
                                }
                            }
                        }
                    }
#endif
                    // Case 3: No network
                    else if (current_active_network == ACTIVE_NET_NONE) {
                        printf("\n[STATE] No active network, attempting recovery...");
                        if (wifi_reconnect()) {
                            current_active_network = ACTIVE_NET_WIFI;
                        }
#if GSM_ENABLED
                        else if (try_gsm_connection()) {
                            current_active_network = ACTIVE_NET_GSM;
                        }
#endif
                        else {
                            printf("\n[STATE] Network recovery failed");
                            current_state = STATE_ERROR;
                            last_state_change = current_time;
                        }
                    }
                }
                
                // ========================================
                // MQTT CONNECTION MONITORING (every 30 seconds)
                // ========================================
                if ((current_time - last_mqtt_check) > pdMS_TO_TICKS(30000)) {
                    last_mqtt_check = current_time;
                    
                    // Only try MQTT reconnection if network is up
                    if (is_any_network_connected() && !mqtt_connected) {
                        printf("\n[STATE] MQTT disconnected, reconnecting (Network: %s)...",
                               get_current_network_name());
                        
                        if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
                            subscribe_to_topics();
                            printf("\n[STATE] MQTT reconnected successfully");
                            send_pending_alerts_from_storage();
                        } else {
                            printf("\n[STATE] MQTT reconnection failed");
                        }
                    }
                }
                
                // Periodic check for pending alerts
                static TickType_t last_pending_alerts_check = 0;
                if ((current_time - last_pending_alerts_check) > pdMS_TO_TICKS(60000)) {
                    last_pending_alerts_check = current_time;
                    
                    if (mqtt_connected && mqtt_client) {
                        printf("\n[STATE] Periodic check for pending alerts...");
                        check_and_send_pending_alerts(false);
                    }
                }
                
                // Perform periodic tasks (heartbeat, status, etc.)
                perform_periodic_tasks();
                break;
                
            case STATE_ERROR:
                printf("\n[STATE] ERROR");
                printf("\n[STATE] Resetting provisioning state...");
                
                provisioning_complete = false;
                provisioning_in_progress = false;
                is_provisioned = false;
                wifi_reconnect_attempts = 0;
                wifi_consecutive_failures = 0;
                
                // Disconnect all networks
                printf("\n[STATE] Disconnecting all networks...");
                time_manager_notify_network(false, TIME_NET_WIFI);
                wifi_disconnect();
                
#if GSM_ENABLED
                if (gsm_manager_is_connected()) {
                    gsm_manager_disconnect();
                }
#endif
                current_active_network = ACTIVE_NET_NONE;
                
                printf("\n[STATE] Waiting 10 seconds before retry...");
                vTaskDelay(pdMS_TO_TICKS(10000));
                
                printf("\n[STATE] -> INIT (retry)");
                current_state = STATE_INIT;
                last_state_change = current_time;
                break;
        }
        
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(2000));
    }
}

// ========================================
// DISPLAY FUNCTIONS
// ========================================

void display_system_status(void) {
    static int display_count = 0;
    display_count++;

    static char buf[2200];
    int len = 0;

    len += snprintf(buf + len, sizeof(buf) - len, "\n=== STATUS REPORT #%d ===\n", display_count);
    len += snprintf(buf + len, sizeof(buf) - len, "Thing: %s | Provisioned: %s\n", thing_name, is_provisioned ? "YES" : "NO");
    len += snprintf(buf + len, sizeof(buf) - len, "MQTT Connected: %s\n", mqtt_connected ? "YES" : "NO");
    len += snprintf(buf + len, sizeof(buf) - len, "Time Synced: %s\n", time_manager_is_synced() ? "YES" : "NO");

    char timestamp[32];
    if (time_manager_get_timestamp(timestamp, sizeof(timestamp)) == ESP_OK) {
        len += snprintf(buf + len, sizeof(buf) - len, "Current Time (UTC): %s\n", timestamp);
    }

    len += snprintf(buf + len, sizeof(buf) - len, "\nNETWORK STATUS:\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Active Network: %s\n", get_current_network_name());

    len += snprintf(buf + len, sizeof(buf) - len, "\nWIFI STATUS:\n");
    char ip_address[16];
    get_wifi_ip_address(ip_address, sizeof(ip_address));
    len += snprintf(buf + len, sizeof(buf) - len, "Connected: %s | IP: %s | SSID: %s\n",
        is_wifi_connected() ? "YES" : "NO", ip_address, get_current_wifi_ssid());

#if GSM_ENABLED
    len += snprintf(buf + len, sizeof(buf) - len, "\nGSM STATUS:\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Connected: %s | Signal: %d\n",
        gsm_manager_is_connected() ? "YES" : "NO", gsm_manager_get_signal_quality());
#endif

    len += snprintf(buf + len, sizeof(buf) - len, "\nDevice Need Restart: %s", wifi_has_pending_update() ? "YES" : "NO");

    len += snprintf(buf + len, sizeof(buf) - len, "\nstartAllPumps Active: %s\n", startAllPumpsActive ? "YES" : "NO");
    if (startAllPumpsActive) {
        TickType_t elapsed = xTaskGetTickCount() - startAllPumpsActivationTime;
        len += snprintf(buf + len, sizeof(buf) - len, "  Active for: %u seconds (configured runtime: %lu seconds)\n",
            (unsigned int)(elapsed * portTICK_PERIOD_MS / 1000), startAllPumpsConfiguredDurationMs / 1000UL);
    }

    const char* profileName = "Unknown";
    if (currentProfile >= WILDLAND_STANDARD && currentProfile <= CONTINUOUS_FEED) {
        profileName = profiles[currentProfile].name;
    }
    len += snprintf(buf + len, sizeof(buf) - len, "Current Profile: %d (%s)\n", convert_profile_enum_to_number(currentProfile), profileName);
    len += snprintf(buf + len, sizeof(buf) - len, "Emergency Stop: %s\n", emergencyStopActive ? "ACTIVE" : "INACTIVE");
    len += snprintf(buf + len, sizeof(buf) - len, "Water Lockout: %s\n", waterLockout ? "YES" : "NO");
    len += snprintf(buf + len, sizeof(buf) - len, "Continuous Feed: %s\n", continuousWaterFeed ? "YES" : "NO");

    len += snprintf(buf + len, sizeof(buf) - len, "\nPUMP STATUS:\n");
    for (int i = 0; i < 4; i++) {
        const char* state_str = get_pump_state_string(i);
        const char* stop_reason_str = get_stop_reason_string(pumps[i].lastStopReason);
        const char* activation_str = get_activation_source_string(pumps[i].activationSource);
        len += snprintf(buf + len, sizeof(buf) - len, "Pump %d (%s): State=%s, Running=%s, Source=%s, StopReason=%s\n",
            i+1, pumps[i].name, state_str, pumps[i].isRunning ? "YES" : "NO", activation_str, stop_reason_str);
    }

    len += snprintf(buf + len, sizeof(buf) - len, "\nSENSOR STATUS:\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Water Level: %.1f%%\n", level_s);
    len += snprintf(buf + len, sizeof(buf) - len, "IR Sensors: N=%.1f%%, S=%.1f%%, E=%.1f%%, W=%.1f%%\n",
        ir_s1, ir_s2, ir_s3, ir_s4);
    len += snprintf(buf + len, sizeof(buf) - len, "Battery: %.2fV | Solar: %.2fV\n", bat_v, sol_v);
    len += snprintf(buf + len, sizeof(buf) - len, "Inverters: Inv1=%.1fV | Inv2=%.1fV | Inv3=%.1fV | Inv4=%.1fV\n",
        volt1, volt2, volt3, volt4);
    len += snprintf(buf + len, sizeof(buf) - len, "CT1=%.2f A, CT2=%.2f A, CT3=%.2f A, CT4=%.2f A\n",
        currentSensors[0].currentValue, currentSensors[1].currentValue,
        currentSensors[2].currentValue, currentSensors[3].currentValue);

    FireDetectionInfo* fireInfo = get_fire_detection_info();
    len += snprintf(buf + len, sizeof(buf) - len, "\nFIRE DETECTION STATUS:\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Suppression Mode: %s\n", get_suppression_mode());
    len += snprintf(buf + len, sizeof(buf) - len, "Active Sectors: %d (%s)\n",
        fireInfo->detectedSectorCount, fireInfo->detectedSectorNames[0] ? fireInfo->detectedSectorNames : "none");

    len += snprintf(buf + len, sizeof(buf) - len, "\nSYSTEM STATUS:\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Suppression Active: %s\n", is_suppression_active() ? "YES" : "NO");
    len += snprintf(buf + len, sizeof(buf) - len, "Door: %s\n", doorOpen ? "OPEN" : "CLOSED");
    if (doorOpen) {
        unsigned long openTime = (xTaskGetTickCount() * portTICK_PERIOD_MS - doorOpenTime) / 1000;
        len += snprintf(buf + len, sizeof(buf) - len, "Door open for: %lu seconds\n", openTime);
    }

    // Single write — this is the actual fix for dropped characters
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}

static void get_mac_address(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_address, sizeof(mac_address), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("\nDevice MAC: %s", mac_address);
}

// ========================================
// APPLICATION ENTRY POINT - OPTIMIZED
// ========================================
void app_main(void) {
    
    
    esp_log_level_set("*", ESP_LOG_NONE);
    
    printf("\n[INIT] GUARDIAN FIRE SYSTEM STARTING...\n");
    // Initialize boot time for sensor warmup
    boot_time = xTaskGetTickCount();
    sensors_ready = false;
    
    printf("\n[INIT] Sensor warmup period: %d seconds\n", SENSOR_WARMUP_SECONDS);
    
    
    get_mac_address();
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    // Initialize time manager
   time_manager_init();
      
    // Initialize SPIFFS
	spiffs_init();
	
	// ✅ CRITICAL: Load WiFi credentials from SPIFFS BEFORE WiFi init
	printf("\n[BOOT] Loading WiFi credentials from SPIFFS...\n");
	bool credentials_loaded = load_wifi_credentials_from_spiffs();
	
	if (credentials_loaded) {
	    printf("\n[BOOT]  Custom WiFi credentials loaded from SPIFFS");
	} else {
	    printf("\n[BOOT]  No custom credentials in SPIFFS, using defaults");
	}
	
	// NEW: Check for pending alerts on boot
	int pending_alerts = spiffs_get_pending_alert_count();
	if (pending_alerts > 0) {
	    printf("\n[BOOT] Found %d pending alerts in SPIFFS storage", pending_alerts);
	    spiffs_print_alert_summary();
	}
	
	// Load Thing Name if exists
	snprintf(thing_name, sizeof(thing_name), "FD_%s_%s", DEVICE_TYPE, mac_address);
	
	printf("\n[BOOT] Checking WiFi configuration...");
	if (wifi_has_custom_credentials()) {
	    printf("\n[BOOT] Using stored WiFi credentials from SPIFFS");
	    printf("\n[BOOT] SSID: %s", get_current_wifi_ssid());
	    printf("\n[BOOT] Password: %s", get_current_wifi_password());
	} else {
	    printf("\n[BOOT] Using default WiFi credentials");
	    printf("\n[BOOT] Default SSID: %s", WIFI_SSID);
	    printf("\n[BOOT] Default Password: %s", WIFI_PASSWORD);
	}
	printf("\n[BOOT] Pending Update: %s", wifi_has_pending_update() ? "YES" : "NO");
    // Create provisioning mutex
    provisioning_mutex = xSemaphoreCreateMutex();
    
    // Check provisioning status
    check_provisioning_status();
	is_registered = load_registration_status();
    if (is_registered) {
        printf("\n[BOOT] Device already registered - will skip registration");
    }
    
    // Initialize hardware
    init_fire_suppression_system();
    #if GSM_ENABLED
    gsm_manager_init();
    printf("\n[INIT] ========================================");
#else
    printf("\n[INIT] GSM fallback: DISABLED (compile-time)");
#endif
    
    init_wifi();
    
    // Initialize RTOS components with optimized sizes
    mutexSensorData = xSemaphoreCreateMutex();
    mutexPumpState = xSemaphoreCreateMutex();
    mutexWaterState = xSemaphoreCreateMutex();
    mutexSystemState = xSemaphoreCreateMutex();
    commandQueue = xQueueCreate(10, sizeof(SystemCommand));
    mqtt_publish_queue = xQueueCreate(10, sizeof(mqtt_publish_message_t));
    
    // Initialize alert system
    init_alert_system();
    
    // Create tasks with optimized stack sizes
    xTaskCreate(task_state_machine, "State", TASK_STATE_MACHINE_STACK_SIZE, NULL, TASK_PRIORITY_STATE_MACHINE, &taskStateMachineHandle);
    xTaskCreate(task_sensor_reading, "Sensor", TASK_SENSOR_STACK_SIZE, NULL, TASK_PRIORITY_SENSOR, &taskSensorHandle);
    xTaskCreate(task_fire_detection, "Fire", TASK_FIRE_DETECT_STACK_SIZE, NULL, TASK_PRIORITY_FIRE_DETECT, &taskFireDetectionHandle);
    xTaskCreate(task_pump_management, "Pump", TASK_PUMP_MGMT_STACK_SIZE, NULL, TASK_PRIORITY_PUMP_MGMT, &taskPumpManagementHandle);
    xTaskCreate(task_water_lockout, "Water", TASK_WATER_LOCK_STACK_SIZE, NULL, TASK_PRIORITY_WATER_LOCK, &taskWaterLockoutHandle);
    xTaskCreate(task_command_processor, "Cmd", TASK_CMD_STACK_SIZE, NULL, TASK_PRIORITY_CMD, &taskCommandHandle);
    xTaskCreate(task_door_monitoring, "Door", TASK_DOOR_STACK_SIZE, NULL, TASK_PRIORITY_DOOR, &taskDoorHandle);
    xTaskCreate(task_serial_monitor, "Mon", TASK_MONITOR_STACK_SIZE, NULL, TASK_PRIORITY_MONITOR, &taskMonitorHandle);
    xTaskCreate(task_mqtt_publish, "Mqtt", TASK_MQTT_PUBLISH_STACK_SIZE, NULL, TASK_PRIORITY_MQTT_PUBLISH, &taskMqttPublishHandle);
    
    printf("[INIT] System Running\n");
    
    // Main loop
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}