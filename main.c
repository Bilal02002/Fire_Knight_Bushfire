/*#include <stdio.h>
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

// ========================================
// AWS IoT CONFIGURATION
// ========================================
#define AWS_IOT_ENDPOINT "a3t2gw3osxkpr2-ats.iot.us-east-1.amazonaws.com"
#define AWS_IOT_PORT 8883
#define CLAIM_THING_NAME "ClaimDevice"

// Provisioning topics

#define SECURE_PROVISION_REQUEST_TOPIC      "Provision/Request/%s"      // Device → Lambda
#define SECURE_PROVISION_RESPONSE_TOPIC     "Provision/Response/%s"     // Lambda → Device
#define SECURE_PROVISION_TIMEOUT_MS         30000   // 30 seconds for Lambda response
#define REGISTER_THING_TIMEOUT_MS           30000   // 30 seconds for RegisterThing



// ========================================
// SYSTEM CONFIGURATION
// ========================================
#define TASK_SENSOR_STACK_SIZE      8192
#define TASK_FIRE_DETECT_STACK_SIZE 8192
#define TASK_PUMP_MGMT_STACK_SIZE   6144
#define TASK_WATER_LOCK_STACK_SIZE  4096
#define TASK_MONITOR_STACK_SIZE     4096
#define TASK_DOOR_STACK_SIZE        4096
#define TASK_CMD_STACK_SIZE         8192
#define TASK_MQTT_PUBLISH_STACK_SIZE 4096
#define TASK_STATE_MACHINE_STACK_SIZE 6144
#define TASK_ALERT_STACK_SIZE       4096

// Task priorities
#define TASK_PRIORITY_SENSOR        2
#define TASK_PRIORITY_FIRE_DETECT   3
#define TASK_PRIORITY_PUMP_MGMT     2
#define TASK_PRIORITY_WATER_LOCK    2
#define TASK_PRIORITY_MONITOR       1
#define TASK_PRIORITY_DOOR          1
#define TASK_PRIORITY_CMD           2
#define TASK_PRIORITY_MQTT_PUBLISH  1
#define TASK_PRIORITY_STATE_MACHINE 3
#define TASK_PRIORITY_ALERT         2

// Device Configuration
#define MAX_TOPIC_LENGTH 128
#define DEVICE_TYPE "G"

// Timing intervals (in milliseconds)
#define HEARTBEAT_INTERVAL          60000
#define SYSTEM_STATUS_INTERVAL      70000
#define SHADOW_UPDATE_INTERVAL      30000

// Memory optimization constants
#define MIN_FREE_HEAP_THRESHOLD     10240  // 10KB minimum free heap
#define MAX_JSON_PAYLOAD_SIZE       1024   // Reduced from 2048
#define MQTT_QOS_LEVEL              0      // Use QoS 0 for memory efficiency

#define ALERT_SYSTEM_ENABLED 1

#define SENSOR_WARMUP_SECONDS 15  // Wait 15 seconds for sensors to stabilize


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
    ALERT_TYPE_PROFILE_CHANGE,
    ALERT_TYPE_EMERGENCY_STOP,           
    ALERT_TYPE_SYSTEM_RESET,             
    ALERT_TYPE_START_ALL_PUMPS,          
    ALERT_TYPE_PUMP_STATE_CHANGE,
    ALERT_TYPE_PUMP_EXTEND_TIME,
    ALERT_TYPE_FIRE_DETECTED,
    ALERT_TYPE_FIRE_CLEARED,         
    ALERT_TYPE_MULTIPLE_FIRES,
    ALERT_TYPE_WATER_LOCKOUT,
    ALERT_TYPE_DOOR_STATUS,
    ALERT_TYPE_MANUAL_OVERRIDE,
    ALERT_TYPE_AUTO_ACTIVATION,    
    ALERT_TYPE_WIFI_UPDATE,       
    ALERT_TYPE_SYSTEM_ERROR,
    ALERT_TYPE_SENSOR_FAULT,
    ALERT_TYPE_CONTINUOUS_FEED, 
    ALERT_TYPE_CURRENT_SENSOR_FAULT,
    ALERT_TYPE_IR_SENSOR_FAULT,
    ALERT_TYPE_HARDWARE_CONTROL_FAIL,
    ALERT_TYPE_ADC_INIT_FAIL,
    ALERT_TYPE_PCA9555_FAIL,
        
    // System State Alerts
    ALERT_TYPE_GRACE_PERIOD_EXPIRED,
    ALERT_TYPE_PUMP_COOLDOWN,
    ALERT_TYPE_TIMER_OVERRIDE,
    
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
    STATE_CHECK_PROVISION,
    STATE_PROVISIONING,
    STATE_REGISTERING,
    STATE_OPERATIONAL,
    STATE_ERROR
} system_state_t;


// MQTT Publish Queue
typedef struct {
    char topic[128];
    char payload[1024];  // Reduced from 2048
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
            float sensorTemperature;
            char stopReason[32];
            int totalRuntime;
            int cooldownDuration;
            int previousRuntime;
        } pump;
        
        // Pump extension data
        struct {
            int pumpId;
            char pumpName[16];
            int extensionCode;
            int extensionDuration;
            int newTotalRuntime;
        } pumpExtend;
        
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
            // NEW: Fire detection type info
            int fireType;                    // 0=none, 1=single, 2=multiple, 3=full
            char fireTypeString[24];         // "SINGLE_SECTOR", "MULTIPLE_SECTORS", "FULL_SYSTEM"
            int totalActiveSectors;          // Total number of sectors with fire
            char allActiveSectors[32];       // List of all active sectors e.g. "N,S,E"
        } fire;
        
        // Multiple fires data
        struct {
            int activeFireCount;
            struct {
                char sector[16];
                float temperature;
                bool pumpActive;
            } affectedSectors[4];
            float waterLevel;
            float estimatedRuntime;
            // NEW: Fire detection type info
            int fireType;                    // 2=multiple, 3=full
            char fireTypeString[24];         // "MULTIPLE_SECTORS" or "FULL_SYSTEM"
        } multipleFires;
        
        // Water lockout data
        struct {
            bool activated;
            float currentWaterLevel;
            float minThreshold;
            bool allPumpsDisabled;
            bool continuousFeedActive;
            char systemStatus[32];
        } waterLockout;
        
        // Door status data
        struct {
            bool opened;
            char action[16];
            bool doorState;
            bool securityConcern;
            int wasOpenDuration;
        } door;
        
        // Manual override data
        struct {
            bool activated;
            char action[16];
            int manualPumpCount;
            struct {
                int pumpId;
                char pumpName[16];
                char state[16];
            } manualPumps[4];
            bool autoProtectionDisabled;
            bool autoProtectionEnabled;
            char activationSource[32];
            char systemMode[16];
            int totalManualDuration;
        } manualOverride;
        
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
            int estimatedRuntime;        // Minutes remaining
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
static int last_shadow_extend_time[4] = {-1, -1, -1, -1};
static bool last_shadow_stop_pump[4] = {false,false,false, false};
static bool last_shadow_manual_mode[4] = {false, false, false, false};
static int last_reported_extend_time[4] = {-1, -1, -1, -1};
static bool last_reported_manual_mode[4] = {false, false, false, false};

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
static bool fire_alerts_active[4] = {false};
static int active_fire_count = 0;

// NEW: Start All Pumps tracking
static bool startAllPumpsActive = false;
static TickType_t startAllPumpsActivationTime = 0;
static int pending_extend_ack[4] = {-1, -1, -1, -1};
static int previous_extend_time[4] = {-1, -1, -1, -1};

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
extern bool emergencyStopActive; 


// ========================================
// HELPER FUNCTIONS - ENUM TO STRING
// ========================================

static const char* get_alert_type_string(alert_type_t type) {
    switch(type) {
        case ALERT_TYPE_PROFILE_CHANGE: return "profileChange";
        case ALERT_TYPE_EMERGENCY_STOP: return "emergencyStop";
        case ALERT_TYPE_SYSTEM_RESET: return "systemReset";
        case ALERT_TYPE_START_ALL_PUMPS: return "startAllPumps";
        case ALERT_TYPE_PUMP_STATE_CHANGE: return "pumpStateChange";
        case ALERT_TYPE_PUMP_EXTEND_TIME: return "pumpExtendTime";
        case ALERT_TYPE_FIRE_DETECTED: return "fireDetected";
        case ALERT_TYPE_FIRE_CLEARED: return "fireCleared";
        case ALERT_TYPE_MULTIPLE_FIRES: return "multipleFires";
        case ALERT_TYPE_WATER_LOCKOUT: return "waterLockout";
        case ALERT_TYPE_DOOR_STATUS: return "doorStatus";
        case ALERT_TYPE_MANUAL_OVERRIDE: return "manualOverride";
        case ALERT_TYPE_AUTO_ACTIVATION: return "autoActivation";
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
                
        // System state
        case ALERT_TYPE_GRACE_PERIOD_EXPIRED: return "gracePeriodExpired";
        case ALERT_TYPE_PUMP_COOLDOWN: return "pumpCooldown";
        case ALERT_TYPE_TIMER_OVERRIDE: return "timerOverride";
        
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

// ==========================================
// HELPER FUNCTIONS FOR PROFILE CONVERSION
// ==========================================

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



// ========================================
// AWS CERTIFICATES
// ========================================


// ==================== AWS CLAIM CERTIFICATE ====================
static const char AWS_CA_CERT[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
"rqXRfboQnoZsG4q5WTP468SQvvG5\n"
"-----END CERTIFICATE-----";

// ==================== AWS CLAIM CERTIFICATE ====================
static const char AWS_CLAIM_CERT[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDWTCCAkGgAwIBAgIUVLqEQh+m3sbqpAf/pgxhfhm13kkwDQYJKoZIhvcNAQEL\n"
"BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g\n"
"SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTI1MDgwNzEwNDkw\n"
"NVoXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0\n"
"ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKPCODUQ/XtJBxhqoq0+\n"
"k4H8mrF08aRMaZ5H597t/bEp5kCfIkpx/Q2T8wx4XJGZSZKS/J7OORLvJqQlRItR\n"
"7x8x+0s5PcqjSVIvr77bMElf5R9dO26YpgGG3L1DhSXLbXthznALceDdUWoNl1d+\n"
"HeZs7XsaPCDIswHWK0vFvk2C2HknPY5METseFxAqS/Uzu3oAu68qJcbgrj6wNDWe\n"
"7pbM/+A2qvYi2w7gnYIMi6DN/3yFx7tTQdQyntOFRcS4SE1wcY/NkH+MTsyY418T\n"
"BW4NDQfPajeSLcMEUgXIdRKonSp3PkgS3Xr3oOppec55D3SVVNWTdGEe/zh9qmjP\n"
"YQsCAwEAAaNgMF4wHwYDVR0jBBgwFoAU4SIhyBUw9JO5U+wtNZ/MUg9SyuIwHQYD\n"
"VR0OBBYEFC/OCRI7hQ4mupBmaZcAWlTRBTyWMAwGA1UdEwEB/wQCMAAwDgYDVR0P\n"
"AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQDJUiJ4wWJ70D0rW2JiVcXqkyui\n"
"d+tWeMKMc4qxVBAyRvH8oWZn8l9+gC8+7+6dH6mc5YRx0tGl0jbt2KwPjDyS8zn1\n"
"HXMpa+SEmmibXJ3rKLE4rb5uHRVNfEe+bNruOcpRcp6Vmgn9Ortq314acBsURE8s\n"
"s5WWWzv/go1UiZ9x6AWUXWb/kRwxvf9EuU8/t0jtfhxCxmuFrg/APBaZ4c6A5K+j\n"
"jGJ0BIeARFJap49Rc0QPryihB/MDJUyEY/KT8SXUjJlzi82mOb8MPGK8WzAPtW3c\n"
"QeTBQ/PYSEFwt3I/JuwNiHQ+ebsm9MK9NWgSv5pMTWFd/N3E5se7jcwiaKu6\n"
"-----END CERTIFICATE-----";

// ==================== AWS CLAIM PRIVATE KEY ====================
static const char AWS_CLAIM_PRIVATE_KEY[] =
"-----BEGIN RSA PRIVATE KEY-----\n"
"MIIEowIBAAKCAQEAo8I4NRD9e0kHGGqirT6TgfyasXTxpExpnkfn3u39sSnmQJ8i\n"
"SnH9DZPzDHhckZlJkpL8ns45Eu8mpCVEi1HvHzH7Szk9yqNJUi+vvtswSV/lH107\n"
"bpimAYbcvUOFJctte2HOcAtx4N1Rag2XV34d5mztexo8IMizAdYrS8W+TYLYeSc9\n"
"jkwROx4XECpL9TO7egC7ryolxuCuPrA0NZ7ulsz/4Daq9iLbDuCdggyLoM3/fIXH\n"
"u1NB1DKe04VFxLhITXBxj82Qf4xOzJjjXxMFbg0NB89qN5ItwwRSBch1EqidKnc+\n"
"SBLdeveg6ml5znkPdJVU1ZN0YR7/OH2qaM9hCwIDAQABAoIBAQCCxJ950N16S7C8\n"
"0LqjOas1S/CD8Ozd1J8q5CTHIqlJhjn2NJ1/cVMwOosF1D+njQ7xWysb7XYqJotm\n"
"3NPFpWIcOR+AzG8JmCb+2FGxSPtgPJGM4DiLcp5t7bHr+TUkHzSIKGxfkOQZOuK+\n"
"m6fVGELsNOPXP/XwABTiTJI6aegzoBeamoOWVT4FV9vpG73sYoswaviCCaqvgkJr\n"
"3OYyDv+ml7GQ8nG3PRISxGI9t3KmwGoUO4+YbAusCOnISoC2UcL4mGSX1U1y5IbK\n"
"ULDlSOxEjUiv4MOPptZr54TQspr0N/6sjK294D9tnb02zgcNS/bZpnHd8pPtvhJA\n"
"OGcAbE3RAoGBANhCikaxVmtx2b3TQrPYywAXjxyr932oAGedp2JgvTOKciFj2Pe7\n"
"NZjrY9ocvJTDY2d+GcsYZVNYtq6ynSH7eJArFLkx7UQLs/qu38YGvp6plNtHMtLa\n"
"hcjExgUyEsutK8eQvOXeQIE3rX4ad2aK68j8ePxuQZuPapIyQ5XgsyAVAoGBAMHZ\n"
"4GPV19EJDgaGaI2D2Icqkg7etw0AGlmZKYZlxE3KSpvCrbvy2+p4nBgjadljBZs1\n"
"mfqkBuNqwvc8MOm315HPrD2nSml4CEw3g7Obai5oibNOoQQc3jADSrYsM9k+Gusp\n"
"BvyGmDE1B5N26U9iue91UO7ZeCExzmkPhxGkPaSfAoGALnzfTKMCeMZYkD3BsPeB\n"
"a9ukn/03joN20s9JFBTHlzTDo/nawiY0N1Mie9iBkVkPHUg2MzpjTa9cVeF/dbah\n"
"DBy2r7jT0DTT06eT4vXANEsv/JMpkbn32Fi0WJmTAMWRC61JbgCAzUYyvVDjKd/j\n"
"H6lmOJ1a7R2/Qv4bGTTcTKECgYBVKdsjATenZkr7IuGcCmh+OX2hescAtyLcaiWM\n"
"Hfl4E39jnsuk3rUu9X3ePPCryI0V+x6Ctr0v/B9bbt4uT84tCQeqrmxKmalLkrgR\n"
"mB219cdJNyoWHHigr1GLZzAAKQC6f3PKTXdfZuTFLGCjt8PoJ6o+xNu5+Z+tGF1G\n"
"qtlKEQKBgErmX6Nks5U+UNsvzMuNXmzLVxBh8wz7VJg1/5M88p+0rMXKWn8BW/sf\n"
"ZKXJ26mzvAMDve6MZCo5iQZcX1oVUWEGDj84qtvXIDn36pJXBDbGgsz5KpQAzqi4\n"
"lpI8zu9ZOEcDDd5SksaOsPMQrPFBLoXtiB/gGG9MC23a072U9ls+\n"
"-----END RSA PRIVATE KEY-----";




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
static void send_alert_pump_extend_time(int pumpIndex, int extensionCode,
                                       int extensionDuration, int newTotalRuntime);
static void send_alert_fire_detected(int sensorIndex, const char* sectorName,
                                    float temperature, bool pumpActivated);
static void send_alert_fire_cleared(int sensorIndex, const char* sectorName,
                                   float currentTemp);
static void send_alert_multiple_fires(int fireCount, float sensorValues[4],
                                     bool pumpStates[4]);
static void send_alert_water_lockout(bool activated, float currentLevel, float threshold);
static void send_alert_door_status(bool opened, int openDuration);
static void send_alert_manual_override(bool activated, int manualDuration);
static void send_alert_auto_activation(void);
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
void send_alert_battery_critical(float batteryVoltage, int estimatedRuntime);
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
            printf("\n[SHADOW] All pumps stopped, resetting startAllPumpsActive to false");
            startAllPumpsActive = false;
            
            // ✅ IMPORTANT: Update shadow immediately (event-driven)
            vTaskDelay(pdMS_TO_TICKS(100));
            update_shadow_state();
        }
    }
}



static bool process_wifi_credentials_from_shadow(cJSON *state) {
    bool credentials_changed = false;
    
    // Check for WiFi configuration object
    cJSON *wifiConfig = cJSON_GetObjectItem(state, "wifiCredentials");
    if (!wifiConfig || !cJSON_IsObject(wifiConfig)) {
        return false;
    }
    
    cJSON *ssid = cJSON_GetObjectItem(wifiConfig, "ssid");
    cJSON *password = cJSON_GetObjectItem(wifiConfig, "password");
    
    if (!ssid || !password) {
        printf("\n[SHADOW] WiFi config incomplete");
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
        printf("\n[SHADOW] WiFi credentials validation failed");
        
        // Report error back to shadow
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                "Invalid WiFi credentials: SSID length=%d, Password length=%d",
                strlen(new_ssid), strlen(new_password));
        send_alert_wifi_invalid(strlen(new_ssid), strlen(new_password), 
                       "SSID empty or password too short");
        return false;
    }
    
    // Check if credentials are different from current
    const char *current_ssid = get_current_wifi_ssid();
    if (strcmp(current_ssid, new_ssid) != 0) {
        credentials_changed = true;
    }
    
    if (credentials_changed) {
        printf("\n[SHADOW] WiFi credentials changed");
        printf("\n[SHADOW] New SSID: %s", new_ssid);
        printf("\n[SHADOW] New Password: %s", new_password);
        
        // Call the correct function to store credentials
        set_wifi_credentials(new_ssid, new_password);
        
       send_alert_wifi_updated(new_ssid, current_ssid);
        
        return true;
    }
    
    return false;
}

// ==========================================
// SHADOW DELTA PROCESSING FUNCTION
// ==========================================

static bool process_shadow_delta(cJSON *state) {
    bool state_changed = false;
    
    if (process_wifi_credentials_from_shadow(state)) {
        printf("\n[SHADOW] WiFi credentials being updated...");
        state_changed = true;
    }
    
    // 1. PROFILE CHANGE
    cJSON *currentProfileJson = cJSON_GetObjectItem(state, "currentProfile");
    if (currentProfileJson) {
        int profileNum = (int)cJSON_GetNumberValue(currentProfileJson);
        printf("\n[SHADOW] Delta: currentProfile = %d", profileNum);
        
        SystemProfile newProfile = convert_profile_number_to_enum(profileNum);
        
        if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (newProfile != currentProfile) {
                apply_system_profile(newProfile);
                shadow_profile = profileNum;
                printf("[SYSTEM] Profile changed to: %s\n", profiles[newProfile].name);
                state_changed = true;
                
                char alert_msg[128];
                snprintf(alert_msg, sizeof(alert_msg),
                        "Profile changed to %s via shadow", profiles[newProfile].name);
            }
            xSemaphoreGive(mutexSystemState);
        }
    }
    
    // 2. EMERGENCY STOP
    cJSON *emergencyStopJson = cJSON_GetObjectItem(state, "emergencyStop");
    if (emergencyStopJson) {
        bool stopCommand = cJSON_IsTrue(emergencyStopJson);
        printf("\n[SHADOW] Delta: emergencyStop = %s", 
               stopCommand ? "true" : "false");
        
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
    cJSON *systemResetJson = cJSON_GetObjectItem(state, "systemReset");
    if (systemResetJson) {
        bool resetCommand = cJSON_IsTrue(systemResetJson);
        printf("\n[SHADOW] Delta: systemReset = %s", 
               resetCommand ? "true" : "false");
        
        if (resetCommand) {
            printf("\n[SHADOW] SYSTEM RESET REQUESTED");
            
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
                        for (int i = 0; i < 4; i++) {
                            last_shadow_pump_manual[i] = false;
                            last_shadow_manual_mode[i] = false;
                            last_shadow_extend_time[i] = -1;
                            last_reported_extend_time[i] = -1;
                            last_shadow_stop_pump[i] = false;
                            pending_extend_ack[i] = -1;
                            previous_extend_time[i] = -1;
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
    
    // 4. START ALL PUMPS
    cJSON *startAllPumpsJson = cJSON_GetObjectItem(state, "startAllPumps");
    if (startAllPumpsJson) {
        bool desiredStartAllPumps = cJSON_IsTrue(startAllPumpsJson);
        printf("\n[SHADOW] startAllPumps delta received: %s", 
               desiredStartAllPumps ? "true" : "false");
        
        if (desiredStartAllPumps != startAllPumpsActive) {
            if (desiredStartAllPumps) {
                printf("\n[SHADOW] Activating ALL pumps via startAllPumps");
                
                if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(50)) == pdTRUE) {
                        bool result = shadow_manual_activate_all_pumps();
                        
                        if (result) {
                            startAllPumpsActive = true;
                            startAllPumpsActivationTime = xTaskGetTickCount();
                            state_changed = true;
                            
                            printf("\n[SHADOW] Sending immediate shadow update after startAllPumps");
                            update_shadow_state();
                            
                            char alert_msg[128];
                            snprintf(alert_msg, sizeof(alert_msg),
                                    "ALL PUMPS activated via startAllPumps");
                            
                        }
                        xSemaphoreGive(mutexWaterState);
                    }
                    xSemaphoreGive(mutexPumpState);
                }
            } else {
                printf("\n[SHADOW] User requested startAllPumps deactivation");
                
                if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                    shadow_manual_stop_all_pumps();
                    startAllPumpsActive = false;
                    state_changed = true;
                    xSemaphoreGive(mutexPumpState);
                }
            }
        }
    }
    
    // 4. INDIVIDUAL PUMP CONTROLS
    const char* pumpNames[4] = {"NorthPump", "SouthPump", "EastPump", "WestPump"};
    
    for (int i = 0; i < 4; i++) {
        cJSON *pumpObj = cJSON_GetObjectItem(state, pumpNames[i]);
        if (pumpObj && cJSON_IsObject(pumpObj)) {
            printf("\n[SHADOW] Processing %s", pumpNames[i]);
            
            //  CHECK FOR stopPump PARAMETER FIRST (highest priority)
            cJSON *stopPumpJson = cJSON_GetObjectItem(pumpObj, "stopPump");
            if (stopPumpJson && cJSON_IsBool(stopPumpJson)) {
				bool stopPumpvalue = cJSON_IsTrue(stopPumpJson);
				//Only Process if value changed 
				if(stopPumpvalue != last_shadow_stop_pump[i]){
					last_shadow_stop_pump[i] = stopPumpvalue;
					state_changed = true;
					
					if(stopPumpvalue){
						//Process STOP Command
						printf("\n[SHADOW] STOP REQUEST for %s via stopPump parameter", pumpNames[i]);
						if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                            extern bool shadow_manual_stop_pump_override_timer(int index);
                            shadow_manual_stop_pump_override_timer(i);

                            char alert_msg[128];
                            snprintf(alert_msg, sizeof(alert_msg),
                                    "User stopped %s via stopPump button", pumps[i].name);
                            

                            xSemaphoreGive(mutexPumpState);
                        }
                        continue;
                        
					}
					else
					{
						printf("\n[SHADOW] stopPump cleared for %s", pumpNames[i]);
					}
				}
            }
          
            
            
            //  CHECK IF PUMP IS TIMER-PROTECTED
            if (pumps[i].timerProtected && !is_timer_expired(i)) {
                unsigned long remaining = get_timer_remaining(i);
                printf("\n[SHADOW] %s is TIMER-PROTECTED (%lu seconds remaining) - IGNORING manualMode changes",
                       pumpNames[i], remaining);
                
                // ONLY SKIP manualMode - Still process extendTime below!
            } else {
                // Process manualMode only if NOT timer-protected
	cJSON *manualModeJson = cJSON_GetObjectItem(pumpObj, "manualMode");
	if (manualModeJson) {
	    bool desiredManualMode = cJSON_IsTrue(manualModeJson);
	    bool currentManualMode = (pumps[i].state == PUMP_MANUAL_ACTIVE);
	    
	    printf("\n[SHADOW] %s manualMode desired=%s, current=%s", 
	           pumpNames[i],
	           desiredManualMode ? "true" : "false",
	           currentManualMode ? "true" : "false");
	    
	    // ✅ FIX: Just track the desired state, don't control hardware
	    if (desiredManualMode != last_shadow_manual_mode[i]) {
	        printf("\n[SHADOW] %s: Acknowledging manualMode change %d -> %d (HARDWARE NOT AFFECTED)",
	               pumpNames[i], last_shadow_manual_mode[i], desiredManualMode);
	        
	        // Update tracking to match desired
	        last_shadow_manual_mode[i] = desiredManualMode;
	        state_changed = true;
	        
	        // Only control hardware if going from false -> true
	        if (desiredManualMode && !currentManualMode) {
	            // Activate pump in hardware
	            if (startAllPumpsActive) {
	                printf("\n[SHADOW] BLOCKED: Cannot activate %s - startAllPumps active",
	                       pumpNames[i]);
	            } else if (can_activate_pump_manually(i)) {
	                if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
	                    if (shadow_manual_activate_pump(i)) {
	                        char alert_msg[128];
	                        snprintf(alert_msg, sizeof(alert_msg),
	                                "Manual activation: %s", pumps[i].name);
	                       
	                    }
	                    xSemaphoreGive(mutexPumpState);
	                }
	            }
	        }
	        // ✅ If going from true -> false, just acknowledge (don't stop pump)
	        else if (!desiredManualMode && currentManualMode) {
	            printf("\n[SHADOW] %s: User set manualMode to false - ACKNOWLEDGING ONLY (pump continues running)",
	                   pumpNames[i]);
	        }
	    }
	}
	            }
	            
	            // ✅ EXTEND TIME PROCESSING
	cJSON *extendTimeJson = cJSON_GetObjectItem(pumpObj, "extendTime");
	if (extendTimeJson && cJSON_IsNumber(extendTimeJson)) {
	    int extendValue = (int)cJSON_GetNumberValue(extendTimeJson);
	    
	    // ✅ Check if value changed from what we last reported
	    if (extendValue != last_shadow_extend_time[i]) {
	        printf("\n[SHADOW] %s: extendTime changed %d -> %d",
	               pumpNames[i], last_shadow_extend_time[i], extendValue);
	        
	        // Only process extensions (0-3), not -1
	        if (extendValue >= 0 && extendValue <= 3 && pumps[i].timerProtected) {
	            printf("\n[SHADOW] Processing extension request for %s: code=%d", 
	                   pumpNames[i], extendValue);
	            
	            if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
	                unsigned long extensionMs = get_duration_from_code(extendValue);
	                
	                if (extensionMs > 0) {
	                    // Extend the timer in hardware
	                    extend_timer_protection(i, extensionMs);
	                    
	                    // ✅ Mark for reporting back the SAME value
	                    last_shadow_extend_time[i] = extendValue;
	                    last_reported_extend_time[i] = extendValue; // Will report this value
	                    state_changed = true;
	                    
	                    char alert_msg[128];
	                    snprintf(alert_msg, sizeof(alert_msg),
	                            "Extended %s by %s", 
	                            pumps[i].name, 
	                            get_duration_code_string(extendValue));
	                  // Calculate new total runtime
					int currentRuntime = (int)(pumps[i].timerDuration / 1000);
					int newTotalRuntime = currentRuntime + (extensionMs / 1000);
					send_alert_pump_extend_time(i, extendValue, extensionMs / 1000, newTotalRuntime);
	                              
	                    printf("\n[SHADOW] %s: Extension applied, will report back extendTime=%d\n",
	                           pumpNames[i], extendValue);
	                }
	                xSemaphoreGive(mutexPumpState);
	            }
	        }
	        // ✅ When user sets back to -1, acknowledge it
	        else if (extendValue == -1) {
	            printf("\n[SHADOW] %s: User reset extendTime to -1, acknowledging\n",
	                   pumpNames[i]);
	            last_shadow_extend_time[i] = -1;
	            last_reported_extend_time[i] = -1;
	            state_changed = true;
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
				            cJSON_AddNumberToObject(ack_reported, "currentProfile", profileNum);
				            
				            // Emergency stop
				            cJSON_AddBoolToObject(ack_reported, "emergencyStop", emergencyStopActive);
				            
				            // System reset
				            cJSON_AddBoolToObject(ack_reported, "systemReset", false);
				            
				            // Report current startAllPumps state
				            cJSON_AddBoolToObject(ack_reported, "startAllPumps", startAllPumpsActive);
				            
				            xSemaphoreGive(mutexSystemState);
				        }
				        
				        // Pump states
				        const char* pumpNames[4] = {"NorthPump", "SouthPump", "EastPump", "WestPump"};
				        
				        for (int i = 0; i < 4; i++) {
				            cJSON *pump_obj = cJSON_CreateObject();
				            if (pump_obj) {
				                // FIX: Report the ACKNOWLEDGED desired state, not hardware state
				                // This ensures reported matches desired after processing
				                bool manual_mode = last_shadow_manual_mode[i];
				                cJSON_AddBoolToObject(pump_obj, "manualMode", manual_mode);
				                printf("\n[SHADOW] ACK %s: manualMode=%s (acknowledging desired state)", 
				                       pumpNames[i], manual_mode ? "true" : "false");
				                
				                // FIX: Report the ACKNOWLEDGED extendTime value
				                // Use last_reported_extend_time which tracks what we processed
				                int extend_val = last_reported_extend_time[i];
				                printf("\n[SHADOW] ACK %s: extendTime=%d (acknowledging processed value)", 
				                       pumpNames[i], extend_val);
				                
				                cJSON_AddNumberToObject(pump_obj, "extendTime", extend_val);
				                cJSON_AddBoolToObject(pump_obj, "stopPump", last_shadow_stop_pump[i]);
				                
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
                    printf("\n[SHADOW] Update accepted");
                        for (int i = 0; i < 4; i++) {
        		if (pending_extend_ack[i] >= 0) {
            			printf("\n[SHADOW] Clearing pending_extend_ack[%d] = %d\n", 
                   			i, pending_extend_ack[i]);
            			pending_extend_ack[i] = -1;
        			}
    			}

                }
                else if (strstr(topic, "/shadow/update/rejected") != NULL) {
                    printf("\n[SHADOW] Update rejected");
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
    printf("\n[SHADOW] Checking for state changes...\n");
    
    if (!mqtt_client || !mqtt_connected) {
        printf("\n[SHADOW] ERROR: MQTT not connected");
        return;
    }
    
   // ✅ DETECT CHANGES
    bool changes_detected = false;
    
    int current_profile = convert_profile_enum_to_number(currentProfile);
    if (current_profile != last_shadow_profile) {
        printf("[SHADOW] Change detected: Profile %d -> %d\n", last_shadow_profile, current_profile);
        changes_detected = true;
        last_shadow_profile = current_profile;
    }
    
    if (emergencyStopActive != last_shadow_emergency_stop) {
        printf("[SHADOW] Change detected: Emergency Stop %d -> %d\n", last_shadow_emergency_stop, emergencyStopActive);
        changes_detected = true;
        last_shadow_emergency_stop = emergencyStopActive;
    }
    
    if (startAllPumpsActive != last_shadow_start_all_pumps) {
        printf("[SHADOW] Change detected: Start All Pumps %d -> %d\n", last_shadow_start_all_pumps, startAllPumpsActive);
        changes_detected = true;
        last_shadow_start_all_pumps = startAllPumpsActive;
    }
    
    // ✅ NEW: Check for manualMode changes that need reporting
    for (int i = 0; i < 4; i++) {
        if (last_shadow_manual_mode[i] != last_reported_manual_mode[i]) {
            printf("[SHADOW] Change detected: Pump %d manualMode %d -> %d (needs reporting)\n", 
                   i, last_reported_manual_mode[i], last_shadow_manual_mode[i]);
            changes_detected = true;
            last_reported_manual_mode[i] = last_shadow_manual_mode[i];
        }
    }
    
    // Check for extendTime acknowledgements
	for (int i = 0; i < 4; i++) {
    	// Check if pending_extend_ack needs processing
    	if (pending_extend_ack[i] >= 0) {
        	printf("[SHADOW] Change detected: Pump %d has pending extendTime acknowledgement (%d)\n", 
            	   i, pending_extend_ack[i]);
        	changes_detected = true;
    		}
    
    	// Check if last_reported_extend_time differs from last_shadow_extend_time
    if (last_reported_extend_time[i] != last_shadow_extend_time[i]) {
        printf("[SHADOW] Change detected: Pump %d extendTime sync needed (%d -> %d)\n", 
               i, last_reported_extend_time[i], last_shadow_extend_time[i]);
        changes_detected = true;
        last_reported_extend_time[i] = last_shadow_extend_time[i];
    }
}
    
    // ✅ If no changes, skip update
    if (!changes_detected) {
        printf("[SHADOW] No changes detected - skipping update\n");
        return;
    }
    
    printf("[SHADOW] CHANGES DETECTED - Sending shadow update...\n");
    
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
    
    cJSON_AddNumberToObject(reported, "currentProfile", profileNum);
    cJSON_AddBoolToObject(reported, "emergencyStop", emergencyStopActive);
    cJSON_AddBoolToObject(reported, "systemReset", false);
    cJSON_AddBoolToObject(reported, "startAllPumps", startAllPumpsActive);
    
    // WiFi Config (for acknowledgement)
    if (wifi_has_custom_credentials()) {
        cJSON *wifiCredentials = cJSON_CreateObject();
        if (wifiCredentials) {
            cJSON_AddStringToObject(wifiCredentials, "ssid", get_current_wifi_ssid());
            cJSON_AddStringToObject(wifiCredentials, "password", get_current_wifi_password());
            cJSON_AddItemToObject(reported, "wifiCredentials", wifiCredentials);
        }
    }
    
   // ✅ PUMP OBJECTS - Report what user wants to see, not hardware state
const char* pumpNames[4] = {"NorthPump", "SouthPump", "EastPump", "WestPump"};

for (int i = 0; i < 4; i++) {
    cJSON *pumpObj = cJSON_CreateObject();
    if (pumpObj) {
        // ✅ MANUAL MODE - Report the last acknowledged desired state
        bool reportedManualMode = last_shadow_manual_mode[i];
        
        printf("\n[SHADOW] Reporting %s: manualMode=%s (tracking value, not hardware)",
               pumpNames[i], reportedManualMode ? "true" : "false");
        
        cJSON_AddBoolToObject(pumpObj, "manualMode", reportedManualMode);
        
        // ✅ EXTEND TIME - Report the last acknowledged value
        int reportedExtendTime = last_reported_extend_time[i];
        
        printf("\n[SHADOW] Reporting %s: extendTime=%d (acknowledged value)",
               pumpNames[i], reportedExtendTime);
        
        cJSON_AddNumberToObject(pumpObj, "extendTime", reportedExtendTime);
        
        // ✅ STOP PUMP
        cJSON_AddBoolToObject(pumpObj, "stopPump", last_shadow_stop_pump[i]);
        
        cJSON_AddItemToObject(reported, pumpNames[i], pumpObj);
    }
}
    
    // Create compact JSON string
    char *json_str = create_compact_json_string(root);
    if (json_str) {
        char shadow_update_topic[128];
        snprintf(shadow_update_topic, sizeof(shadow_update_topic),
                 "$aws/things/%s/shadow/update", thing_name);
        
        printf("\n[SHADOW] Publishing to: %s", shadow_update_topic);
        printf("\n[SHADOW] Payload: %s", json_str);
        
        int msg_id = esp_mqtt_client_publish(mqtt_client, shadow_update_topic, 
                                            json_str, 0, MQTT_QOS_LEVEL, 0);
        
        if (msg_id >= 0) {
            printf("\n[SHADOW] Shadow update sent (msg_id: %d)", msg_id);
        } else {
            printf("\n[SHADOW] Failed to send shadow update (error: %d)", msg_id);
        }
        
        free(json_str);
    }
    
    cJSON_Delete(root);
    printf("\n[SHADOW] State update complete\n");
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
    if (payload_len >= 1024) {
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
        printf("\n  • %s", shadow_update_delta);
        printf("\n  • %s", shadow_get_accepted);
        printf("\n  • %s", shadow_update_accepted);
        printf("\n  • %s", shadow_update_rejected);
        printf("\n  • %s", registration_response_topic);
       
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

// ========================================
// SYSTEM STATUS FUNCTION - OPTIMIZED
// ========================================

static void send_system_status(void) {
    // Collect data efficiently
    int profileNum = 0;
    const char* profileName = "Unknown";
    bool lockout = false;
    float ir_values[4] = {0};
    float current_values[4] = {0};      // ✅ NEW: Array for current sensor values (Amperes)
    bool current_faults[4] = {false};   // ✅ NEW: Track sensor fault status
    bool suppression_active = false;    // ✅ NEW: Track if fire suppression is active
    
    if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(100)) == pdTRUE) {
        profileNum = convert_profile_enum_to_number(currentProfile);
        if (currentProfile >= WILDLAND_STANDARD && currentProfile <= CONTINUOUS_FEED) {
            profileName = profiles[currentProfile].name;
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
        
        // ✅ NEW: Extract current sensor values and fault status
        for (int i = 0; i < 4; i++) {
            current_values[i] = currentSensors[i].currentValue;
            current_faults[i] = currentSensors[i].fault;
        }
        
        xSemaphoreGive(mutexSensorData);
    }
    
    // ✅ NEW: Get suppression active status
    suppression_active = is_suppression_active();
    
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    
    cJSON_AddStringToObject(root, "macAddress", mac_address);
    cJSON_AddStringToObject(root, "event", "periodicupdate");
    cJSON_AddStringToObject(root, "devicetype", "G");
    cJSON_AddStringToObject(root, "timestamp", get_custom_timestamp());
    
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "payload", payload);
    
    // Essential fields
    cJSON_AddStringToObject(payload, "wifiSSID", get_current_wifi_ssid());
    cJSON_AddStringToObject(payload, "password", get_current_wifi_password());
    cJSON_AddBoolToObject(payload, "waterLockout", lockout);
    cJSON_AddBoolToObject(payload, "doorOpen", doorOpen);
    cJSON_AddNumberToObject(payload, "currentProfile", profileNum);
    cJSON_AddStringToObject(payload, "profileName", profileName);
    cJSON_AddNumberToObject(payload, "waterLevel", level_s);
    cJSON_AddNumberToObject(payload, "batteryVoltage", bat_v);
    cJSON_AddNumberToObject(payload, "solarVoltage", sol_v);
    cJSON_AddBoolToObject(payload, "emergencyStopActive", emergencyStopActive);
    
    // ✅ NEW: Add suppression active status
    cJSON_AddBoolToObject(payload, "suppressionActive", suppression_active);
    
    // Compact IR sensors
    cJSON_AddNumberToObject(payload, "irNorth", ir_values[0]);
    cJSON_AddNumberToObject(payload, "irSouth", ir_values[1]);
    cJSON_AddNumberToObject(payload, "irEast", ir_values[2]);
    cJSON_AddNumberToObject(payload, "irWest", ir_values[3]);
    
    // ✅ NEW: Add current sensor values for each pump (in Amperes)
    cJSON_AddNumberToObject(payload, "currentNorth", current_values[0]);
    cJSON_AddNumberToObject(payload, "currentSouth", current_values[1]);
    cJSON_AddNumberToObject(payload, "currentEast", current_values[2]);
    cJSON_AddNumberToObject(payload, "currentWest", current_values[3]);
    
    // ✅ NEW: Add current sensor fault status (optional but recommended)
    cJSON_AddBoolToObject(payload, "currentFaultNorth", current_faults[0]);
    cJSON_AddBoolToObject(payload, "currentFaultSouth", current_faults[1]);
    cJSON_AddBoolToObject(payload, "currentFaultEast", current_faults[2]);
    cJSON_AddBoolToObject(payload, "currentFaultWest", current_faults[3]);
    
    // Pump states with descriptive names (true = running, false = not running)
    cJSON_AddBoolToObject(payload, "NorthPumpState", pumps[0].isRunning);
    cJSON_AddBoolToObject(payload, "SouthPumpState", pumps[1].isRunning);
    cJSON_AddBoolToObject(payload, "EastPumpState", pumps[2].isRunning);
    cJSON_AddBoolToObject(payload, "WestPumpState", pumps[3].isRunning);
    
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
    
    // Check startAllPumps status
    static bool last_start_all_pumps = false;
    if (startAllPumpsActive != last_start_all_pumps) {
        if (startAllPumpsActive) {
            send_alert_start_all_pumps_activated();
        } else {
            send_alert_start_all_pumps_deactivated("TIMER_EXPIRED", 90);
        }
        last_start_all_pumps = startAllPumpsActive;
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
            // Gather relevant data
            const char* activationSource = NULL;
            const char* trigger = NULL;
            const char* stopReason = NULL;
            float sensorTemp = 0.0f;
            int runtime = 0;
            int cooldown = 0;
            
            // Determine activation source and trigger
            if (current_state == PUMP_AUTO_ACTIVE) {
                trigger = "FIRE_DETECTED";
                float sensorValues[4] = {ir_s1, ir_s2, ir_s3, ir_s4};
                sensorTemp = sensorValues[i];
            } else if (current_state == PUMP_MANUAL_ACTIVE) {
                if (pumps[i].activationSource == ACTIVATION_SOURCE_SHADOW_SINGLE) {
                    activationSource = "SHADOW";
                } else {
                    activationSource = "MANUAL";
                }
            } else if (current_state == PUMP_OFF) {
                // Determine stop reason
                switch(pumps[i].lastStopReason) {
                    case STOP_REASON_MANUAL:
                        stopReason = "MANUAL_STOP";
                        break;
                    case STOP_REASON_TIMEOUT:
                        stopReason = "TIMER_EXPIRED";
                        break;
                    case STOP_REASON_EMERGENCY_STOP:
                        stopReason = "EMERGENCY_STOP";
                        break;
                    case STOP_REASON_WATER_LOCKOUT:
                        stopReason = "WATER_LOCKOUT";
                        break;
                    default:
                        stopReason = "SYSTEM";
                        break;
                }
                
                // Calculate runtime if timer was active
                if (pumps[i].timerProtected) {
                    runtime = (int)(pumps[i].protectionTimeRemaining / 1000);
                }
            } else if (current_state == PUMP_COOLDOWN) {
                // Get cooldown from profile
					cooldown = (int)(profiles[currentProfile].cooldown / 1000);
            }
            
            // Send alert with all data
            send_alert_pump_state_change(i, last_pump_states[i], current_state,
                                         activationSource, trigger, sensorTemp,
                                         stopReason, runtime, cooldown);
            
            last_pump_states[i] = current_state;
        }
    }
    
    // Check door status
    static TickType_t door_open_start_time = 0;
    if (doorOpen != last_door_state) {
        if (doorOpen) {
            door_open_start_time = xTaskGetTickCount();
            send_alert_door_status(true, 0);
        } else {
            int openDuration = (int)((xTaskGetTickCount() - door_open_start_time) * portTICK_PERIOD_MS / 1000);
            send_alert_door_status(false, openDuration);
        }
        last_door_state = doorOpen;
    }
    
    // Check water lockout
    if (waterLockout != last_water_lockout) {
        float threshold = 10.0; // Get from your system config if available
        send_alert_water_lockout(waterLockout, level_s, threshold);
        last_water_lockout = waterLockout;
    }
    
  
}

// ========================================
// UPDATED monitor_fire_sectors
// ========================================

static void monitor_fire_sectors(void) {
    if (!ALERT_SYSTEM_ENABLED) return;
    
    float sensor_values[4] = {0, 0, 0, 0};  // Initialize to zero
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
        // Failed to get mutex, skip this cycle
        printf("[FIRE] Warning: Could not get sensor mutex\n");
        return;
    }
    
    // Update fire detection info (this calculates fire type)
    update_fire_detection_info();
    FireDetectionInfo* fireInfo = get_fire_detection_info();
    
    // Check for fires
    for (int i = 0; i < 4; i++) {
        fire_detected[i] = (sensor_values[i] > FIRE_THRESHOLD);
        if (fire_detected[i]) {
            current_fire_count++;
        }
    }
    // Log fire type changes
    static FireDetectionType last_fire_type = FIRE_TYPE_NONE;
    if (fireInfo->type != last_fire_type) {
        printf("[FIRE] Detection type changed: %s -> %s (sectors: %s)\n",
               get_fire_detection_type_string(last_fire_type),
               get_fire_detection_type_string(fireInfo->type),
               fireInfo->activeSectorNames[0] ? fireInfo->activeSectorNames : "none");
        last_fire_type = fireInfo->type;
    }
    
    // Check individual sector fire alerts
    for (int i = 0; i < 4; i++) {
        if (fire_detected[i] && !fire_alerts_active[i]) {
            // New fire detected in this sector
            fire_sector_t sector = get_sector_from_index(i);
            const char* sectorName = get_sector_name_string(sector);
            bool pumpActivated = (pumps[i].state == PUMP_AUTO_ACTIVE);
            
            send_alert_fire_detected(i, sectorName, sensor_values[i], pumpActivated);
            fire_alerts_active[i] = true;
        }
        else if (!fire_detected[i] && fire_alerts_active[i]) {
            // Fire cleared in this sector
            fire_sector_t sector = get_sector_from_index(i);
            const char* sectorName = get_sector_name_string(sector);
            
            
            send_alert_fire_cleared(i, sectorName, sensor_values[i]);
            fire_alerts_active[i] = false;
        }
    }
    
    // Check for multiple fires
    static int last_fire_count = 0;
    if (current_fire_count > 1 && current_fire_count != last_fire_count) {
        bool pumpStates[4] = {
            pumps[0].state == PUMP_AUTO_ACTIVE,
            pumps[1].state == PUMP_AUTO_ACTIVE,
            pumps[2].state == PUMP_AUTO_ACTIVE,
            pumps[3].state == PUMP_AUTO_ACTIVE
        };
        
        send_alert_multiple_fires(current_fire_count, sensor_values, pumpStates);
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
    static TickType_t manual_start_time = 0;
    bool current_manual_override = false;
    
    // Check if any pump is in manual mode
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
            current_manual_override = true;
            break;
        }
    }
    
    if (current_manual_override && !manual_override_active) {
        manual_start_time = xTaskGetTickCount();
        send_alert_manual_override(true, 0);
        manual_override_active = true;
    }
    else if (!current_manual_override && manual_override_active) {
        int manualDuration = (int)((xTaskGetTickCount() - manual_start_time) * portTICK_PERIOD_MS / 1000);
        send_alert_manual_override(false, manualDuration);
        manual_override_active = false;
    }
    
    // Check for auto activations
    static bool auto_activation_reported = false;
    bool auto_active = false;
    
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_AUTO_ACTIVE) {
            auto_active = true;
            break;
        }
    }
    
    if (auto_active && !auto_activation_reported) {
        send_alert_auto_activation();
        auto_activation_reported = true;
    }
    else if (!auto_active && auto_activation_reported) {
        auto_activation_reported = false;
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
        cJSON_AddStringToObject(payload, "alertType", get_alert_type_string(alert.type));
        cJSON_AddStringToObject(payload, "severity", get_severity_string(alert.severity));
        cJSON_AddStringToObject(payload, "message", alert.message);
        
        // Add type-specific payload fields
        switch(alert.type) {
            
            // ==========================================
            // ALERT #1: PROFILE CHANGE
            // ==========================================
            case ALERT_TYPE_PROFILE_CHANGE:
                cJSON_AddNumberToObject(payload, "previousProfile", alert.data.profile.previousProfile);
                cJSON_AddNumberToObject(payload, "currentProfile", alert.data.profile.currentProfile);
                cJSON_AddStringToObject(payload, "profileName", alert.data.profile.profileName);
                break;
            
            // ==========================================
            // ALERT #2-3: EMERGENCY STOP
            // ==========================================
            case ALERT_TYPE_EMERGENCY_STOP:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.emergencyStop.activated ? "ACTIVATED" : "DEACTIVATED");
                
                if (alert.data.emergencyStop.activated) {
                    cJSON_AddBoolToObject(payload, "allPumpsStopped", true);
                    
                    // Add affected pumps array
                    cJSON *affectedPumps = cJSON_CreateArray();
                    for (int i = 0; i < alert.data.emergencyStop.affectedPumpCount; i++) {
                        cJSON *pump = cJSON_CreateObject();
                        cJSON_AddNumberToObject(pump, "pumpId", 
                            alert.data.emergencyStop.affectedPumps[i].pumpId);
                        cJSON_AddStringToObject(pump, "pumpName", 
                            alert.data.emergencyStop.affectedPumps[i].pumpName);
                        cJSON_AddStringToObject(pump, "previousState", 
                            get_pump_state_string_for_alert(
                                alert.data.emergencyStop.affectedPumps[i].previousState));
                        cJSON_AddItemToArray(affectedPumps, pump);
                    }
                    cJSON_AddItemToObject(payload, "affectedPumps", affectedPumps);
                } else {
                    cJSON_AddStringToObject(payload, "systemStatus", "OPERATIONAL");
                }
                break;
            
            // ==========================================
            // ALERT #3: SYSTEM RESET
            // ==========================================
            case ALERT_TYPE_SYSTEM_RESET:
                cJSON_AddStringToObject(payload, "resetType", alert.data.systemReset.resetType);
                cJSON_AddStringToObject(payload, "defaultProfile", alert.data.systemReset.defaultProfile);
                cJSON_AddBoolToObject(payload, "allPumpsReset", alert.data.systemReset.allPumpsReset);
                cJSON_AddBoolToObject(payload, "emergencyStopCleared", 
                    alert.data.systemReset.emergencyStopCleared);
                break;
            
            // ==========================================
            // ALERT #4-5: START ALL PUMPS
            // ==========================================
            case ALERT_TYPE_START_ALL_PUMPS:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.startAllPumps.activated ? "ACTIVATED" : "DEACTIVATED");
                
                if (alert.data.startAllPumps.activated) {
                    cJSON_AddNumberToObject(payload, "duration", alert.data.startAllPumps.duration);
                    
                    // Add activated pumps array
                    cJSON *activatedPumps = cJSON_CreateArray();
                    for (int i = 0; i < 4; i++) {
                        cJSON *pump = cJSON_CreateObject();
                        cJSON_AddNumberToObject(pump, "pumpId", i + 1);
                        
                        // Get pump name
                        char pumpName[16];
                        switch(i) {
                            case 0: strcpy(pumpName, "North"); break;
                            case 1: strcpy(pumpName, "South"); break;
                            case 2: strcpy(pumpName, "East"); break;
                            case 3: strcpy(pumpName, "West"); break;
                        }
                        cJSON_AddStringToObject(pump, "pumpName", pumpName);
                        cJSON_AddItemToArray(activatedPumps, pump);
                    }
                    cJSON_AddItemToObject(payload, "activatedPumps", activatedPumps);
                    cJSON_AddBoolToObject(payload, "waterLockout", alert.data.startAllPumps.waterLockout);
                } else {
                    cJSON_AddStringToObject(payload, "reason", alert.data.startAllPumps.reason);
                    cJSON_AddNumberToObject(payload, "totalRuntime", alert.data.startAllPumps.totalRuntime);
                }
                break;
            
            // ==========================================
            // ALERT #6-9: PUMP STATE CHANGE
            // ==========================================
            case ALERT_TYPE_PUMP_STATE_CHANGE:
                cJSON_AddNumberToObject(payload, "pumpId", alert.data.pump.pumpId);
                cJSON_AddStringToObject(payload, "pumpName", alert.data.pump.pumpName);
                cJSON_AddStringToObject(payload, "previousState", 
                    get_pump_state_string_for_alert(alert.data.pump.previousState));
                cJSON_AddStringToObject(payload, "currentState", 
                    get_pump_state_string_for_alert(alert.data.pump.currentState));
                
                // Add state-specific fields
                if (alert.data.pump.currentState == 1) { // AUTO_ACTIVE
                    cJSON_AddStringToObject(payload, "activationMode", "AUTOMATIC");
                    if (strlen(alert.data.pump.trigger) > 0) {
                        cJSON_AddStringToObject(payload, "trigger", alert.data.pump.trigger);
                    }
                    if (alert.data.pump.sensorTemperature > 0) {
                        cJSON_AddNumberToObject(payload, "sensorTemperature", alert.data.pump.sensorTemperature);
                    }
                } else if (alert.data.pump.currentState == 2) { // MANUAL_ACTIVE
                    cJSON_AddStringToObject(payload, "activationMode", "MANUAL");
                    if (strlen(alert.data.pump.activationSource) > 0) {
                        cJSON_AddStringToObject(payload, "activationSource", alert.data.pump.activationSource);
                    }
                } else if (alert.data.pump.currentState == 0) { // OFF
                    if (strlen(alert.data.pump.stopReason) > 0) {
                        cJSON_AddStringToObject(payload, "stopReason", alert.data.pump.stopReason);
                    }
                    if (alert.data.pump.totalRuntime > 0) {
                        cJSON_AddNumberToObject(payload, "totalRuntime", alert.data.pump.totalRuntime);
                    }
                } else if (alert.data.pump.currentState == 3) { // COOLDOWN
                    cJSON_AddNumberToObject(payload, "cooldownDuration", alert.data.pump.cooldownDuration);
                    if (alert.data.pump.previousRuntime > 0) {
                        cJSON_AddNumberToObject(payload, "previousRuntime", alert.data.pump.previousRuntime);
                    }
                }
                break;
            
            // ==========================================
            // ALERT #10: PUMP TIMER EXTENSION
            // ==========================================
            case ALERT_TYPE_PUMP_EXTEND_TIME:
                cJSON_AddNumberToObject(payload, "pumpId", alert.data.pumpExtend.pumpId);
                cJSON_AddStringToObject(payload, "pumpName", alert.data.pumpExtend.pumpName);
                cJSON_AddNumberToObject(payload, "extensionCode", alert.data.pumpExtend.extensionCode);
                cJSON_AddNumberToObject(payload, "extensionDuration", alert.data.pumpExtend.extensionDuration);
                cJSON_AddNumberToObject(payload, "newTotalRuntime", alert.data.pumpExtend.newTotalRuntime);
                break;
            
            // ==========================================
            // ALERT #11: FIRE DETECTED
            // ==========================================
            case ALERT_TYPE_FIRE_DETECTED:
                cJSON_AddStringToObject(payload, "sector", alert.data.fire.sector);
                cJSON_AddNumberToObject(payload, "sensorId", alert.data.fire.sensorId);
                cJSON_AddNumberToObject(payload, "temperature", alert.data.fire.temperature);
                cJSON_AddNumberToObject(payload, "threshold", alert.data.fire.threshold);
                cJSON_AddBoolToObject(payload, "pumpActivated", alert.data.fire.pumpActivated);
                if (alert.data.fire.pumpActivated) {
                    cJSON_AddNumberToObject(payload, "pumpId", alert.data.fire.pumpId);
                    cJSON_AddStringToObject(payload, "pumpName", alert.data.fire.pumpName);
                }
                break;
            
            // ==========================================
            // ALERT #12: FIRE CLEARED
            // ==========================================
            case ALERT_TYPE_FIRE_CLEARED:
                cJSON_AddStringToObject(payload, "sector", alert.data.fire.sector);
                cJSON_AddNumberToObject(payload, "sensorId", alert.data.fire.sensorId);
                cJSON_AddNumberToObject(payload, "currentTemperature", alert.data.fire.currentTemperature);
                if (alert.data.fire.duration > 0) {
                    cJSON_AddNumberToObject(payload, "duration", alert.data.fire.duration);
                }
                break;
            
            // ==========================================
            // ALERT #13: MULTIPLE FIRES
            // ==========================================
            case ALERT_TYPE_MULTIPLE_FIRES:
                cJSON_AddNumberToObject(payload, "activeFireCount", 
                    alert.data.multipleFires.activeFireCount);
                
                // Add affected sectors array
                cJSON *affectedSectors = cJSON_CreateArray();
                for (int i = 0; i < alert.data.multipleFires.activeFireCount && i < 4; i++) {
                    cJSON *sector = cJSON_CreateObject();
                    cJSON_AddStringToObject(sector, "sector", 
                        alert.data.multipleFires.affectedSectors[i].sector);
                    cJSON_AddNumberToObject(sector, "temperature", 
                        alert.data.multipleFires.affectedSectors[i].temperature);
                    cJSON_AddBoolToObject(sector, "pumpActive", 
                        alert.data.multipleFires.affectedSectors[i].pumpActive);
                    cJSON_AddItemToArray(affectedSectors, sector);
                }
                cJSON_AddItemToObject(payload, "affectedSectors", affectedSectors);
                
                cJSON_AddNumberToObject(payload, "waterLevel", alert.data.multipleFires.waterLevel);
                cJSON_AddNumberToObject(payload, "estimatedRuntime", 
                    alert.data.multipleFires.estimatedRuntime);
                break;
            
            // ==========================================
            // ALERT #14-15: WATER LOCKOUT
            // ==========================================
            case ALERT_TYPE_WATER_LOCKOUT:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.waterLockout.activated ? "ACTIVATED" : "DEACTIVATED");
                cJSON_AddNumberToObject(payload, "currentWaterLevel", 
                    alert.data.waterLockout.currentWaterLevel);
                
                if (alert.data.waterLockout.activated) {
                    cJSON_AddNumberToObject(payload, "minThreshold", alert.data.waterLockout.minThreshold);
                    cJSON_AddBoolToObject(payload, "allPumpsDisabled", 
                        alert.data.waterLockout.allPumpsDisabled);
                    cJSON_AddBoolToObject(payload, "continuousFeedActive", 
                        alert.data.waterLockout.continuousFeedActive);
                } else {
                    cJSON_AddStringToObject(payload, "systemStatus", alert.data.waterLockout.systemStatus);
                }
                break;
            
            // ==========================================
            // ALERT #16-17:DOOR STATUS
            // ==========================================
            case ALERT_TYPE_DOOR_STATUS:
                cJSON_AddStringToObject(payload, "action", alert.data.door.action);
                cJSON_AddBoolToObject(payload, "doorState", alert.data.door.doorState);
                
                if (alert.data.door.opened) {
                    cJSON_AddBoolToObject(payload, "securityConcern", alert.data.door.securityConcern);
                } else {
                    if (alert.data.door.wasOpenDuration > 0) {
                        cJSON_AddNumberToObject(payload, "wasOpenDuration", 
                            alert.data.door.wasOpenDuration);
                    }
                }
                break;
            
            // ==========================================
            // ALERT #18-19: MANUAL OVERRIDE
            // ==========================================
            case ALERT_TYPE_MANUAL_OVERRIDE:
                cJSON_AddStringToObject(payload, "action", alert.data.manualOverride.action);
                
                if (alert.data.manualOverride.activated) {
                    // Add manual pumps array
                    cJSON *manualPumps = cJSON_CreateArray();
                    for (int i = 0; i < alert.data.manualOverride.manualPumpCount; i++) {
                        cJSON *pump = cJSON_CreateObject();
                        cJSON_AddNumberToObject(pump, "pumpId", 
                            alert.data.manualOverride.manualPumps[i].pumpId);
                        cJSON_AddStringToObject(pump, "pumpName", 
                            alert.data.manualOverride.manualPumps[i].pumpName);
                        cJSON_AddStringToObject(pump, "state", 
                            alert.data.manualOverride.manualPumps[i].state);
                        cJSON_AddItemToArray(manualPumps, pump);
                    }
                    cJSON_AddItemToObject(payload, "manualPumps", manualPumps);
                    
                    cJSON_AddBoolToObject(payload, "autoProtectionDisabled", 
                        alert.data.manualOverride.autoProtectionDisabled);
                    if (strlen(alert.data.manualOverride.activationSource) > 0) {
                        cJSON_AddStringToObject(payload, "activationSource", 
                            alert.data.manualOverride.activationSource);
                    }
                } else {
                    cJSON_AddStringToObject(payload, "systemMode", alert.data.manualOverride.systemMode);
                    cJSON_AddBoolToObject(payload, "autoProtectionEnabled", 
                        alert.data.manualOverride.autoProtectionEnabled);
                    if (alert.data.manualOverride.totalManualDuration > 0) {
                        cJSON_AddNumberToObject(payload, "totalManualDuration", 
                            alert.data.manualOverride.totalManualDuration);
                    }
                }
                break;
            
            // ==========================================
            // ALERT #20: AUTO ACTIVATION
            // ==========================================
            case ALERT_TYPE_AUTO_ACTIVATION:
                cJSON_AddStringToObject(payload, "trigger", alert.data.autoActivation.trigger);
                
                // Add activated pumps array
                cJSON *autoActivatedPumps = cJSON_CreateArray();
                for (int i = 0; i < alert.data.autoActivation.activatedPumpCount; i++) {
                    cJSON *pump = cJSON_CreateObject();
                    cJSON_AddNumberToObject(pump, "pumpId", 
                        alert.data.autoActivation.activatedPumps[i].pumpId);
                    cJSON_AddStringToObject(pump, "pumpName", 
                        alert.data.autoActivation.activatedPumps[i].pumpName);
                    cJSON_AddStringToObject(pump, "sector", 
                        alert.data.autoActivation.activatedPumps[i].sector);
                    cJSON_AddNumberToObject(pump, "temperature", 
                        alert.data.autoActivation.activatedPumps[i].temperature);
                    cJSON_AddStringToObject(pump, "state", 
                        alert.data.autoActivation.activatedPumps[i].state);
                    cJSON_AddItemToArray(autoActivatedPumps, pump);
                }
                cJSON_AddItemToObject(payload, "activatedPumps", autoActivatedPumps);
                
                cJSON_AddStringToObject(payload, "currentProfile", 
                    alert.data.autoActivation.currentProfile);
                cJSON_AddNumberToObject(payload, "waterLevel", alert.data.autoActivation.waterLevel);
                cJSON_AddNumberToObject(payload, "estimatedRuntime", 
                    alert.data.autoActivation.estimatedRuntime);
                break;
           
            
            // ==========================================
            // ALERT #21-22: WIFI UPDATE
            // ==========================================
            case ALERT_TYPE_WIFI_UPDATE:
                cJSON_AddStringToObject(payload, "action", alert.data.wifi.action);
                
                if (strcmp(alert.data.wifi.action, "CREDENTIALS_UPDATED") == 0) {
                    cJSON_AddStringToObject(payload, "newSSID", alert.data.wifi.newSSID);
                    if (strlen(alert.data.wifi.previousSSID) > 0) {
                        cJSON_AddStringToObject(payload, "previousSSID", alert.data.wifi.previousSSID);
                    }
                    cJSON_AddBoolToObject(payload, "requiresReboot", alert.data.wifi.requiresReboot);
                    cJSON_AddBoolToObject(payload, "stored", alert.data.wifi.stored);
                } else {
                    // Invalid credentials
                    cJSON_AddStringToObject(payload, "errorType", alert.data.wifi.errorType);
                    cJSON_AddStringToObject(payload, "errorCode", alert.data.wifi.errorCode);
                    
                    cJSON *details = cJSON_CreateObject();
                    cJSON_AddNumberToObject(details, "ssidLength", alert.data.wifi.ssidLength);
                    cJSON_AddNumberToObject(details, "passwordLength", alert.data.wifi.passwordLength);
                    cJSON_AddStringToObject(details, "reason", alert.data.wifi.reason);
                    cJSON_AddItemToObject(payload, "details", details);
                }
                break;
            
            // ==========================================
            // ALERT #23: SENSOR FAULT
            // ==========================================
            case ALERT_TYPE_SENSOR_FAULT:
                cJSON_AddStringToObject(payload, "sensorType", alert.data.sensorFault.sensorType);
                cJSON_AddNumberToObject(payload, "sensorId", alert.data.sensorFault.sensorId);
                cJSON_AddStringToObject(payload, "sectorAffected", alert.data.sensorFault.sectorAffected);
                cJSON_AddStringToObject(payload, "errorCode", alert.data.sensorFault.errorCode);
                cJSON_AddNumberToObject(payload, "lastValidReading", 
                    alert.data.sensorFault.lastValidReading);
                break;
            
          
            // ==========================================
            // ALERT #24: SYSTEM ERROR (GENERIC)
            // ==========================================
            case ALERT_TYPE_SYSTEM_ERROR:
                cJSON_AddStringToObject(payload, "errorType", alert.data.systemError.errorType);
                cJSON_AddStringToObject(payload, "errorCode", alert.data.systemError.errorCode);
                if (strlen(alert.data.systemError.details) > 0) {
                    cJSON_AddStringToObject(payload, "details", alert.data.systemError.details);
                }
                break;
            
            // ==========================================
            // ALERT #25: CONTINUOUS FEED
            // ==========================================
            case ALERT_TYPE_CONTINUOUS_FEED:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.continuousFeed.activated ? "ACTIVATED" : "DEACTIVATED");
                cJSON_AddStringToObject(payload, "profile", alert.data.continuousFeed.profile);
                cJSON_AddBoolToObject(payload, "waterLockoutDisabled", 
                    alert.data.continuousFeed.waterLockoutDisabled);
                cJSON_AddBoolToObject(payload, "unlimitedWaterSupply", 
                    alert.data.continuousFeed.unlimitedWaterSupply);
                break;
            
            // ==========================================
            // ALERT #26: HARDWARE FAULT ALERTS
            // ==========================================
            case ALERT_TYPE_PCA9555_FAIL:
                cJSON_AddStringToObject(payload, "hardwareType", 
                    alert.data.hardwareFault.hardwareType);
                cJSON_AddNumberToObject(payload, "componentId", 
                    alert.data.hardwareFault.componentId);
                cJSON_AddStringToObject(payload, "errorCode", 
                    alert.data.hardwareFault.errorCode);
                cJSON_AddStringToObject(payload, "errorMessage", 
                    alert.data.hardwareFault.errorMessage);
                cJSON_AddBoolToObject(payload, "systemCritical", 
                    alert.data.hardwareFault.systemCritical);
                cJSON_AddNumberToObject(payload, "affectedPumpCount", 
                    alert.data.hardwareFault.affectedPumpCount);
                cJSON_AddStringToObject(payload, "affectedPumps", 
                    alert.data.hardwareFault.affectedPumps);
                break;
            
            case ALERT_TYPE_HARDWARE_CONTROL_FAIL:
            case ALERT_TYPE_ADC_INIT_FAIL:
            case ALERT_TYPE_CURRENT_SENSOR_FAULT:
            case ALERT_TYPE_IR_SENSOR_FAULT:
                // Same structure as PCA9555_FAIL
                cJSON_AddStringToObject(payload, "hardwareType", 
                    alert.data.hardwareFault.hardwareType);
                cJSON_AddNumberToObject(payload, "componentId", 
                    alert.data.hardwareFault.componentId);
                cJSON_AddStringToObject(payload, "errorCode", 
                    alert.data.hardwareFault.errorCode);
                cJSON_AddStringToObject(payload, "errorMessage", 
                    alert.data.hardwareFault.errorMessage);
                cJSON_AddBoolToObject(payload, "systemCritical", 
                    alert.data.hardwareFault.systemCritical);
                if (alert.data.hardwareFault.affectedPumpCount > 0) {
                    cJSON_AddNumberToObject(payload, "affectedPumpCount", 
                        alert.data.hardwareFault.affectedPumpCount);
                    cJSON_AddStringToObject(payload, "affectedPumps", 
                        alert.data.hardwareFault.affectedPumps);
                }
                break;
            
            // ==========================================
            // ALERT #27: POWER ALERTS
            // ==========================================
            case ALERT_TYPE_BATTERY_CRITICAL:
            case ALERT_TYPE_BATTERY_LOW:
            case ALERT_TYPE_SOLAR_FAULT:
                cJSON_AddNumberToObject(payload, "batteryVoltage", 
                    alert.data.powerStatus.batteryVoltage);
                cJSON_AddNumberToObject(payload, "solarVoltage", 
                    alert.data.powerStatus.solarVoltage);
                cJSON_AddNumberToObject(payload, "threshold", 
                    alert.data.powerStatus.threshold);
                cJSON_AddStringToObject(payload, "powerState", 
                    alert.data.powerStatus.powerState);
                if (alert.data.powerStatus.estimatedRuntime > 0) {
                    cJSON_AddNumberToObject(payload, "estimatedRuntime", 
                        alert.data.powerStatus.estimatedRuntime);
                }
                cJSON_AddBoolToObject(payload, "chargingActive", 
                    alert.data.powerStatus.chargingActive);
                break;
            
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
                break;
            
            default:
                printf("\n[ALERT] Unknown alert type: %d", alert.type);
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
        if (alert->severity < ALERT_SEVERITY_CRITICAL) {
            printf("\n[ALERT] Blocked alert - Sensors warming up (%u/%d sec)",
       (unsigned int)seconds_since_boot, SENSOR_WARMUP_SECONDS);
            return false;
        }
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
    alert.type = ALERT_TYPE_EMERGENCY_STOP;
    alert.severity = ALERT_SEVERITY_CRITICAL;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    strcpy(alert.message, "EMERGENCY STOP ACTIVATED - All pumps stopped immediately");
    
    alert.data.emergencyStop.activated = true;
    alert.data.emergencyStop.affectedPumpCount = 0;
    
    // Capture pump states before emergency stop
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state != PUMP_OFF) {
            int idx = alert.data.emergencyStop.affectedPumpCount++;
            alert.data.emergencyStop.affectedPumps[idx].pumpId = i + 1;
            strncpy(alert.data.emergencyStop.affectedPumps[idx].pumpName, 
                   pumps[i].name, 15);
            alert.data.emergencyStop.affectedPumps[idx].previousState = pumps[i].state;
        }
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #3: EMERGENCY STOP DEACTIVATED
// ==========================================
static void send_alert_emergency_stop_deactivated(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_EMERGENCY_STOP;
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
    
    strcpy(alert.message, "START ALL PUMPS ACTIVATED - All 4 pumps activated for 90 seconds");
    
    alert.data.startAllPumps.activated = true;
    alert.data.startAllPumps.duration = 90;
    alert.data.startAllPumps.activatedPumpCount = 4;
    alert.data.startAllPumps.waterLockout = waterLockout;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #6: START ALL PUMPS DEACTIVATED
// ==========================================
static void send_alert_start_all_pumps_deactivated(const char* reason, int totalRuntime) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_START_ALL_PUMPS;
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
// ALERT #7-10: PUMP STATE CHANGE
// ==========================================
static void send_alert_pump_state_change(int pumpIndex, int previousState, int currentState,
                                        const char* activationSource, const char* trigger,
                                        float sensorTemp, const char* stopReason,
                                        int runtime, int cooldownDuration) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_PUMP_STATE_CHANGE;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    // Determine severity based on state
    if (currentState == 1) { // AUTO_ACTIVE
        alert.severity = ALERT_SEVERITY_CRITICAL;
    } else if (currentState == 2) { // MANUAL_ACTIVE
        alert.severity = ALERT_SEVERITY_WARNING;
    } else {
        alert.severity = ALERT_SEVERITY_INFO;
    }
    
    // Create message
    const char* stateStr = get_pump_state_string_for_alert(currentState);
    snprintf(alert.message, sizeof(alert.message),
            "Pump %d (%s) changed to %s", pumpIndex + 1, pumps[pumpIndex].name, stateStr);
    
    // Fill data
    alert.data.pump.pumpId = pumpIndex + 1;
    strncpy(alert.data.pump.pumpName, pumps[pumpIndex].name, sizeof(alert.data.pump.pumpName) - 1);
    alert.data.pump.previousState = previousState;
    alert.data.pump.currentState = currentState;
    
    if (activationSource) {
        strncpy(alert.data.pump.activationSource, activationSource, 
               sizeof(alert.data.pump.activationSource) - 1);
    }
    if (trigger) {
        strncpy(alert.data.pump.trigger, trigger, sizeof(alert.data.pump.trigger) - 1);
    }
    if (stopReason) {
        strncpy(alert.data.pump.stopReason, stopReason, sizeof(alert.data.pump.stopReason) - 1);
    }
    
    alert.data.pump.sensorTemperature = sensorTemp;
    alert.data.pump.totalRuntime = runtime;
    alert.data.pump.cooldownDuration = cooldownDuration;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #11: PUMP TIMER EXTENSION
// ==========================================
static void send_alert_pump_extend_time(int pumpIndex, int extensionCode, 
                                       int extensionDuration, int newTotalRuntime) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_PUMP_EXTEND_TIME;
    alert.severity = ALERT_SEVERITY_INFO;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Extended %s by %d seconds", pumps[pumpIndex].name, extensionDuration);
    
    alert.data.pumpExtend.pumpId = pumpIndex + 1;
    strncpy(alert.data.pumpExtend.pumpName, pumps[pumpIndex].name, 
           sizeof(alert.data.pumpExtend.pumpName) - 1);
    alert.data.pumpExtend.extensionCode = extensionCode;
    alert.data.pumpExtend.extensionDuration = extensionDuration;
    alert.data.pumpExtend.newTotalRuntime = newTotalRuntime;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #12: FIRE DETECTED
// ==========================================
static void send_alert_fire_detected(int sensorIndex, const char* sectorName, 
                                    float temperature, bool pumpActivated) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_FIRE_DETECTED;
    alert.severity = ALERT_SEVERITY_EMERGENCY;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    // Get fire detection type info
    update_fire_detection_info();
    FireDetectionInfo* fireInfo = get_fire_detection_info();
    
    snprintf(alert.message, sizeof(alert.message),
            "FIRE DETECTED in %s sector | Temp: %.1f°C | Type: %s", 
            sectorName, temperature, get_fire_detection_type_string(fireInfo->type));
    
    strncpy(alert.data.fire.sector, sectorName, sizeof(alert.data.fire.sector) - 1);
    alert.data.fire.sensorId = sensorIndex + 1;
    alert.data.fire.temperature = temperature;
    alert.data.fire.threshold = FIRE_THRESHOLD;
    alert.data.fire.pumpActivated = pumpActivated;
    
    // Add fire type info
    alert.data.fire.fireType = (int)fireInfo->type;
    strncpy(alert.data.fire.fireTypeString, get_fire_detection_type_string(fireInfo->type),
            sizeof(alert.data.fire.fireTypeString) - 1);
    alert.data.fire.totalActiveSectors = fireInfo->activeSectorCount;
    strncpy(alert.data.fire.allActiveSectors, fireInfo->activeSectorNames,
            sizeof(alert.data.fire.allActiveSectors) - 1);
    
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
    
    // Determine fire type based on count
    FireDetectionType fireType = (fireCount == 4) ? FIRE_TYPE_FULL_SYSTEM : FIRE_TYPE_MULTIPLE_SECTORS;
    const char* fireTypeStr = (fireCount == 4) ? "FULL_SYSTEM" : "MULTIPLE_SECTORS";
    
    snprintf(alert.message, sizeof(alert.message),
            "MULTIPLE FIRES DETECTED! %d active fire sectors | Type: %s", 
            fireCount, fireTypeStr);
    
    alert.data.multipleFires.activeFireCount = fireCount;
    
    // Add fire type info
    alert.data.multipleFires.fireType = (int)fireType;
    strncpy(alert.data.multipleFires.fireTypeString, fireTypeStr,
            sizeof(alert.data.multipleFires.fireTypeString) - 1);
    
    const char* sectorNames[4] = {"NORTH", "SOUTH", "EAST", "WEST"};
    int sectorIdx = 0;
    for (int i = 0; i < 4 && sectorIdx < fireCount; i++) {
        if (sensorValues[i] > FIRE_THRESHOLD) {
            strncpy(alert.data.multipleFires.affectedSectors[sectorIdx].sector,
                   sectorNames[i], 15);
            alert.data.multipleFires.affectedSectors[sectorIdx].temperature = sensorValues[i];
            alert.data.multipleFires.affectedSectors[sectorIdx].pumpActive = pumpStates[i];
            sectorIdx++;
        }
    }
    
    alert.data.multipleFires.waterLevel = level_s;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #15-16: WATER LOCKOUT
// ==========================================
static void send_alert_water_lockout(bool activated, float currentLevel, float threshold) {
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
        alert.data.waterLockout.minThreshold = threshold;
        alert.data.waterLockout.allPumpsDisabled = true;
        alert.data.waterLockout.continuousFeedActive = continuousWaterFeed;
    } else {
        strcpy(alert.data.waterLockout.systemStatus, "OPERATIONAL");
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #17-18: DOOR STATUS
// ==========================================
static void send_alert_door_status(bool opened, int openDuration) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_DOOR_STATUS;
    alert.severity = opened ? ALERT_SEVERITY_WARNING : ALERT_SEVERITY_INFO;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    if (opened) {
        strcpy(alert.message, "Door OPENED");
        strcpy(alert.data.door.action, "OPENED");
        alert.data.door.opened = true;
        alert.data.door.doorState = true;
        alert.data.door.securityConcern = true;
    } else {
        snprintf(alert.message, sizeof(alert.message),
                "Door CLOSED - Was open for %d seconds", openDuration);
        strcpy(alert.data.door.action, "CLOSED");
        alert.data.door.opened = false;
        alert.data.door.doorState = false;
        alert.data.door.wasOpenDuration = openDuration;
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #19-20: MANUAL OVERRIDE
// ==========================================
static void send_alert_manual_override(bool activated, int manualDuration) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_MANUAL_OVERRIDE;
    alert.severity = activated ? ALERT_SEVERITY_WARNING : ALERT_SEVERITY_INFO;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    if (activated) {
        strcpy(alert.message, "MANUAL OVERRIDE ACTIVATED - System under manual control");
        strcpy(alert.data.manualOverride.action, "ACTIVATED");
        alert.data.manualOverride.activated = true;
        alert.data.manualOverride.autoProtectionDisabled = true;
        strcpy(alert.data.manualOverride.activationSource, "USER");
        
        // Count manual pumps
        alert.data.manualOverride.manualPumpCount = 0;
        for (int i = 0; i < 4; i++) {
            if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
                int idx = alert.data.manualOverride.manualPumpCount++;
                alert.data.manualOverride.manualPumps[idx].pumpId = i + 1;
                strncpy(alert.data.manualOverride.manualPumps[idx].pumpName,
                       pumps[i].name, 15);
                strcpy(alert.data.manualOverride.manualPumps[idx].state, "MANUAL_ACTIVE");
            }
        }
    } else {
        strcpy(alert.message, "Manual override DEACTIVATED - System returning to auto mode");
        strcpy(alert.data.manualOverride.action, "DEACTIVATED");
        alert.data.manualOverride.activated = false;
        strcpy(alert.data.manualOverride.systemMode, "AUTOMATIC");
        alert.data.manualOverride.autoProtectionEnabled = true;
        alert.data.manualOverride.totalManualDuration = manualDuration;
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #21: AUTO ACTIVATION
// ==========================================
static void send_alert_auto_activation(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_AUTO_ACTIVATION;
    alert.severity = ALERT_SEVERITY_CRITICAL;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    strcpy(alert.message, "AUTO ACTIVATION - Fire suppression system automatically activated");
    strcpy(alert.data.autoActivation.trigger, "FIRE_DETECTED");
    
    // Count auto-activated pumps
    alert.data.autoActivation.activatedPumpCount = 0;
    const char* sectorNames[4] = {"NORTH", "SOUTH", "EAST", "WEST"};
    float sensorValues[4] = {ir_s1, ir_s2, ir_s3, ir_s4};
    
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_AUTO_ACTIVE) {
            int idx = alert.data.autoActivation.activatedPumpCount++;
            alert.data.autoActivation.activatedPumps[idx].pumpId = i + 1;
            strncpy(alert.data.autoActivation.activatedPumps[idx].pumpName, pumps[i].name, 15);
            strncpy(alert.data.autoActivation.activatedPumps[idx].sector, sectorNames[i], 15);
            alert.data.autoActivation.activatedPumps[idx].temperature = sensorValues[i];
            strcpy(alert.data.autoActivation.activatedPumps[idx].state, "AUTO_ACTIVE");
        }
    }
    
    strncpy(alert.data.autoActivation.currentProfile, profiles[currentProfile].name, 63);
    alert.data.autoActivation.waterLevel = level_s;
    
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


void send_alert_battery_critical(float batteryVoltage, int estimatedRuntime) {
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

void task_pump_management(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    // Track previous states to detect changes
    static PumpState prev_states[4] = {PUMP_OFF, PUMP_OFF, PUMP_OFF, PUMP_OFF};
    static bool prev_manual_mode[4] = {false, false, false, false};
    
    for (;;) {
        if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(10)) == pdTRUE) {
            update_pump_states();
            
            // Detect pump state changes
            bool shadow_update_needed = false;
            
            for (int i = 0; i < 4; i++) {
                // Calculate current manualMode state
                bool current_manual_mode = false;
                if (pumps[i].state == PUMP_MANUAL_ACTIVE && !startAllPumpsActive) {
                    if (pumps[i].activationSource == ACTIVATION_SOURCE_SHADOW_SINGLE ||
                        pumps[i].activationSource == ACTIVATION_SOURCE_MANUAL_SINGLE) {
                        current_manual_mode = true;
                    }
                }
                
                // Detect manual mode changes (especially true -> false when timer expires)
                if (current_manual_mode != prev_manual_mode[i]) {
                    printf("\n[PUMP] Pump %d manualMode changed: %s -> %s\n", 
                           i, prev_manual_mode[i] ? "true" : "false",
                           current_manual_mode ? "true" : "false");
                    
                    // Update tracking variable
                    last_shadow_manual_mode[i] = current_manual_mode;
                    prev_manual_mode[i] = current_manual_mode;
                    shadow_update_needed = true;
                }
                
                // Also track general state changes for logging
                if (pumps[i].state != prev_states[i]) {
                    printf("\n[PUMP] Pump %d state changed: %d -> %d\n", 
                           i, prev_states[i], pumps[i].state);
                    prev_states[i] = pumps[i].state;
                }
            }
            
            xSemaphoreGive(mutexPumpState);
            
            // Trigger shadow update if needed
            if (shadow_update_needed) {
                printf("\n[PUMP] Triggering event-driven shadow update\n");
                vTaskDelay(pdMS_TO_TICKS(100));
                update_shadow_state();
            }
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
                    if (xSemaphoreTake(mutexPumpState, portMAX_DELAY) == pdTRUE) {
                        if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(10)) == pdTRUE) {
                            manual_activate_all_pumps();
                            
                            // Set startAllPumps as active for local commands too
                            startAllPumpsActive = true;
                            startAllPumpsActivationTime = xTaskGetTickCount();
                            
                            xSemaphoreGive(mutexWaterState);
                        }
                        xSemaphoreGive(mutexPumpState);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        update_shadow_state();  // Send acknowledgement
                    }
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
// ========================================
// STATE MACHINE TASK
// ========================================

// ========================================
// IMPROVED STATE MACHINE WITH WIFI MONITORING
// ========================================

void task_state_machine(void *parameter) {
    static TickType_t last_mqtt_check = 0;
    static TickType_t last_wifi_check = 0;  // NEW: WiFi monitoring
    static int wifi_reconnect_attempts = 0;
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    for (;;) {
        TickType_t current_time = xTaskGetTickCount();
        
        switch (current_state) {
            case STATE_INIT:
                printf("\n[STATE] INIT");
                current_state = STATE_WIFI_CONNECTING;
                last_state_change = current_time;
                break;
                
            case STATE_WIFI_CONNECTING:
                if (is_wifi_connected()) {
                    printf("\n[STATE] WiFi Connected");
                    time_manager_notify_wifi_status(true);
                    printf("\n[STATE] Time sync started in background");
                    printf("\n[STATE] CHECK_PROVISION");
                    current_state = STATE_CHECK_PROVISION;
                    last_state_change = current_time;
                    wifi_reconnect_attempts = 0;  // Reset counter on success
                } else if ((current_time - last_state_change) > pdMS_TO_TICKS(45000)) {
                    printf("\n[STATE] WiFi timeout, retrying...");
                    time_manager_notify_wifi_status(false);
                    wifi_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    init_wifi();
                    last_state_change = current_time;
                }
                break;
                
            case STATE_CHECK_PROVISION:
                printf("\n[STATE] Checking provisioning status...");
                check_provisioning_status();
                
                if (is_provisioned) {
                    printf("\n[STATE] Device is provisioned");
                    printf("\n[STATE] Connecting with device certificate");
                    printf("\n[STATE] Thing Name:%s",thing_name);
                    if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
                        subscribe_to_topics();
                        printf("\n[STATE] Device Type: %s", DEVICE_TYPE);
                        current_state = STATE_REGISTERING;
                        last_state_change = current_time;
                    } else {
                        printf("\n[STATE] MQTT connection failed");
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                } else {
                    printf("\n[STATE] Device NOT provisioned");
                    printf("\n[STATE] PROVISIONING");
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
                 if ((current_time - last_wifi_check) > pdMS_TO_TICKS(10000)) {
                    last_wifi_check = current_time;
                    
                    if (!is_wifi_connected()) {
                        printf("\n[STATE]  WiFi DISCONNECTED in operational state!");
                        time_manager_notify_wifi_status(false);
                        
                        // Increment reconnect attempts
                        wifi_reconnect_attempts++;
                        printf("\n[STATE] WiFi reconnection attempt %d/10", wifi_reconnect_attempts);
                        
                        // Try to reconnect WiFi
                        wifi_disconnect();
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        reconnect_wifi();
                        
                        // Wait for connection (max 30 seconds)
                        int wait_count = 0;
                        while (!is_wifi_connected() && wait_count < 30) {
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            wait_count++;
                        }
                        
                        if (is_wifi_connected()) {
                            printf("\n[STATE] WiFi RECONNECTED successfully!");
                            time_manager_notify_wifi_status(true);
                            wifi_reconnect_attempts = 0;  // Reset counter
                            
                            // Force MQTT reconnection after WiFi recovery
                            printf("\n[STATE] Reconnecting MQTT after WiFi recovery...");
                            if (mqtt_client) {
                                esp_mqtt_client_stop(mqtt_client);
                                vTaskDelay(pdMS_TO_TICKS(1000));
                            }
                            
                            if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
                                subscribe_to_topics();
                                printf("\n[STATE] MQTT reconnected after WiFi recovery");
                           		// NEW: Send pending alerts after reconnection
			                    printf("\n[STATE] Sending pending alerts after reconnection...");
			                    send_pending_alerts_from_storage();
			                           
                            }
                        } else {
                            printf("\n[STATE] WiFi reconnection failed (attempt %d)", 
                                   wifi_reconnect_attempts);
                            
                            // If too many failed attempts, go to error state
                            if (wifi_reconnect_attempts >= 10) {
                                printf("\n[STATE] WiFi reconnection failed after 10 attempts");
                                current_state = STATE_ERROR;
                                last_state_change = current_time;
                            }
                        }
                    } else {
                        // WiFi is connected, reset counter
                        if (wifi_reconnect_attempts > 0) {
                            wifi_reconnect_attempts = 0;
                        }
                    }
                }
                
                // ========================================
                // MQTT CONNECTION MONITORING (every 30 seconds)
                // ========================================
                if ((current_time - last_mqtt_check) > pdMS_TO_TICKS(30000)) {
                    last_mqtt_check = current_time;
                    
                    // Only try MQTT reconnection if WiFi is up
                    if (is_wifi_connected() && !mqtt_connected) {
                        printf("\n[STATE] MQTT disconnected, reconnecting...");
                        
                        if (mqtt_connect(thing_name, device_cert_pem, device_private_key) == ESP_OK) {
                            subscribe_to_topics();
                            printf("\n[STATE] MQTT reconnected successfully");
                            // NEW: Send pending alerts after MQTT reconnection
			                printf("\n[STATE] Sending pending alerts after MQTT reconnection...");
			                send_pending_alerts_from_storage();
                        } else {
                            printf("\n[STATE] MQTT reconnection failed");
                        }
                    } else if (!is_wifi_connected()) {
                        printf("\n[STATE] Cannot reconnect MQTT - WiFi is down");
                    }
                }
                
                // NEW: Periodically check for pending alerts (every 60 seconds)
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
                wifi_reconnect_attempts = 0;  // Reset WiFi counter
                
                printf("\n[STATE] Disconnecting WiFi...");
                time_manager_notify_wifi_status(false);
                wifi_disconnect();
                
                printf("\n[STATE] Waiting 10 seconds before retry...");
                vTaskDelay(pdMS_TO_TICKS(10000));
                
                printf("\n[STATE] INIT (retry)");
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
    
    printf("\n=== STATUS REPORT #%d ===\n", display_count);
    printf("Thing: %s | Provisioned: %s\n", thing_name, is_provisioned ? "YES" : "NO");
    printf("MQTT Connected: %s\n", mqtt_connected ? "YES" : "NO");
    
    printf("Time Synced: %s\n", time_manager_is_synced() ? "YES" : "NO");
    char timestamp[32];
    if (time_manager_get_timestamp(timestamp, sizeof(timestamp)) == ESP_OK) {
        printf("Current Time (UTC): %s\n", timestamp);
    }
    
    printf("\nWIFI STATUS:\n");
    char ip_address[16];
    get_wifi_ip_address(ip_address, sizeof(ip_address));
    printf("Connected: %s | IP: %s | SSID: %s \n",
           is_wifi_connected() ? "YES" : "NO",
           ip_address,
           get_current_wifi_ssid());
    
    // ADDED: Show startAllPumps status
    printf("startAllPumps Active: %s\n", startAllPumpsActive ? "YES" : "NO");
    if (startAllPumpsActive) {
        TickType_t elapsed = xTaskGetTickCount() - startAllPumpsActivationTime;
        printf("  Active for: %u seconds\n", (unsigned int)(elapsed * portTICK_PERIOD_MS / 1000));
    }
    
    const char* profileName = "Unknown";
    if (currentProfile >= WILDLAND_STANDARD && currentProfile <= CONTINUOUS_FEED) {
        profileName = profiles[currentProfile].name;
    }
    
    printf("Current Profile: %d (%s)\n", convert_profile_enum_to_number(currentProfile), profileName);
    printf("Emergency Stop: %s\n", emergencyStopActive ? "ACTIVE" : "INACTIVE");
    printf("Water Lockout: %s\n", waterLockout ? "YES" : "NO");
    printf("Continuous Feed: %s\n", continuousWaterFeed ? "YES" : "NO");
    
    printf("\nPUMP STATUS:\n");
    for (int i = 0; i < 4; i++) {
        const char* state_str = get_pump_state_string(i);
        const char* stop_reason_str = get_stop_reason_string(pumps[i].lastStopReason);
        const char* activation_str = get_activation_source_string(pumps[i].activationSource);
        printf("Pump %d (%s): State=%s, Running=%s, Source=%s, StopReason=%s\n",
               i+1, pumps[i].name, state_str,
               pumps[i].isRunning ? "YES" : "NO",
               activation_str,
               stop_reason_str);
    }
    
    printf("\nSENSOR STATUS:\n");
    printf("Water Level: %.1f%%\n", level_s);
    printf("IR Sensors: N=%.1f%%, S=%.1f%%, E=%.1f%%, W=%.1f%%\n", 
           ir_s1, ir_s2, ir_s3, ir_s4);
    printf("Battery: %.2fV | Solar: %.2fV\n", bat_v, sol_v);
    
    // ADDED: Show fire detection type info
    FireDetectionInfo* fireInfo = get_fire_detection_info();
    printf("\nFIRE DETECTION STATUS:\n");
    printf("Fire Type: %s\n", get_fire_detection_type_string(fireInfo->type));
    printf("Active Sectors: %d (%s)\n", 
           fireInfo->activeSectorCount,
           fireInfo->activeSectorNames[0] ? fireInfo->activeSectorNames : "none");
    
    printf("\nSYSTEM STATUS:\n");
    printf("Suppression Active: %s\n", is_suppression_active() ? "YES" : "NO");
    printf("Door: %s\n", doorOpen ? "OPEN" : "CLOSED");
    if (doorOpen) {
        unsigned long openTime = (xTaskGetTickCount() * portTICK_PERIOD_MS - doorOpenTime) / 1000;
        printf("Door open for: %lu seconds\n", openTime);
    }
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
    esp_log_level_set("*", ESP_LOG_INFO);
    
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
    
    printf("\n[INIT] Initializing time manager...");
    esp_err_t time_init = time_manager_init();
    if (time_init != ESP_OK) {
        printf("\n[INIT] WARNING: Time manager init failed: %s", esp_err_to_name(time_init));
    } else {
        printf("\n[INIT] Time manager initialized (UTC mode)");
    }
    
    // Initialize SPIFFS
    spiffs_init();
    
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

    
    // Initialize hardware
    init_fire_suppression_system();
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
    xTaskCreate(task_sensor_reading, "Sensor", TASK_SENSOR_STACK_SIZE, NULL, TASK_PRIORITY_SENSOR, &taskSensorHandle);
    xTaskCreate(task_fire_detection, "Fire", TASK_FIRE_DETECT_STACK_SIZE, NULL, TASK_PRIORITY_FIRE_DETECT, &taskFireDetectionHandle);
    xTaskCreate(task_pump_management, "Pump", TASK_PUMP_MGMT_STACK_SIZE, NULL, TASK_PRIORITY_PUMP_MGMT, &taskPumpManagementHandle);
    xTaskCreate(task_water_lockout, "Water", TASK_WATER_LOCK_STACK_SIZE, NULL, TASK_PRIORITY_WATER_LOCK, &taskWaterLockoutHandle);
    xTaskCreate(task_command_processor, "Cmd", TASK_CMD_STACK_SIZE, NULL, TASK_PRIORITY_CMD, &taskCommandHandle);
    xTaskCreate(task_door_monitoring, "Door", TASK_DOOR_STACK_SIZE, NULL, TASK_PRIORITY_DOOR, &taskDoorHandle);
    xTaskCreate(task_serial_monitor, "Mon", TASK_MONITOR_STACK_SIZE, NULL, TASK_PRIORITY_MONITOR, &taskMonitorHandle);
    xTaskCreate(task_mqtt_publish, "Mqtt", TASK_MQTT_PUBLISH_STACK_SIZE, NULL, TASK_PRIORITY_MQTT_PUBLISH, &taskMqttPublishHandle);
    xTaskCreate(task_state_machine, "State", TASK_STATE_MACHINE_STACK_SIZE, NULL, TASK_PRIORITY_STATE_MACHINE, &taskStateMachineHandle);
    
    printf("[INIT] System Running\n");
    
    // Main loop
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
*/
//-----------------------DEEP----


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

// ========================================
// AWS IoT CONFIGURATION
// ========================================
#define AWS_IOT_ENDPOINT "a3t2gw3osxkpr2-ats.iot.us-east-1.amazonaws.com"
#define AWS_IOT_PORT 8883
#define CLAIM_THING_NAME "ClaimDevice"

// Provisioning topics

#define SECURE_PROVISION_REQUEST_TOPIC      "Provision/Request/%s"      // Device → Lambda
#define SECURE_PROVISION_RESPONSE_TOPIC     "Provision/Response/%s"     // Lambda → Device
#define SECURE_PROVISION_TIMEOUT_MS         30000   // 30 seconds for Lambda response
#define REGISTER_THING_TIMEOUT_MS           30000   // 30 seconds for RegisterThing



// ========================================
// SYSTEM CONFIGURATION
// ========================================
#define TASK_SENSOR_STACK_SIZE      8192
#define TASK_FIRE_DETECT_STACK_SIZE 8192
#define TASK_PUMP_MGMT_STACK_SIZE   6144
#define TASK_WATER_LOCK_STACK_SIZE  4096
#define TASK_MONITOR_STACK_SIZE     4096
#define TASK_DOOR_STACK_SIZE        4096
#define TASK_CMD_STACK_SIZE         8192
#define TASK_MQTT_PUBLISH_STACK_SIZE 4096
#define TASK_STATE_MACHINE_STACK_SIZE 6144
#define TASK_ALERT_STACK_SIZE       4096

// Task priorities
#define TASK_PRIORITY_SENSOR        2
#define TASK_PRIORITY_FIRE_DETECT   3
#define TASK_PRIORITY_PUMP_MGMT     2
#define TASK_PRIORITY_WATER_LOCK    2
#define TASK_PRIORITY_MONITOR       1
#define TASK_PRIORITY_DOOR          1
#define TASK_PRIORITY_CMD           2
#define TASK_PRIORITY_MQTT_PUBLISH  1
#define TASK_PRIORITY_STATE_MACHINE 3
#define TASK_PRIORITY_ALERT         2

// Device Configuration
#define MAX_TOPIC_LENGTH 128
#define DEVICE_TYPE "G"

// Timing intervals (in milliseconds)
#define HEARTBEAT_INTERVAL          60000
#define SYSTEM_STATUS_INTERVAL      70000
#define SHADOW_UPDATE_INTERVAL      30000

// Memory optimization constants
#define MIN_FREE_HEAP_THRESHOLD     10240  // 10KB minimum free heap
#define MAX_JSON_PAYLOAD_SIZE       1024   // Reduced from 2048
#define MQTT_QOS_LEVEL              0      // Use QoS 0 for memory efficiency

#define ALERT_SYSTEM_ENABLED 1

#define SENSOR_WARMUP_SECONDS 15  // Wait 15 seconds for sensors to stabilize

// ========================================
// GSM FALLBACK CONFIGURATION
// ========================================
#define GSM_ENABLED                 1       // Set to 0 to disable GSM fallback
#define WIFI_MAX_RETRY_BEFORE_GSM   3       // WiFi failures before GSM fallback
#define WIFI_RETRY_WHEN_ON_GSM_MS   300000  // Try WiFi every 5 min when on GSM

//---gsm test
// UART configuration
#define GSM_UART       UART_NUM_2
#define GSM_TX_PIN     16
#define GSM_RX_PIN     17
#define GSM_BAUDRATE   115200

// GSM control pins
#define GSM_PWRKEY     12
#define GSM_POWER     4
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
    ALERT_TYPE_PROFILE_CHANGE,
    ALERT_TYPE_EMERGENCY_STOP,           
    ALERT_TYPE_SYSTEM_RESET,             
    ALERT_TYPE_START_ALL_PUMPS,          
    ALERT_TYPE_PUMP_STATE_CHANGE,
    ALERT_TYPE_PUMP_EXTEND_TIME,
    ALERT_TYPE_FIRE_DETECTED,
    ALERT_TYPE_FIRE_CLEARED,         
    ALERT_TYPE_MULTIPLE_FIRES,
    ALERT_TYPE_WATER_LOCKOUT,
    ALERT_TYPE_DOOR_STATUS,
    ALERT_TYPE_MANUAL_OVERRIDE,
    ALERT_TYPE_AUTO_ACTIVATION,    
    ALERT_TYPE_WIFI_UPDATE,       
    ALERT_TYPE_SYSTEM_ERROR,
    ALERT_TYPE_SENSOR_FAULT,
    ALERT_TYPE_CONTINUOUS_FEED, 
    ALERT_TYPE_CURRENT_SENSOR_FAULT,
    ALERT_TYPE_IR_SENSOR_FAULT,
    ALERT_TYPE_HARDWARE_CONTROL_FAIL,
    ALERT_TYPE_ADC_INIT_FAIL,
    ALERT_TYPE_PCA9555_FAIL,
        
    // System State Alerts
    ALERT_TYPE_GRACE_PERIOD_EXPIRED,
    ALERT_TYPE_PUMP_COOLDOWN,
    ALERT_TYPE_TIMER_OVERRIDE,
    
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
    STATE_GSM_CONNECTING,      // NEW: GSM fallback state
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
    char payload[1024];  // Reduced from 2048
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
            float sensorTemperature;
            char stopReason[32];
            int totalRuntime;
            int cooldownDuration;
            int previousRuntime;
        } pump;
        
        // Pump extension data
        struct {
            int pumpId;
            char pumpName[16];
            int extensionCode;
            int extensionDuration;
            int newTotalRuntime;
        } pumpExtend;
        
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
            // NEW: Fire detection type info
            int fireType;                    // 0=none, 1=single, 2=multiple, 3=full
            char fireTypeString[24];         // "SINGLE_SECTOR", "MULTIPLE_SECTORS", "FULL_SYSTEM"
            int totalActiveSectors;          // Total number of sectors with fire
            char allActiveSectors[32];       // List of all active sectors e.g. "N,S,E"
        } fire;
        
        // Multiple fires data
        struct {
            int activeFireCount;
            struct {
                char sector[16];
                float temperature;
                bool pumpActive;
            } affectedSectors[4];
            float waterLevel;
            float estimatedRuntime;
            // NEW: Fire detection type info
            int fireType;                    // 2=multiple, 3=full
            char fireTypeString[24];         // "MULTIPLE_SECTORS" or "FULL_SYSTEM"
        } multipleFires;
        
        // Water lockout data
        struct {
            bool activated;
            float currentWaterLevel;
            float minThreshold;
            bool allPumpsDisabled;
            bool continuousFeedActive;
            char systemStatus[32];
        } waterLockout;
        
        // Door status data
        struct {
            bool opened;
            char action[16];
            bool doorState;
            bool securityConcern;
            int wasOpenDuration;
        } door;
        
        // Manual override data
        struct {
            bool activated;
            char action[16];
            int manualPumpCount;
            struct {
                int pumpId;
                char pumpName[16];
                char state[16];
            } manualPumps[4];
            bool autoProtectionDisabled;
            bool autoProtectionEnabled;
            char activationSource[32];
            char systemMode[16];
            int totalManualDuration;
        } manualOverride;
        
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
            int estimatedRuntime;        // Minutes remaining
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


// ========================================
// GLOBAL VARIABLES
// ========================================
static bool wifi_already_initialized = false;
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
static int last_shadow_extend_time[4] = {-1, -1, -1, -1};
static bool last_shadow_stop_pump[4] = {false,false,false, false};
static bool last_shadow_manual_mode[4] = {false, false, false, false};
static int last_reported_extend_time[4] = {-1, -1, -1, -1};
static bool last_reported_manual_mode[4] = {false, false, false, false};

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
static bool fire_alerts_active[4] = {false};
static int active_fire_count = 0;

// NEW: Start All Pumps tracking
static bool startAllPumpsActive = false;
static TickType_t startAllPumpsActivationTime = 0;
static int pending_extend_ack[4] = {-1, -1, -1, -1};
static int previous_extend_time[4] = {-1, -1, -1, -1};

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
extern bool emergencyStopActive; 


// ========================================
// HELPER FUNCTIONS - ENUM TO STRING
// ========================================

static const char* get_alert_type_string(alert_type_t type) {
    switch(type) {
        case ALERT_TYPE_PROFILE_CHANGE: return "profileChange";
        case ALERT_TYPE_EMERGENCY_STOP: return "emergencyStop";
        case ALERT_TYPE_SYSTEM_RESET: return "systemReset";
        case ALERT_TYPE_START_ALL_PUMPS: return "startAllPumps";
        case ALERT_TYPE_PUMP_STATE_CHANGE: return "pumpStateChange";
        case ALERT_TYPE_PUMP_EXTEND_TIME: return "pumpExtendTime";
        case ALERT_TYPE_FIRE_DETECTED: return "fireDetected";
        case ALERT_TYPE_FIRE_CLEARED: return "fireCleared";
        case ALERT_TYPE_MULTIPLE_FIRES: return "multipleFires";
        case ALERT_TYPE_WATER_LOCKOUT: return "waterLockout";
        case ALERT_TYPE_DOOR_STATUS: return "doorStatus";
        case ALERT_TYPE_MANUAL_OVERRIDE: return "manualOverride";
        case ALERT_TYPE_AUTO_ACTIVATION: return "autoActivation";
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
                
        // System state
        case ALERT_TYPE_GRACE_PERIOD_EXPIRED: return "gracePeriodExpired";
        case ALERT_TYPE_PUMP_COOLDOWN: return "pumpCooldown";
        case ALERT_TYPE_TIMER_OVERRIDE: return "timerOverride";
        
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



// ========================================
// AWS CERTIFICATES
// ========================================


// ==================== AWS CLAIM CERTIFICATE ====================
static const char AWS_CA_CERT[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
"rqXRfboQnoZsG4q5WTP468SQvvG5\n"
"-----END CERTIFICATE-----";

// ==================== AWS CLAIM CERTIFICATE ====================
static const char AWS_CLAIM_CERT[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDWTCCAkGgAwIBAgIUVLqEQh+m3sbqpAf/pgxhfhm13kkwDQYJKoZIhvcNAQEL\n"
"BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g\n"
"SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTI1MDgwNzEwNDkw\n"
"NVoXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0\n"
"ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKPCODUQ/XtJBxhqoq0+\n"
"k4H8mrF08aRMaZ5H597t/bEp5kCfIkpx/Q2T8wx4XJGZSZKS/J7OORLvJqQlRItR\n"
"7x8x+0s5PcqjSVIvr77bMElf5R9dO26YpgGG3L1DhSXLbXthznALceDdUWoNl1d+\n"
"HeZs7XsaPCDIswHWK0vFvk2C2HknPY5METseFxAqS/Uzu3oAu68qJcbgrj6wNDWe\n"
"7pbM/+A2qvYi2w7gnYIMi6DN/3yFx7tTQdQyntOFRcS4SE1wcY/NkH+MTsyY418T\n"
"BW4NDQfPajeSLcMEUgXIdRKonSp3PkgS3Xr3oOppec55D3SVVNWTdGEe/zh9qmjP\n"
"YQsCAwEAAaNgMF4wHwYDVR0jBBgwFoAU4SIhyBUw9JO5U+wtNZ/MUg9SyuIwHQYD\n"
"VR0OBBYEFC/OCRI7hQ4mupBmaZcAWlTRBTyWMAwGA1UdEwEB/wQCMAAwDgYDVR0P\n"
"AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQDJUiJ4wWJ70D0rW2JiVcXqkyui\n"
"d+tWeMKMc4qxVBAyRvH8oWZn8l9+gC8+7+6dH6mc5YRx0tGl0jbt2KwPjDyS8zn1\n"
"HXMpa+SEmmibXJ3rKLE4rb5uHRVNfEe+bNruOcpRcp6Vmgn9Ortq314acBsURE8s\n"
"s5WWWzv/go1UiZ9x6AWUXWb/kRwxvf9EuU8/t0jtfhxCxmuFrg/APBaZ4c6A5K+j\n"
"jGJ0BIeARFJap49Rc0QPryihB/MDJUyEY/KT8SXUjJlzi82mOb8MPGK8WzAPtW3c\n"
"QeTBQ/PYSEFwt3I/JuwNiHQ+ebsm9MK9NWgSv5pMTWFd/N3E5se7jcwiaKu6\n"
"-----END CERTIFICATE-----";

// ==================== AWS CLAIM PRIVATE KEY ====================
static const char AWS_CLAIM_PRIVATE_KEY[] =
"-----BEGIN RSA PRIVATE KEY-----\n"
"MIIEowIBAAKCAQEAo8I4NRD9e0kHGGqirT6TgfyasXTxpExpnkfn3u39sSnmQJ8i\n"
"SnH9DZPzDHhckZlJkpL8ns45Eu8mpCVEi1HvHzH7Szk9yqNJUi+vvtswSV/lH107\n"
"bpimAYbcvUOFJctte2HOcAtx4N1Rag2XV34d5mztexo8IMizAdYrS8W+TYLYeSc9\n"
"jkwROx4XECpL9TO7egC7ryolxuCuPrA0NZ7ulsz/4Daq9iLbDuCdggyLoM3/fIXH\n"
"u1NB1DKe04VFxLhITXBxj82Qf4xOzJjjXxMFbg0NB89qN5ItwwRSBch1EqidKnc+\n"
"SBLdeveg6ml5znkPdJVU1ZN0YR7/OH2qaM9hCwIDAQABAoIBAQCCxJ950N16S7C8\n"
"0LqjOas1S/CD8Ozd1J8q5CTHIqlJhjn2NJ1/cVMwOosF1D+njQ7xWysb7XYqJotm\n"
"3NPFpWIcOR+AzG8JmCb+2FGxSPtgPJGM4DiLcp5t7bHr+TUkHzSIKGxfkOQZOuK+\n"
"m6fVGELsNOPXP/XwABTiTJI6aegzoBeamoOWVT4FV9vpG73sYoswaviCCaqvgkJr\n"
"3OYyDv+ml7GQ8nG3PRISxGI9t3KmwGoUO4+YbAusCOnISoC2UcL4mGSX1U1y5IbK\n"
"ULDlSOxEjUiv4MOPptZr54TQspr0N/6sjK294D9tnb02zgcNS/bZpnHd8pPtvhJA\n"
"OGcAbE3RAoGBANhCikaxVmtx2b3TQrPYywAXjxyr932oAGedp2JgvTOKciFj2Pe7\n"
"NZjrY9ocvJTDY2d+GcsYZVNYtq6ynSH7eJArFLkx7UQLs/qu38YGvp6plNtHMtLa\n"
"hcjExgUyEsutK8eQvOXeQIE3rX4ad2aK68j8ePxuQZuPapIyQ5XgsyAVAoGBAMHZ\n"
"4GPV19EJDgaGaI2D2Icqkg7etw0AGlmZKYZlxE3KSpvCrbvy2+p4nBgjadljBZs1\n"
"mfqkBuNqwvc8MOm315HPrD2nSml4CEw3g7Obai5oibNOoQQc3jADSrYsM9k+Gusp\n"
"BvyGmDE1B5N26U9iue91UO7ZeCExzmkPhxGkPaSfAoGALnzfTKMCeMZYkD3BsPeB\n"
"a9ukn/03joN20s9JFBTHlzTDo/nawiY0N1Mie9iBkVkPHUg2MzpjTa9cVeF/dbah\n"
"DBy2r7jT0DTT06eT4vXANEsv/JMpkbn32Fi0WJmTAMWRC61JbgCAzUYyvVDjKd/j\n"
"H6lmOJ1a7R2/Qv4bGTTcTKECgYBVKdsjATenZkr7IuGcCmh+OX2hescAtyLcaiWM\n"
"Hfl4E39jnsuk3rUu9X3ePPCryI0V+x6Ctr0v/B9bbt4uT84tCQeqrmxKmalLkrgR\n"
"mB219cdJNyoWHHigr1GLZzAAKQC6f3PKTXdfZuTFLGCjt8PoJ6o+xNu5+Z+tGF1G\n"
"qtlKEQKBgErmX6Nks5U+UNsvzMuNXmzLVxBh8wz7VJg1/5M88p+0rMXKWn8BW/sf\n"
"ZKXJ26mzvAMDve6MZCo5iQZcX1oVUWEGDj84qtvXIDn36pJXBDbGgsz5KpQAzqi4\n"
"lpI8zu9ZOEcDDd5SksaOsPMQrPFBLoXtiB/gGG9MC23a072U9ls+\n"
"-----END RSA PRIVATE KEY-----";




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
static void send_alert_pump_extend_time(int pumpIndex, int extensionCode,
                                       int extensionDuration, int newTotalRuntime);
static void send_alert_fire_detected(int sensorIndex, const char* sectorName,
                                    float temperature, bool pumpActivated);
static void send_alert_fire_cleared(int sensorIndex, const char* sectorName,
                                   float currentTemp);
static void send_alert_multiple_fires(int fireCount, float sensorValues[4],
                                     bool pumpStates[4]);
static void send_alert_water_lockout(bool activated, float currentLevel, float threshold);
static void send_alert_door_status(bool opened, int openDuration);
static void send_alert_manual_override(bool activated, int manualDuration);
static void send_alert_auto_activation(void);
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
void send_alert_battery_critical(float batteryVoltage, int estimatedRuntime);
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


// ========================================
// NEW: GSM HELPER FUNCTIONS
// ========================================

#if GSM_ENABLED
/**
 * @brief Try to establish GSM connection
 * @return true if connected successfully
 */
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

/**
 * @brief Handle GSM disconnection
 */
static void handle_gsm_disconnect(void) {
    printf("\n[GSM] Handling GSM disconnection...");
    // time_manager is notified inside gsm_manager's IP event handler
    // via time_manager_notify_network(false, TIME_NET_GSM)
}
#endif

/**
 * @brief Try to reconnect WiFi
 * @return true if connected successfully
 */
static bool try_wifi_reconnection(void) {
    printf("\n[WiFi] Attempting WiFi reconnection...");
    
    wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(2000));
    reconnect_wifi();
    
    // Wait for connection (max 30 seconds)
    int wait_count = 0;
    while (!is_wifi_connected() && wait_count < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait_count++;
    }
    
    if (is_wifi_connected()) {
        printf("\n[WiFi] WiFi reconnected successfully");
        return true;
    }
    
    printf("\n[WiFi] WiFi reconnection failed");
    return false;
}

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

/**
 * @brief Store alert to SPIFFS with topic information
 */
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

/**
 * @brief Send all pending alerts from SPIFFS storage
 */
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

/**
 * @brief Check and send pending alerts based on conditions
 * @param force_check If true, check immediately regardless of timing
 */
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
            printf("\n[SHADOW] All pumps stopped, resetting startAllPumpsActive to false");
            startAllPumpsActive = false;
            
            // ✅ IMPORTANT: Update shadow immediately (event-driven)
            vTaskDelay(pdMS_TO_TICKS(100));
            update_shadow_state();
        }
    }
}


/**
 * @brief Process WiFi credential changes from shadow
 * @return true if WiFi needs reconnection, false otherwise
 */
static bool process_wifi_credentials_from_shadow(cJSON *state) {
    bool credentials_changed = false;
    
    // Check for WiFi configuration object
    cJSON *wifiConfig = cJSON_GetObjectItem(state, "wifiCredentials");
    if (!wifiConfig || !cJSON_IsObject(wifiConfig)) {
        return false;
    }
    
    cJSON *ssid = cJSON_GetObjectItem(wifiConfig, "ssid");
    cJSON *password = cJSON_GetObjectItem(wifiConfig, "password");
    
    if (!ssid || !password) {
        printf("\n[SHADOW] WiFi config incomplete");
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
        printf("\n[SHADOW] WiFi credentials validation failed");
        
        // Report error back to shadow
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                "Invalid WiFi credentials: SSID length=%d, Password length=%d",
                strlen(new_ssid), strlen(new_password));
        send_alert_wifi_invalid(strlen(new_ssid), strlen(new_password), 
                       "SSID empty or password too short");
        return false;
    }
    
    // Check if credentials are different from current
    const char *current_ssid = get_current_wifi_ssid();
    if (strcmp(current_ssid, new_ssid) != 0) {
        credentials_changed = true;
    }
    
    if (credentials_changed) {
        printf("\n[SHADOW] WiFi credentials changed");
        printf("\n[SHADOW] New SSID: %s", new_ssid);
        printf("\n[SHADOW] New Password: %s", new_password);
        
        // Call the correct function to store credentials
        set_wifi_credentials(new_ssid, new_password);
        
       send_alert_wifi_updated(new_ssid, current_ssid);
        
        return true;
    }
    
    return false;
}

// ==========================================
// SHADOW DELTA PROCESSING FUNCTION
// ==========================================

static bool process_shadow_delta(cJSON *state) {
    bool state_changed = false;
    
    if (process_wifi_credentials_from_shadow(state)) {
        printf("\n[SHADOW] WiFi credentials being updated...");
        state_changed = true;
    }
    
    // 1. PROFILE CHANGE
    cJSON *currentProfileJson = cJSON_GetObjectItem(state, "currentProfile");
    if (currentProfileJson) {
        int profileNum = (int)cJSON_GetNumberValue(currentProfileJson);
        printf("\n[SHADOW] Delta: currentProfile = %d", profileNum);
        
        SystemProfile newProfile = convert_profile_number_to_enum(profileNum);
        
        if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (newProfile != currentProfile) {
                apply_system_profile(newProfile);
                shadow_profile = profileNum;
                printf("[SYSTEM] Profile changed to: %s\n", profiles[newProfile].name);
                state_changed = true;
                
                char alert_msg[128];
                snprintf(alert_msg, sizeof(alert_msg),
                        "Profile changed to %s via shadow", profiles[newProfile].name);
            }
            xSemaphoreGive(mutexSystemState);
        }
    }
    
    // 2. EMERGENCY STOP
    cJSON *emergencyStopJson = cJSON_GetObjectItem(state, "emergencyStop");
    if (emergencyStopJson) {
        bool stopCommand = cJSON_IsTrue(emergencyStopJson);
        printf("\n[SHADOW] Delta: emergencyStop = %s", 
               stopCommand ? "true" : "false");
        
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
    cJSON *systemResetJson = cJSON_GetObjectItem(state, "systemReset");
    if (systemResetJson) {
        bool resetCommand = cJSON_IsTrue(systemResetJson);
        printf("\n[SHADOW] Delta: systemReset = %s", 
               resetCommand ? "true" : "false");
        
        if (resetCommand) {
            printf("\n[SHADOW] SYSTEM RESET REQUESTED");
            
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
                        for (int i = 0; i < 4; i++) {
                            last_shadow_pump_manual[i] = false;
                            last_shadow_manual_mode[i] = false;
                            last_shadow_extend_time[i] = -1;
                            last_reported_extend_time[i] = -1;
                            last_shadow_stop_pump[i] = false;
                            pending_extend_ack[i] = -1;
                            previous_extend_time[i] = -1;
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
    
    // 4. START ALL PUMPS
    cJSON *startAllPumpsJson = cJSON_GetObjectItem(state, "startAllPumps");
    if (startAllPumpsJson) {
        bool desiredStartAllPumps = cJSON_IsTrue(startAllPumpsJson);
        printf("\n[SHADOW] startAllPumps delta received: %s", 
               desiredStartAllPumps ? "true" : "false");
        
        if (desiredStartAllPumps != startAllPumpsActive) {
            if (desiredStartAllPumps) {
                printf("\n[SHADOW] Activating ALL pumps via startAllPumps");
                
                if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(50)) == pdTRUE) {
                        bool result = shadow_manual_activate_all_pumps();
                        
                        if (result) {
                            startAllPumpsActive = true;
                            startAllPumpsActivationTime = xTaskGetTickCount();
                            state_changed = true;
                            
                            printf("\n[SHADOW] Sending immediate shadow update after startAllPumps");
                            update_shadow_state();
                            
                            char alert_msg[128];
                            snprintf(alert_msg, sizeof(alert_msg),
                                    "ALL PUMPS activated via startAllPumps");
                            
                        }
                        xSemaphoreGive(mutexWaterState);
                    }
                    xSemaphoreGive(mutexPumpState);
                }
            } else {
                printf("\n[SHADOW] User requested startAllPumps deactivation");
                
                if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                    shadow_manual_stop_all_pumps();
                    startAllPumpsActive = false;
                    state_changed = true;
                    xSemaphoreGive(mutexPumpState);
                }
            }
        }
    }
    
    // 4. INDIVIDUAL PUMP CONTROLS
    const char* pumpNames[4] = {"NorthPump", "SouthPump", "EastPump", "WestPump"};
    
    for (int i = 0; i < 4; i++) {
        cJSON *pumpObj = cJSON_GetObjectItem(state, pumpNames[i]);
        if (pumpObj && cJSON_IsObject(pumpObj)) {
            printf("\n[SHADOW] Processing %s", pumpNames[i]);
            
            //  CHECK FOR stopPump PARAMETER FIRST (highest priority)
            cJSON *stopPumpJson = cJSON_GetObjectItem(pumpObj, "stopPump");
            if (stopPumpJson && cJSON_IsBool(stopPumpJson)) {
				bool stopPumpvalue = cJSON_IsTrue(stopPumpJson);
				//Only Process if value changed 
				if(stopPumpvalue != last_shadow_stop_pump[i]){
					last_shadow_stop_pump[i] = stopPumpvalue;
					state_changed = true;
					
					if(stopPumpvalue){
						//Process STOP Command
						printf("\n[SHADOW] STOP REQUEST for %s via stopPump parameter", pumpNames[i]);
						if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
                            extern bool shadow_manual_stop_pump_override_timer(int index);
                            shadow_manual_stop_pump_override_timer(i);

                            char alert_msg[128];
                            snprintf(alert_msg, sizeof(alert_msg),
                                    "User stopped %s via stopPump button", pumps[i].name);
                            

                            xSemaphoreGive(mutexPumpState);
                        }
                        continue;
                        
					}
					else
					{
						printf("\n[SHADOW] stopPump cleared for %s", pumpNames[i]);
					}
				}
            }
          
            
            
            //  CHECK IF PUMP IS TIMER-PROTECTED
            if (pumps[i].timerProtected && !is_timer_expired(i)) {
                unsigned long remaining = get_timer_remaining(i);
                printf("\n[SHADOW] %s is TIMER-PROTECTED (%lu seconds remaining) - IGNORING manualMode changes",
                       pumpNames[i], remaining);
                
                // ONLY SKIP manualMode - Still process extendTime below!
            } else {
                // Process manualMode only if NOT timer-protected
	cJSON *manualModeJson = cJSON_GetObjectItem(pumpObj, "manualMode");
	if (manualModeJson) {
	    bool desiredManualMode = cJSON_IsTrue(manualModeJson);
	    bool currentManualMode = (pumps[i].state == PUMP_MANUAL_ACTIVE);
	    
	    printf("\n[SHADOW] %s manualMode desired=%s, current=%s", 
	           pumpNames[i],
	           desiredManualMode ? "true" : "false",
	           currentManualMode ? "true" : "false");
	    
	    // ✅ FIX: Just track the desired state, don't control hardware
	    if (desiredManualMode != last_shadow_manual_mode[i]) {
	        printf("\n[SHADOW] %s: Acknowledging manualMode change %d -> %d (HARDWARE NOT AFFECTED)",
	               pumpNames[i], last_shadow_manual_mode[i], desiredManualMode);
	        
	        // Update tracking to match desired
	        last_shadow_manual_mode[i] = desiredManualMode;
	        state_changed = true;
	        
	        // Only control hardware if going from false -> true
	        if (desiredManualMode && !currentManualMode) {
	            // Activate pump in hardware
	            if (startAllPumpsActive) {
	                printf("\n[SHADOW] BLOCKED: Cannot activate %s - startAllPumps active",
	                       pumpNames[i]);
	            } else if (can_activate_pump_manually(i)) {
	                if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
	                    if (shadow_manual_activate_pump(i)) {
	                        char alert_msg[128];
	                        snprintf(alert_msg, sizeof(alert_msg),
	                                "Manual activation: %s", pumps[i].name);
	                       
	                    }
	                    xSemaphoreGive(mutexPumpState);
	                }
	            }
	        }
	        // ✅ If going from true -> false, just acknowledge (don't stop pump)
	        else if (!desiredManualMode && currentManualMode) {
	            printf("\n[SHADOW] %s: User set manualMode to false - ACKNOWLEDGING ONLY (pump continues running)",
	                   pumpNames[i]);
	        }
	    }
	}
	            }
	            
	            // ✅ EXTEND TIME PROCESSING
	cJSON *extendTimeJson = cJSON_GetObjectItem(pumpObj, "extendTime");
	if (extendTimeJson && cJSON_IsNumber(extendTimeJson)) {
	    int extendValue = (int)cJSON_GetNumberValue(extendTimeJson);
	    
	    // ✅ Check if value changed from what we last reported
	    if (extendValue != last_shadow_extend_time[i]) {
	        printf("\n[SHADOW] %s: extendTime changed %d -> %d",
	               pumpNames[i], last_shadow_extend_time[i], extendValue);
	        
	        // Only process extensions (0-3), not -1
	        if (extendValue >= 0 && extendValue <= 3 && pumps[i].timerProtected) {
	            printf("\n[SHADOW] Processing extension request for %s: code=%d", 
	                   pumpNames[i], extendValue);
	            
	            if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(100)) == pdTRUE) {
	                unsigned long extensionMs = get_duration_from_code(extendValue);
	                
	                if (extensionMs > 0) {
	                    // Extend the timer in hardware
	                    extend_timer_protection(i, extensionMs);
	                    
	                    // ✅ Mark for reporting back the SAME value
	                    last_shadow_extend_time[i] = extendValue;
	                    last_reported_extend_time[i] = extendValue; // Will report this value
	                    state_changed = true;
	                    
	                    char alert_msg[128];
	                    snprintf(alert_msg, sizeof(alert_msg),
	                            "Extended %s by %s", 
	                            pumps[i].name, 
	                            get_duration_code_string(extendValue));
	                  // Calculate new total runtime
					int currentRuntime = (int)(pumps[i].timerDuration / 1000);
					int newTotalRuntime = currentRuntime + (extensionMs / 1000);
					send_alert_pump_extend_time(i, extendValue, extensionMs / 1000, newTotalRuntime);
	                              
	                    printf("\n[SHADOW] %s: Extension applied, will report back extendTime=%d\n",
	                           pumpNames[i], extendValue);
	                }
	                xSemaphoreGive(mutexPumpState);
	            }
	        }
	        // ✅ When user sets back to -1, acknowledge it
	        else if (extendValue == -1) {
	            printf("\n[SHADOW] %s: User reset extendTime to -1, acknowledging\n",
	                   pumpNames[i]);
	            last_shadow_extend_time[i] = -1;
	            last_reported_extend_time[i] = -1;
	            state_changed = true;
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
				            cJSON_AddNumberToObject(ack_reported, "currentProfile", profileNum);
				            
				            // Emergency stop
				            cJSON_AddBoolToObject(ack_reported, "emergencyStop", emergencyStopActive);
				            
				            // System reset
				            cJSON_AddBoolToObject(ack_reported, "systemReset", false);
				            
				            // Report current startAllPumps state
				            cJSON_AddBoolToObject(ack_reported, "startAllPumps", startAllPumpsActive);
				            
				            xSemaphoreGive(mutexSystemState);
				        }
				        
				        // Pump states
				        const char* pumpNames[4] = {"NorthPump", "SouthPump", "EastPump", "WestPump"};
				        
				        for (int i = 0; i < 4; i++) {
				            cJSON *pump_obj = cJSON_CreateObject();
				            if (pump_obj) {
				                // FIX: Report the ACKNOWLEDGED desired state, not hardware state
				                // This ensures reported matches desired after processing
				                bool manual_mode = last_shadow_manual_mode[i];
				                cJSON_AddBoolToObject(pump_obj, "manualMode", manual_mode);
				                printf("\n[SHADOW] ACK %s: manualMode=%s (acknowledging desired state)", 
				                       pumpNames[i], manual_mode ? "true" : "false");
				                
				                // FIX: Report the ACKNOWLEDGED extendTime value
				                // Use last_reported_extend_time which tracks what we processed
				                int extend_val = last_reported_extend_time[i];
				                printf("\n[SHADOW] ACK %s: extendTime=%d (acknowledging processed value)", 
				                       pumpNames[i], extend_val);
				                
				                cJSON_AddNumberToObject(pump_obj, "extendTime", extend_val);
				                cJSON_AddBoolToObject(pump_obj, "stopPump", last_shadow_stop_pump[i]);
				                
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
                    printf("\n[SHADOW] Update accepted");
                        for (int i = 0; i < 4; i++) {
        		if (pending_extend_ack[i] >= 0) {
            			printf("\n[SHADOW] Clearing pending_extend_ack[%d] = %d\n", 
                   			i, pending_extend_ack[i]);
            			pending_extend_ack[i] = -1;
        			}
    			}

                }
                else if (strstr(topic, "/shadow/update/rejected") != NULL) {
                    printf("\n[SHADOW] Update rejected");
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
    printf("\n[SHADOW] Checking for state changes...\n");
    
    if (!mqtt_client || !mqtt_connected) {
        printf("\n[SHADOW] ERROR: MQTT not connected");
        return;
    }
    
   // ✅ DETECT CHANGES
    bool changes_detected = false;
    
    int current_profile = convert_profile_enum_to_number(currentProfile);
    if (current_profile != last_shadow_profile) {
        printf("[SHADOW] Change detected: Profile %d -> %d\n", last_shadow_profile, current_profile);
        changes_detected = true;
        last_shadow_profile = current_profile;
    }
    
    if (emergencyStopActive != last_shadow_emergency_stop) {
        printf("[SHADOW] Change detected: Emergency Stop %d -> %d\n", last_shadow_emergency_stop, emergencyStopActive);
        changes_detected = true;
        last_shadow_emergency_stop = emergencyStopActive;
    }
    
    if (startAllPumpsActive != last_shadow_start_all_pumps) {
        printf("[SHADOW] Change detected: Start All Pumps %d -> %d\n", last_shadow_start_all_pumps, startAllPumpsActive);
        changes_detected = true;
        last_shadow_start_all_pumps = startAllPumpsActive;
    }
    
    // ✅ NEW: Check for manualMode changes that need reporting
    for (int i = 0; i < 4; i++) {
        if (last_shadow_manual_mode[i] != last_reported_manual_mode[i]) {
            printf("[SHADOW] Change detected: Pump %d manualMode %d -> %d (needs reporting)\n", 
                   i, last_reported_manual_mode[i], last_shadow_manual_mode[i]);
            changes_detected = true;
            last_reported_manual_mode[i] = last_shadow_manual_mode[i];
        }
    }
    
    // Check for extendTime acknowledgements
	for (int i = 0; i < 4; i++) {
    	// Check if pending_extend_ack needs processing
    	if (pending_extend_ack[i] >= 0) {
        	printf("[SHADOW] Change detected: Pump %d has pending extendTime acknowledgement (%d)\n", 
            	   i, pending_extend_ack[i]);
        	changes_detected = true;
    		}
    
    	// Check if last_reported_extend_time differs from last_shadow_extend_time
    if (last_reported_extend_time[i] != last_shadow_extend_time[i]) {
        printf("[SHADOW] Change detected: Pump %d extendTime sync needed (%d -> %d)\n", 
               i, last_reported_extend_time[i], last_shadow_extend_time[i]);
        changes_detected = true;
        last_reported_extend_time[i] = last_shadow_extend_time[i];
    }
}
    
    // ✅ If no changes, skip update
    if (!changes_detected) {
        printf("[SHADOW] No changes detected - skipping update\n");
        return;
    }
    
    printf("[SHADOW] CHANGES DETECTED - Sending shadow update...\n");
    
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
    
    cJSON_AddNumberToObject(reported, "currentProfile", profileNum);
    cJSON_AddBoolToObject(reported, "emergencyStop", emergencyStopActive);
    cJSON_AddBoolToObject(reported, "systemReset", false);
    cJSON_AddBoolToObject(reported, "startAllPumps", startAllPumpsActive);
    
    // WiFi Config (for acknowledgement)
    if (wifi_has_custom_credentials()) {
        cJSON *wifiCredentials = cJSON_CreateObject();
        if (wifiCredentials) {
            cJSON_AddStringToObject(wifiCredentials, "ssid", get_current_wifi_ssid());
            cJSON_AddStringToObject(wifiCredentials, "password", get_current_wifi_password());
            cJSON_AddItemToObject(reported, "wifiCredentials", wifiCredentials);
        }
    }
    
   // ✅ PUMP OBJECTS - Report what user wants to see, not hardware state
const char* pumpNames[4] = {"NorthPump", "SouthPump", "EastPump", "WestPump"};

for (int i = 0; i < 4; i++) {
    cJSON *pumpObj = cJSON_CreateObject();
    if (pumpObj) {
        // ✅ MANUAL MODE - Report the last acknowledged desired state
        bool reportedManualMode = last_shadow_manual_mode[i];
        
        printf("\n[SHADOW] Reporting %s: manualMode=%s (tracking value, not hardware)",
               pumpNames[i], reportedManualMode ? "true" : "false");
        
        cJSON_AddBoolToObject(pumpObj, "manualMode", reportedManualMode);
        
        // ✅ EXTEND TIME - Report the last acknowledged value
        int reportedExtendTime = last_reported_extend_time[i];
        
        printf("\n[SHADOW] Reporting %s: extendTime=%d (acknowledged value)",
               pumpNames[i], reportedExtendTime);
        
        cJSON_AddNumberToObject(pumpObj, "extendTime", reportedExtendTime);
        
        // ✅ STOP PUMP
        cJSON_AddBoolToObject(pumpObj, "stopPump", last_shadow_stop_pump[i]);
        
        cJSON_AddItemToObject(reported, pumpNames[i], pumpObj);
    }
}
    
    // Create compact JSON string
    char *json_str = create_compact_json_string(root);
    if (json_str) {
        char shadow_update_topic[128];
        snprintf(shadow_update_topic, sizeof(shadow_update_topic),
                 "$aws/things/%s/shadow/update", thing_name);
        
        printf("\n[SHADOW] Publishing to: %s", shadow_update_topic);
        printf("\n[SHADOW] Payload: %s", json_str);
        
        int msg_id = esp_mqtt_client_publish(mqtt_client, shadow_update_topic, 
                                            json_str, 0, MQTT_QOS_LEVEL, 0);
        
        if (msg_id >= 0) {
            printf("\n[SHADOW] Shadow update sent (msg_id: %d)", msg_id);
        } else {
            printf("\n[SHADOW] Failed to send shadow update (error: %d)", msg_id);
        }
        
        free(json_str);
    }
    
    cJSON_Delete(root);
    printf("\n[SHADOW] State update complete\n");
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
    if (payload_len >= 1024) {
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
        printf("\n  • %s", shadow_update_delta);
        printf("\n  • %s", shadow_get_accepted);
        printf("\n  • %s", shadow_update_accepted);
        printf("\n  • %s", shadow_update_rejected);
        printf("\n  • %s", registration_response_topic);
       
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

// ========================================
// SYSTEM STATUS FUNCTION - OPTIMIZED
// ========================================

static void send_system_status(void) {
    // Collect data efficiently
    int profileNum = 0;
    const char* profileName = "Unknown";
    bool lockout = false;
    float ir_values[4] = {0};
    float current_values[4] = {0};      // ✅ NEW: Array for current sensor values (Amperes)
    bool current_faults[4] = {false};   // ✅ NEW: Track sensor fault status
    bool suppression_active = false;    // ✅ NEW: Track if fire suppression is active
    
    if (xSemaphoreTake(mutexSystemState, pdMS_TO_TICKS(100)) == pdTRUE) {
        profileNum = convert_profile_enum_to_number(currentProfile);
        if (currentProfile >= WILDLAND_STANDARD && currentProfile <= CONTINUOUS_FEED) {
            profileName = profiles[currentProfile].name;
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
        
        // ✅ NEW: Extract current sensor values and fault status
        for (int i = 0; i < 4; i++) {
            current_values[i] = currentSensors[i].currentValue;
            current_faults[i] = currentSensors[i].fault;
        }
        
        xSemaphoreGive(mutexSensorData);
    }
    
    // ✅ NEW: Get suppression active status
    suppression_active = is_suppression_active();
    
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    
    cJSON_AddStringToObject(root, "macAddress", mac_address);
    cJSON_AddStringToObject(root, "event", "periodicupdate");
    cJSON_AddStringToObject(root, "devicetype", "G");
    cJSON_AddStringToObject(root, "timestamp", get_custom_timestamp());
    
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "payload", payload);
    
    // Essential fields
    cJSON_AddStringToObject(payload, "wifiSSID", get_current_wifi_ssid());
    cJSON_AddStringToObject(payload, "password", get_current_wifi_password());
    cJSON_AddBoolToObject(payload, "waterLockout", lockout);
    cJSON_AddBoolToObject(payload, "doorOpen", doorOpen);
    cJSON_AddNumberToObject(payload, "currentProfile", profileNum);
    cJSON_AddStringToObject(payload, "profileName", profileName);
    cJSON_AddNumberToObject(payload, "waterLevel", level_s);
    cJSON_AddNumberToObject(payload, "batteryVoltage", bat_v);
    cJSON_AddNumberToObject(payload, "solarVoltage", sol_v);
    cJSON_AddBoolToObject(payload, "emergencyStopActive", emergencyStopActive);
    
    // ✅ NEW: Add suppression active status
    cJSON_AddBoolToObject(payload, "suppressionActive", suppression_active);
    
    // Compact IR sensors
    cJSON_AddNumberToObject(payload, "irNorth", ir_values[0]);
    cJSON_AddNumberToObject(payload, "irSouth", ir_values[1]);
    cJSON_AddNumberToObject(payload, "irEast", ir_values[2]);
    cJSON_AddNumberToObject(payload, "irWest", ir_values[3]);
    
    // ✅ NEW: Add current sensor values for each pump (in Amperes)
    cJSON_AddNumberToObject(payload, "currentNorth", current_values[0]);
    cJSON_AddNumberToObject(payload, "currentSouth", current_values[1]);
    cJSON_AddNumberToObject(payload, "currentEast", current_values[2]);
    cJSON_AddNumberToObject(payload, "currentWest", current_values[3]);
    
    // ✅ NEW: Add current sensor fault status (optional but recommended)
    cJSON_AddBoolToObject(payload, "currentFaultNorth", current_faults[0]);
    cJSON_AddBoolToObject(payload, "currentFaultSouth", current_faults[1]);
    cJSON_AddBoolToObject(payload, "currentFaultEast", current_faults[2]);
    cJSON_AddBoolToObject(payload, "currentFaultWest", current_faults[3]);
    
    // Pump states with descriptive names (true = running, false = not running)
    cJSON_AddBoolToObject(payload, "NorthPumpState", pumps[0].isRunning);
    cJSON_AddBoolToObject(payload, "SouthPumpState", pumps[1].isRunning);
    cJSON_AddBoolToObject(payload, "EastPumpState", pumps[2].isRunning);
    cJSON_AddBoolToObject(payload, "WestPumpState", pumps[3].isRunning);
    
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

/**
 * @brief Secure Fleet Provisioning - Lambda-Only Flow
 *
 * Flow:
 * 1. Connect with claim certificate
 * 2. Subscribe to Provision/Response/{MAC}
 * 3. Publish {macAddress, deviceType} to Provision/Request/{MAC}
 * 4. Lambda validates device in DynamoDB
 * 5. Lambda creates certificate, Thing, attaches policy
 * 6. Lambda returns cert/key/thingName to device
 * 7. Device saves cert/key to SPIFFS
 * 8. Device disconnects and reconnects with device certificate
 *
 * NO RegisterThing needed - Lambda creates everything!
 */
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
    
    // Check startAllPumps status
    static bool last_start_all_pumps = false;
    if (startAllPumpsActive != last_start_all_pumps) {
        if (startAllPumpsActive) {
            send_alert_start_all_pumps_activated();
        } else {
            send_alert_start_all_pumps_deactivated("TIMER_EXPIRED", 90);
        }
        last_start_all_pumps = startAllPumpsActive;
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
            // Gather relevant data
            const char* activationSource = NULL;
            const char* trigger = NULL;
            const char* stopReason = NULL;
            float sensorTemp = 0.0f;
            int runtime = 0;
            int cooldown = 0;
            
            // Determine activation source and trigger
            if (current_state == PUMP_AUTO_ACTIVE) {
                trigger = "FIRE_DETECTED";
                float sensorValues[4] = {ir_s1, ir_s2, ir_s3, ir_s4};
                sensorTemp = sensorValues[i];
            } else if (current_state == PUMP_MANUAL_ACTIVE) {
                if (pumps[i].activationSource == ACTIVATION_SOURCE_SHADOW_SINGLE) {
                    activationSource = "SHADOW";
                } else {
                    activationSource = "MANUAL";
                }
            } else if (current_state == PUMP_OFF) {
                // Determine stop reason
                switch(pumps[i].lastStopReason) {
                    case STOP_REASON_MANUAL:
                        stopReason = "MANUAL_STOP";
                        break;
                    case STOP_REASON_TIMEOUT:
                        stopReason = "TIMER_EXPIRED";
                        break;
                    case STOP_REASON_EMERGENCY_STOP:
                        stopReason = "EMERGENCY_STOP";
                        break;
                    case STOP_REASON_WATER_LOCKOUT:
                        stopReason = "WATER_LOCKOUT";
                        break;
                    default:
                        stopReason = "SYSTEM";
                        break;
                }
                
                // Calculate runtime if timer was active
                if (pumps[i].timerProtected) {
                    runtime = (int)(pumps[i].protectionTimeRemaining / 1000);
                }
            } else if (current_state == PUMP_COOLDOWN) {
                // Get cooldown from profile
					cooldown = (int)(profiles[currentProfile].cooldown / 1000);
            }
            
            // Send alert with all data
            send_alert_pump_state_change(i, last_pump_states[i], current_state,
                                         activationSource, trigger, sensorTemp,
                                         stopReason, runtime, cooldown);
            
            last_pump_states[i] = current_state;
        }
    }
    
    // Check door status
    static TickType_t door_open_start_time = 0;
    if (doorOpen != last_door_state) {
        if (doorOpen) {
            door_open_start_time = xTaskGetTickCount();
            send_alert_door_status(true, 0);
        } else {
            int openDuration = (int)((xTaskGetTickCount() - door_open_start_time) * portTICK_PERIOD_MS / 1000);
            send_alert_door_status(false, openDuration);
        }
        last_door_state = doorOpen;
    }
    
    // Check water lockout
    if (waterLockout != last_water_lockout) {
        float threshold = 10.0; // Get from your system config if available
        send_alert_water_lockout(waterLockout, level_s, threshold);
        last_water_lockout = waterLockout;
    }
    
  
}

// ========================================
// UPDATED monitor_fire_sectors
// ========================================

static void monitor_fire_sectors(void) {
    if (!ALERT_SYSTEM_ENABLED) return;
    
    float sensor_values[4] = {0, 0, 0, 0};  // Initialize to zero
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
        // Failed to get mutex, skip this cycle
        printf("[FIRE] Warning: Could not get sensor mutex\n");
        return;
    }
    
    // Update fire detection info (this calculates fire type)
    update_fire_detection_info();
    FireDetectionInfo* fireInfo = get_fire_detection_info();
    
    // Check for fires
    for (int i = 0; i < 4; i++) {
        fire_detected[i] = (sensor_values[i] > FIRE_THRESHOLD);
        if (fire_detected[i]) {
            current_fire_count++;
        }
    }
    // Log fire type changes
    static FireDetectionType last_fire_type = FIRE_TYPE_NONE;
    if (fireInfo->type != last_fire_type) {
        printf("[FIRE] Detection type changed: %s -> %s (sectors: %s)\n",
               get_fire_detection_type_string(last_fire_type),
               get_fire_detection_type_string(fireInfo->type),
               fireInfo->activeSectorNames[0] ? fireInfo->activeSectorNames : "none");
        last_fire_type = fireInfo->type;
    }
    
    // Check individual sector fire alerts
    for (int i = 0; i < 4; i++) {
        if (fire_detected[i] && !fire_alerts_active[i]) {
            // New fire detected in this sector
            fire_sector_t sector = get_sector_from_index(i);
            const char* sectorName = get_sector_name_string(sector);
            bool pumpActivated = (pumps[i].state == PUMP_AUTO_ACTIVE);
            
            send_alert_fire_detected(i, sectorName, sensor_values[i], pumpActivated);
            fire_alerts_active[i] = true;
        }
        else if (!fire_detected[i] && fire_alerts_active[i]) {
            // Fire cleared in this sector
            fire_sector_t sector = get_sector_from_index(i);
            const char* sectorName = get_sector_name_string(sector);
            
            
            send_alert_fire_cleared(i, sectorName, sensor_values[i]);
            fire_alerts_active[i] = false;
        }
    }
    
    // Check for multiple fires
    static int last_fire_count = 0;
    if (current_fire_count > 1 && current_fire_count != last_fire_count) {
        bool pumpStates[4] = {
            pumps[0].state == PUMP_AUTO_ACTIVE,
            pumps[1].state == PUMP_AUTO_ACTIVE,
            pumps[2].state == PUMP_AUTO_ACTIVE,
            pumps[3].state == PUMP_AUTO_ACTIVE
        };
        
        send_alert_multiple_fires(current_fire_count, sensor_values, pumpStates);
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
    static TickType_t manual_start_time = 0;
    bool current_manual_override = false;
    
    // Check if any pump is in manual mode
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
            current_manual_override = true;
            break;
        }
    }
    
    if (current_manual_override && !manual_override_active) {
        manual_start_time = xTaskGetTickCount();
        send_alert_manual_override(true, 0);
        manual_override_active = true;
    }
    else if (!current_manual_override && manual_override_active) {
        int manualDuration = (int)((xTaskGetTickCount() - manual_start_time) * portTICK_PERIOD_MS / 1000);
        send_alert_manual_override(false, manualDuration);
        manual_override_active = false;
    }
    
    // Check for auto activations
    static bool auto_activation_reported = false;
    bool auto_active = false;
    
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_AUTO_ACTIVE) {
            auto_active = true;
            break;
        }
    }
    
    if (auto_active && !auto_activation_reported) {
        send_alert_auto_activation();
        auto_activation_reported = true;
    }
    else if (!auto_active && auto_activation_reported) {
        auto_activation_reported = false;
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
        cJSON_AddStringToObject(payload, "alertType", get_alert_type_string(alert.type));
        cJSON_AddStringToObject(payload, "severity", get_severity_string(alert.severity));
        cJSON_AddStringToObject(payload, "message", alert.message);
        
        // Add type-specific payload fields
        switch(alert.type) {
            
            // ==========================================
            // ALERT #1: PROFILE CHANGE
            // ==========================================
            case ALERT_TYPE_PROFILE_CHANGE:
                cJSON_AddNumberToObject(payload, "previousProfile", alert.data.profile.previousProfile);
                cJSON_AddNumberToObject(payload, "currentProfile", alert.data.profile.currentProfile);
                cJSON_AddStringToObject(payload, "profileName", alert.data.profile.profileName);
                break;
            
            // ==========================================
            // ALERT #2-3: EMERGENCY STOP
            // ==========================================
            case ALERT_TYPE_EMERGENCY_STOP:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.emergencyStop.activated ? "ACTIVATED" : "DEACTIVATED");
                
                if (alert.data.emergencyStop.activated) {
                    cJSON_AddBoolToObject(payload, "allPumpsStopped", true);
                    
                    // Add affected pumps array
                    cJSON *affectedPumps = cJSON_CreateArray();
                    for (int i = 0; i < alert.data.emergencyStop.affectedPumpCount; i++) {
                        cJSON *pump = cJSON_CreateObject();
                        cJSON_AddNumberToObject(pump, "pumpId", 
                            alert.data.emergencyStop.affectedPumps[i].pumpId);
                        cJSON_AddStringToObject(pump, "pumpName", 
                            alert.data.emergencyStop.affectedPumps[i].pumpName);
                        cJSON_AddStringToObject(pump, "previousState", 
                            get_pump_state_string_for_alert(
                                alert.data.emergencyStop.affectedPumps[i].previousState));
                        cJSON_AddItemToArray(affectedPumps, pump);
                    }
                    cJSON_AddItemToObject(payload, "affectedPumps", affectedPumps);
                } else {
                    cJSON_AddStringToObject(payload, "systemStatus", "OPERATIONAL");
                }
                break;
            
            // ==========================================
            // ALERT #3: SYSTEM RESET
            // ==========================================
            case ALERT_TYPE_SYSTEM_RESET:
                cJSON_AddStringToObject(payload, "resetType", alert.data.systemReset.resetType);
                cJSON_AddStringToObject(payload, "defaultProfile", alert.data.systemReset.defaultProfile);
                cJSON_AddBoolToObject(payload, "allPumpsReset", alert.data.systemReset.allPumpsReset);
                cJSON_AddBoolToObject(payload, "emergencyStopCleared", 
                    alert.data.systemReset.emergencyStopCleared);
                break;
            
            // ==========================================
            // ALERT #4-5: START ALL PUMPS
            // ==========================================
            case ALERT_TYPE_START_ALL_PUMPS:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.startAllPumps.activated ? "ACTIVATED" : "DEACTIVATED");
                
                if (alert.data.startAllPumps.activated) {
                    cJSON_AddNumberToObject(payload, "duration", alert.data.startAllPumps.duration);
                    
                    // Add activated pumps array
                    cJSON *activatedPumps = cJSON_CreateArray();
                    for (int i = 0; i < 4; i++) {
                        cJSON *pump = cJSON_CreateObject();
                        cJSON_AddNumberToObject(pump, "pumpId", i + 1);
                        
                        // Get pump name
                        char pumpName[16];
                        switch(i) {
                            case 0: strcpy(pumpName, "North"); break;
                            case 1: strcpy(pumpName, "South"); break;
                            case 2: strcpy(pumpName, "East"); break;
                            case 3: strcpy(pumpName, "West"); break;
                        }
                        cJSON_AddStringToObject(pump, "pumpName", pumpName);
                        cJSON_AddItemToArray(activatedPumps, pump);
                    }
                    cJSON_AddItemToObject(payload, "activatedPumps", activatedPumps);
                    cJSON_AddBoolToObject(payload, "waterLockout", alert.data.startAllPumps.waterLockout);
                } else {
                    cJSON_AddStringToObject(payload, "reason", alert.data.startAllPumps.reason);
                    cJSON_AddNumberToObject(payload, "totalRuntime", alert.data.startAllPumps.totalRuntime);
                }
                break;
            
            // ==========================================
            // ALERT #6-9: PUMP STATE CHANGE
            // ==========================================
            case ALERT_TYPE_PUMP_STATE_CHANGE:
                cJSON_AddNumberToObject(payload, "pumpId", alert.data.pump.pumpId);
                cJSON_AddStringToObject(payload, "pumpName", alert.data.pump.pumpName);
                cJSON_AddStringToObject(payload, "previousState", 
                    get_pump_state_string_for_alert(alert.data.pump.previousState));
                cJSON_AddStringToObject(payload, "currentState", 
                    get_pump_state_string_for_alert(alert.data.pump.currentState));
                
                // Add state-specific fields
                if (alert.data.pump.currentState == 1) { // AUTO_ACTIVE
                    cJSON_AddStringToObject(payload, "activationMode", "AUTOMATIC");
                    if (strlen(alert.data.pump.trigger) > 0) {
                        cJSON_AddStringToObject(payload, "trigger", alert.data.pump.trigger);
                    }
                    if (alert.data.pump.sensorTemperature > 0) {
                        cJSON_AddNumberToObject(payload, "sensorTemperature", alert.data.pump.sensorTemperature);
                    }
                } else if (alert.data.pump.currentState == 2) { // MANUAL_ACTIVE
                    cJSON_AddStringToObject(payload, "activationMode", "MANUAL");
                    if (strlen(alert.data.pump.activationSource) > 0) {
                        cJSON_AddStringToObject(payload, "activationSource", alert.data.pump.activationSource);
                    }
                } else if (alert.data.pump.currentState == 0) { // OFF
                    if (strlen(alert.data.pump.stopReason) > 0) {
                        cJSON_AddStringToObject(payload, "stopReason", alert.data.pump.stopReason);
                    }
                    if (alert.data.pump.totalRuntime > 0) {
                        cJSON_AddNumberToObject(payload, "totalRuntime", alert.data.pump.totalRuntime);
                    }
                } else if (alert.data.pump.currentState == 3) { // COOLDOWN
                    cJSON_AddNumberToObject(payload, "cooldownDuration", alert.data.pump.cooldownDuration);
                    if (alert.data.pump.previousRuntime > 0) {
                        cJSON_AddNumberToObject(payload, "previousRuntime", alert.data.pump.previousRuntime);
                    }
                }
                break;
            
            // ==========================================
            // ALERT #10: PUMP TIMER EXTENSION
            // ==========================================
            case ALERT_TYPE_PUMP_EXTEND_TIME:
                cJSON_AddNumberToObject(payload, "pumpId", alert.data.pumpExtend.pumpId);
                cJSON_AddStringToObject(payload, "pumpName", alert.data.pumpExtend.pumpName);
                cJSON_AddNumberToObject(payload, "extensionCode", alert.data.pumpExtend.extensionCode);
                cJSON_AddNumberToObject(payload, "extensionDuration", alert.data.pumpExtend.extensionDuration);
                cJSON_AddNumberToObject(payload, "newTotalRuntime", alert.data.pumpExtend.newTotalRuntime);
                break;
            
            // ==========================================
            // ALERT #11: FIRE DETECTED
            // ==========================================
            case ALERT_TYPE_FIRE_DETECTED:
                cJSON_AddStringToObject(payload, "sector", alert.data.fire.sector);
                cJSON_AddNumberToObject(payload, "sensorId", alert.data.fire.sensorId);
                cJSON_AddNumberToObject(payload, "temperature", alert.data.fire.temperature);
                cJSON_AddNumberToObject(payload, "threshold", alert.data.fire.threshold);
                cJSON_AddBoolToObject(payload, "pumpActivated", alert.data.fire.pumpActivated);
                if (alert.data.fire.pumpActivated) {
                    cJSON_AddNumberToObject(payload, "pumpId", alert.data.fire.pumpId);
                    cJSON_AddStringToObject(payload, "pumpName", alert.data.fire.pumpName);
                }
                break;
            
            // ==========================================
            // ALERT #12: FIRE CLEARED
            // ==========================================
            case ALERT_TYPE_FIRE_CLEARED:
                cJSON_AddStringToObject(payload, "sector", alert.data.fire.sector);
                cJSON_AddNumberToObject(payload, "sensorId", alert.data.fire.sensorId);
                cJSON_AddNumberToObject(payload, "currentTemperature", alert.data.fire.currentTemperature);
                if (alert.data.fire.duration > 0) {
                    cJSON_AddNumberToObject(payload, "duration", alert.data.fire.duration);
                }
                break;
            
            // ==========================================
            // ALERT #13: MULTIPLE FIRES
            // ==========================================
            case ALERT_TYPE_MULTIPLE_FIRES:
                cJSON_AddNumberToObject(payload, "activeFireCount", 
                    alert.data.multipleFires.activeFireCount);
                
                // Add affected sectors array
                cJSON *affectedSectors = cJSON_CreateArray();
                for (int i = 0; i < alert.data.multipleFires.activeFireCount && i < 4; i++) {
                    cJSON *sector = cJSON_CreateObject();
                    cJSON_AddStringToObject(sector, "sector", 
                        alert.data.multipleFires.affectedSectors[i].sector);
                    cJSON_AddNumberToObject(sector, "temperature", 
                        alert.data.multipleFires.affectedSectors[i].temperature);
                    cJSON_AddBoolToObject(sector, "pumpActive", 
                        alert.data.multipleFires.affectedSectors[i].pumpActive);
                    cJSON_AddItemToArray(affectedSectors, sector);
                }
                cJSON_AddItemToObject(payload, "affectedSectors", affectedSectors);
                
                cJSON_AddNumberToObject(payload, "waterLevel", alert.data.multipleFires.waterLevel);
                cJSON_AddNumberToObject(payload, "estimatedRuntime", 
                    alert.data.multipleFires.estimatedRuntime);
                break;
            
            // ==========================================
            // ALERT #14-15: WATER LOCKOUT
            // ==========================================
            case ALERT_TYPE_WATER_LOCKOUT:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.waterLockout.activated ? "ACTIVATED" : "DEACTIVATED");
                cJSON_AddNumberToObject(payload, "currentWaterLevel", 
                    alert.data.waterLockout.currentWaterLevel);
                
                if (alert.data.waterLockout.activated) {
                    cJSON_AddNumberToObject(payload, "minThreshold", alert.data.waterLockout.minThreshold);
                    cJSON_AddBoolToObject(payload, "allPumpsDisabled", 
                        alert.data.waterLockout.allPumpsDisabled);
                    cJSON_AddBoolToObject(payload, "continuousFeedActive", 
                        alert.data.waterLockout.continuousFeedActive);
                } else {
                    cJSON_AddStringToObject(payload, "systemStatus", alert.data.waterLockout.systemStatus);
                }
                break;
            
            // ==========================================
            // ALERT #16-17:DOOR STATUS
            // ==========================================
            case ALERT_TYPE_DOOR_STATUS:
                cJSON_AddStringToObject(payload, "action", alert.data.door.action);
                cJSON_AddBoolToObject(payload, "doorState", alert.data.door.doorState);
                
                if (alert.data.door.opened) {
                    cJSON_AddBoolToObject(payload, "securityConcern", alert.data.door.securityConcern);
                } else {
                    if (alert.data.door.wasOpenDuration > 0) {
                        cJSON_AddNumberToObject(payload, "wasOpenDuration", 
                            alert.data.door.wasOpenDuration);
                    }
                }
                break;
            
            // ==========================================
            // ALERT #18-19: MANUAL OVERRIDE
            // ==========================================
            case ALERT_TYPE_MANUAL_OVERRIDE:
                cJSON_AddStringToObject(payload, "action", alert.data.manualOverride.action);
                
                if (alert.data.manualOverride.activated) {
                    // Add manual pumps array
                    cJSON *manualPumps = cJSON_CreateArray();
                    for (int i = 0; i < alert.data.manualOverride.manualPumpCount; i++) {
                        cJSON *pump = cJSON_CreateObject();
                        cJSON_AddNumberToObject(pump, "pumpId", 
                            alert.data.manualOverride.manualPumps[i].pumpId);
                        cJSON_AddStringToObject(pump, "pumpName", 
                            alert.data.manualOverride.manualPumps[i].pumpName);
                        cJSON_AddStringToObject(pump, "state", 
                            alert.data.manualOverride.manualPumps[i].state);
                        cJSON_AddItemToArray(manualPumps, pump);
                    }
                    cJSON_AddItemToObject(payload, "manualPumps", manualPumps);
                    
                    cJSON_AddBoolToObject(payload, "autoProtectionDisabled", 
                        alert.data.manualOverride.autoProtectionDisabled);
                    if (strlen(alert.data.manualOverride.activationSource) > 0) {
                        cJSON_AddStringToObject(payload, "activationSource", 
                            alert.data.manualOverride.activationSource);
                    }
                } else {
                    cJSON_AddStringToObject(payload, "systemMode", alert.data.manualOverride.systemMode);
                    cJSON_AddBoolToObject(payload, "autoProtectionEnabled", 
                        alert.data.manualOverride.autoProtectionEnabled);
                    if (alert.data.manualOverride.totalManualDuration > 0) {
                        cJSON_AddNumberToObject(payload, "totalManualDuration", 
                            alert.data.manualOverride.totalManualDuration);
                    }
                }
                break;
            
            // ==========================================
            // ALERT #20: AUTO ACTIVATION
            // ==========================================
            case ALERT_TYPE_AUTO_ACTIVATION:
                cJSON_AddStringToObject(payload, "trigger", alert.data.autoActivation.trigger);
                
                // Add activated pumps array
                cJSON *autoActivatedPumps = cJSON_CreateArray();
                for (int i = 0; i < alert.data.autoActivation.activatedPumpCount; i++) {
                    cJSON *pump = cJSON_CreateObject();
                    cJSON_AddNumberToObject(pump, "pumpId", 
                        alert.data.autoActivation.activatedPumps[i].pumpId);
                    cJSON_AddStringToObject(pump, "pumpName", 
                        alert.data.autoActivation.activatedPumps[i].pumpName);
                    cJSON_AddStringToObject(pump, "sector", 
                        alert.data.autoActivation.activatedPumps[i].sector);
                    cJSON_AddNumberToObject(pump, "temperature", 
                        alert.data.autoActivation.activatedPumps[i].temperature);
                    cJSON_AddStringToObject(pump, "state", 
                        alert.data.autoActivation.activatedPumps[i].state);
                    cJSON_AddItemToArray(autoActivatedPumps, pump);
                }
                cJSON_AddItemToObject(payload, "activatedPumps", autoActivatedPumps);
                
                cJSON_AddStringToObject(payload, "currentProfile", 
                    alert.data.autoActivation.currentProfile);
                cJSON_AddNumberToObject(payload, "waterLevel", alert.data.autoActivation.waterLevel);
                cJSON_AddNumberToObject(payload, "estimatedRuntime", 
                    alert.data.autoActivation.estimatedRuntime);
                break;
           
            
            // ==========================================
            // ALERT #21-22: WIFI UPDATE
            // ==========================================
            case ALERT_TYPE_WIFI_UPDATE:
                cJSON_AddStringToObject(payload, "action", alert.data.wifi.action);
                
                if (strcmp(alert.data.wifi.action, "CREDENTIALS_UPDATED") == 0) {
                    cJSON_AddStringToObject(payload, "newSSID", alert.data.wifi.newSSID);
                    if (strlen(alert.data.wifi.previousSSID) > 0) {
                        cJSON_AddStringToObject(payload, "previousSSID", alert.data.wifi.previousSSID);
                    }
                    cJSON_AddBoolToObject(payload, "requiresReboot", alert.data.wifi.requiresReboot);
                    cJSON_AddBoolToObject(payload, "stored", alert.data.wifi.stored);
                } else {
                    // Invalid credentials
                    cJSON_AddStringToObject(payload, "errorType", alert.data.wifi.errorType);
                    cJSON_AddStringToObject(payload, "errorCode", alert.data.wifi.errorCode);
                    
                    cJSON *details = cJSON_CreateObject();
                    cJSON_AddNumberToObject(details, "ssidLength", alert.data.wifi.ssidLength);
                    cJSON_AddNumberToObject(details, "passwordLength", alert.data.wifi.passwordLength);
                    cJSON_AddStringToObject(details, "reason", alert.data.wifi.reason);
                    cJSON_AddItemToObject(payload, "details", details);
                }
                break;
            
            // ==========================================
            // ALERT #23: SENSOR FAULT
            // ==========================================
            case ALERT_TYPE_SENSOR_FAULT:
                cJSON_AddStringToObject(payload, "sensorType", alert.data.sensorFault.sensorType);
                cJSON_AddNumberToObject(payload, "sensorId", alert.data.sensorFault.sensorId);
                cJSON_AddStringToObject(payload, "sectorAffected", alert.data.sensorFault.sectorAffected);
                cJSON_AddStringToObject(payload, "errorCode", alert.data.sensorFault.errorCode);
                cJSON_AddNumberToObject(payload, "lastValidReading", 
                    alert.data.sensorFault.lastValidReading);
                break;
            
          
            // ==========================================
            // ALERT #24: SYSTEM ERROR (GENERIC)
            // ==========================================
            case ALERT_TYPE_SYSTEM_ERROR:
                cJSON_AddStringToObject(payload, "errorType", alert.data.systemError.errorType);
                cJSON_AddStringToObject(payload, "errorCode", alert.data.systemError.errorCode);
                if (strlen(alert.data.systemError.details) > 0) {
                    cJSON_AddStringToObject(payload, "details", alert.data.systemError.details);
                }
                break;
            
            // ==========================================
            // ALERT #25: CONTINUOUS FEED
            // ==========================================
            case ALERT_TYPE_CONTINUOUS_FEED:
                cJSON_AddStringToObject(payload, "action", 
                    alert.data.continuousFeed.activated ? "ACTIVATED" : "DEACTIVATED");
                cJSON_AddStringToObject(payload, "profile", alert.data.continuousFeed.profile);
                cJSON_AddBoolToObject(payload, "waterLockoutDisabled", 
                    alert.data.continuousFeed.waterLockoutDisabled);
                cJSON_AddBoolToObject(payload, "unlimitedWaterSupply", 
                    alert.data.continuousFeed.unlimitedWaterSupply);
                break;
            
            // ==========================================
            // ALERT #26: HARDWARE FAULT ALERTS
            // ==========================================
            case ALERT_TYPE_PCA9555_FAIL:
                cJSON_AddStringToObject(payload, "hardwareType", 
                    alert.data.hardwareFault.hardwareType);
                cJSON_AddNumberToObject(payload, "componentId", 
                    alert.data.hardwareFault.componentId);
                cJSON_AddStringToObject(payload, "errorCode", 
                    alert.data.hardwareFault.errorCode);
                cJSON_AddStringToObject(payload, "errorMessage", 
                    alert.data.hardwareFault.errorMessage);
                cJSON_AddBoolToObject(payload, "systemCritical", 
                    alert.data.hardwareFault.systemCritical);
                cJSON_AddNumberToObject(payload, "affectedPumpCount", 
                    alert.data.hardwareFault.affectedPumpCount);
                cJSON_AddStringToObject(payload, "affectedPumps", 
                    alert.data.hardwareFault.affectedPumps);
                break;
            
            case ALERT_TYPE_HARDWARE_CONTROL_FAIL:
            case ALERT_TYPE_ADC_INIT_FAIL:
            case ALERT_TYPE_CURRENT_SENSOR_FAULT:
            case ALERT_TYPE_IR_SENSOR_FAULT:
                // Same structure as PCA9555_FAIL
                cJSON_AddStringToObject(payload, "hardwareType", 
                    alert.data.hardwareFault.hardwareType);
                cJSON_AddNumberToObject(payload, "componentId", 
                    alert.data.hardwareFault.componentId);
                cJSON_AddStringToObject(payload, "errorCode", 
                    alert.data.hardwareFault.errorCode);
                cJSON_AddStringToObject(payload, "errorMessage", 
                    alert.data.hardwareFault.errorMessage);
                cJSON_AddBoolToObject(payload, "systemCritical", 
                    alert.data.hardwareFault.systemCritical);
                if (alert.data.hardwareFault.affectedPumpCount > 0) {
                    cJSON_AddNumberToObject(payload, "affectedPumpCount", 
                        alert.data.hardwareFault.affectedPumpCount);
                    cJSON_AddStringToObject(payload, "affectedPumps", 
                        alert.data.hardwareFault.affectedPumps);
                }
                break;
            
            // ==========================================
            // ALERT #27: POWER ALERTS
            // ==========================================
            case ALERT_TYPE_BATTERY_CRITICAL:
            case ALERT_TYPE_BATTERY_LOW:
            case ALERT_TYPE_SOLAR_FAULT:
                cJSON_AddNumberToObject(payload, "batteryVoltage", 
                    alert.data.powerStatus.batteryVoltage);
                cJSON_AddNumberToObject(payload, "solarVoltage", 
                    alert.data.powerStatus.solarVoltage);
                cJSON_AddNumberToObject(payload, "threshold", 
                    alert.data.powerStatus.threshold);
                cJSON_AddStringToObject(payload, "powerState", 
                    alert.data.powerStatus.powerState);
                if (alert.data.powerStatus.estimatedRuntime > 0) {
                    cJSON_AddNumberToObject(payload, "estimatedRuntime", 
                        alert.data.powerStatus.estimatedRuntime);
                }
                cJSON_AddBoolToObject(payload, "chargingActive", 
                    alert.data.powerStatus.chargingActive);
                break;
            
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
                break;
            
            default:
                printf("\n[ALERT] Unknown alert type: %d", alert.type);
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
        if (alert->severity < ALERT_SEVERITY_CRITICAL) {
            printf("\n[ALERT] Blocked alert - Sensors warming up (%u/%d sec)",
       (unsigned int)seconds_since_boot, SENSOR_WARMUP_SECONDS);
            return false;
        }
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
/**
 * @brief Monitor battery and solar voltage
 * Call this from task_sensor_reading() periodically
 */
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
    alert.type = ALERT_TYPE_EMERGENCY_STOP;
    alert.severity = ALERT_SEVERITY_CRITICAL;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    strcpy(alert.message, "EMERGENCY STOP ACTIVATED - All pumps stopped immediately");
    
    alert.data.emergencyStop.activated = true;
    alert.data.emergencyStop.affectedPumpCount = 0;
    
    // Capture pump states before emergency stop
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state != PUMP_OFF) {
            int idx = alert.data.emergencyStop.affectedPumpCount++;
            alert.data.emergencyStop.affectedPumps[idx].pumpId = i + 1;
            strncpy(alert.data.emergencyStop.affectedPumps[idx].pumpName, 
                   pumps[i].name, 15);
            alert.data.emergencyStop.affectedPumps[idx].previousState = pumps[i].state;
        }
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #3: EMERGENCY STOP DEACTIVATED
// ==========================================
static void send_alert_emergency_stop_deactivated(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_EMERGENCY_STOP;
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
    
    strcpy(alert.message, "START ALL PUMPS ACTIVATED - All 4 pumps activated for 90 seconds");
    
    alert.data.startAllPumps.activated = true;
    alert.data.startAllPumps.duration = 90;
    alert.data.startAllPumps.activatedPumpCount = 4;
    alert.data.startAllPumps.waterLockout = waterLockout;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #6: START ALL PUMPS DEACTIVATED
// ==========================================
static void send_alert_start_all_pumps_deactivated(const char* reason, int totalRuntime) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_START_ALL_PUMPS;
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
// ALERT #7-10: PUMP STATE CHANGE
// ==========================================
static void send_alert_pump_state_change(int pumpIndex, int previousState, int currentState,
                                        const char* activationSource, const char* trigger,
                                        float sensorTemp, const char* stopReason,
                                        int runtime, int cooldownDuration) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_PUMP_STATE_CHANGE;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    // Determine severity based on state
    if (currentState == 1) { // AUTO_ACTIVE
        alert.severity = ALERT_SEVERITY_CRITICAL;
    } else if (currentState == 2) { // MANUAL_ACTIVE
        alert.severity = ALERT_SEVERITY_WARNING;
    } else {
        alert.severity = ALERT_SEVERITY_INFO;
    }
    
    // Create message
    const char* stateStr = get_pump_state_string_for_alert(currentState);
    snprintf(alert.message, sizeof(alert.message),
            "Pump %d (%s) changed to %s", pumpIndex + 1, pumps[pumpIndex].name, stateStr);
    
    // Fill data
    alert.data.pump.pumpId = pumpIndex + 1;
    strncpy(alert.data.pump.pumpName, pumps[pumpIndex].name, sizeof(alert.data.pump.pumpName) - 1);
    alert.data.pump.previousState = previousState;
    alert.data.pump.currentState = currentState;
    
    if (activationSource) {
        strncpy(alert.data.pump.activationSource, activationSource, 
               sizeof(alert.data.pump.activationSource) - 1);
    }
    if (trigger) {
        strncpy(alert.data.pump.trigger, trigger, sizeof(alert.data.pump.trigger) - 1);
    }
    if (stopReason) {
        strncpy(alert.data.pump.stopReason, stopReason, sizeof(alert.data.pump.stopReason) - 1);
    }
    
    alert.data.pump.sensorTemperature = sensorTemp;
    alert.data.pump.totalRuntime = runtime;
    alert.data.pump.cooldownDuration = cooldownDuration;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #11: PUMP TIMER EXTENSION
// ==========================================
static void send_alert_pump_extend_time(int pumpIndex, int extensionCode, 
                                       int extensionDuration, int newTotalRuntime) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_PUMP_EXTEND_TIME;
    alert.severity = ALERT_SEVERITY_INFO;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    snprintf(alert.message, sizeof(alert.message),
            "Extended %s by %d seconds", pumps[pumpIndex].name, extensionDuration);
    
    alert.data.pumpExtend.pumpId = pumpIndex + 1;
    strncpy(alert.data.pumpExtend.pumpName, pumps[pumpIndex].name, 
           sizeof(alert.data.pumpExtend.pumpName) - 1);
    alert.data.pumpExtend.extensionCode = extensionCode;
    alert.data.pumpExtend.extensionDuration = extensionDuration;
    alert.data.pumpExtend.newTotalRuntime = newTotalRuntime;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #12: FIRE DETECTED
// ==========================================
static void send_alert_fire_detected(int sensorIndex, const char* sectorName, 
                                    float temperature, bool pumpActivated) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_FIRE_DETECTED;
    alert.severity = ALERT_SEVERITY_EMERGENCY;
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    // Get fire detection type info
    update_fire_detection_info();
    FireDetectionInfo* fireInfo = get_fire_detection_info();
    
    snprintf(alert.message, sizeof(alert.message),
            "FIRE DETECTED in %s sector | Temp: %.1f°C | Type: %s", 
            sectorName, temperature, get_fire_detection_type_string(fireInfo->type));
    
    strncpy(alert.data.fire.sector, sectorName, sizeof(alert.data.fire.sector) - 1);
    alert.data.fire.sensorId = sensorIndex + 1;
    alert.data.fire.temperature = temperature;
    alert.data.fire.threshold = FIRE_THRESHOLD;
    alert.data.fire.pumpActivated = pumpActivated;
    
    // Add fire type info
    alert.data.fire.fireType = (int)fireInfo->type;
    strncpy(alert.data.fire.fireTypeString, get_fire_detection_type_string(fireInfo->type),
            sizeof(alert.data.fire.fireTypeString) - 1);
    alert.data.fire.totalActiveSectors = fireInfo->activeSectorCount;
    strncpy(alert.data.fire.allActiveSectors, fireInfo->activeSectorNames,
            sizeof(alert.data.fire.allActiveSectors) - 1);
    
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
    
    // Determine fire type based on count
    FireDetectionType fireType = (fireCount == 4) ? FIRE_TYPE_FULL_SYSTEM : FIRE_TYPE_MULTIPLE_SECTORS;
    const char* fireTypeStr = (fireCount == 4) ? "FULL_SYSTEM" : "MULTIPLE_SECTORS";
    
    snprintf(alert.message, sizeof(alert.message),
            "MULTIPLE FIRES DETECTED! %d active fire sectors | Type: %s", 
            fireCount, fireTypeStr);
    
    alert.data.multipleFires.activeFireCount = fireCount;
    
    // Add fire type info
    alert.data.multipleFires.fireType = (int)fireType;
    strncpy(alert.data.multipleFires.fireTypeString, fireTypeStr,
            sizeof(alert.data.multipleFires.fireTypeString) - 1);
    
    const char* sectorNames[4] = {"NORTH", "SOUTH", "EAST", "WEST"};
    int sectorIdx = 0;
    for (int i = 0; i < 4 && sectorIdx < fireCount; i++) {
        if (sensorValues[i] > FIRE_THRESHOLD) {
            strncpy(alert.data.multipleFires.affectedSectors[sectorIdx].sector,
                   sectorNames[i], 15);
            alert.data.multipleFires.affectedSectors[sectorIdx].temperature = sensorValues[i];
            alert.data.multipleFires.affectedSectors[sectorIdx].pumpActive = pumpStates[i];
            sectorIdx++;
        }
    }
    
    alert.data.multipleFires.waterLevel = level_s;
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #15-16: WATER LOCKOUT
// ==========================================
static void send_alert_water_lockout(bool activated, float currentLevel, float threshold) {
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
        alert.data.waterLockout.minThreshold = threshold;
        alert.data.waterLockout.allPumpsDisabled = true;
        alert.data.waterLockout.continuousFeedActive = continuousWaterFeed;
    } else {
        strcpy(alert.data.waterLockout.systemStatus, "OPERATIONAL");
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #17-18: DOOR STATUS
// ==========================================
static void send_alert_door_status(bool opened, int openDuration) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_DOOR_STATUS;
    alert.severity = opened ? ALERT_SEVERITY_WARNING : ALERT_SEVERITY_INFO;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    if (opened) {
        strcpy(alert.message, "Door OPENED");
        strcpy(alert.data.door.action, "OPENED");
        alert.data.door.opened = true;
        alert.data.door.doorState = true;
        alert.data.door.securityConcern = true;
    } else {
        snprintf(alert.message, sizeof(alert.message),
                "Door CLOSED - Was open for %d seconds", openDuration);
        strcpy(alert.data.door.action, "CLOSED");
        alert.data.door.opened = false;
        alert.data.door.doorState = false;
        alert.data.door.wasOpenDuration = openDuration;
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #19-20: MANUAL OVERRIDE
// ==========================================
static void send_alert_manual_override(bool activated, int manualDuration) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_MANUAL_OVERRIDE;
    alert.severity = activated ? ALERT_SEVERITY_WARNING : ALERT_SEVERITY_INFO;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    if (activated) {
        strcpy(alert.message, "MANUAL OVERRIDE ACTIVATED - System under manual control");
        strcpy(alert.data.manualOverride.action, "ACTIVATED");
        alert.data.manualOverride.activated = true;
        alert.data.manualOverride.autoProtectionDisabled = true;
        strcpy(alert.data.manualOverride.activationSource, "USER");
        
        // Count manual pumps
        alert.data.manualOverride.manualPumpCount = 0;
        for (int i = 0; i < 4; i++) {
            if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
                int idx = alert.data.manualOverride.manualPumpCount++;
                alert.data.manualOverride.manualPumps[idx].pumpId = i + 1;
                strncpy(alert.data.manualOverride.manualPumps[idx].pumpName,
                       pumps[i].name, 15);
                strcpy(alert.data.manualOverride.manualPumps[idx].state, "MANUAL_ACTIVE");
            }
        }
    } else {
        strcpy(alert.message, "Manual override DEACTIVATED - System returning to auto mode");
        strcpy(alert.data.manualOverride.action, "DEACTIVATED");
        alert.data.manualOverride.activated = false;
        strcpy(alert.data.manualOverride.systemMode, "AUTOMATIC");
        alert.data.manualOverride.autoProtectionEnabled = true;
        alert.data.manualOverride.totalManualDuration = manualDuration;
    }
    
    queue_alert(&alert);
}

// ==========================================
// ALERT #21: AUTO ACTIVATION
// ==========================================
static void send_alert_auto_activation(void) {
    Alert alert = {0};
    alert.type = ALERT_TYPE_AUTO_ACTIVATION;
    alert.severity = ALERT_SEVERITY_CRITICAL;
     
    strncpy(alert.timestamp, get_custom_timestamp(), sizeof(alert.timestamp) - 1);
    
    strcpy(alert.message, "AUTO ACTIVATION - Fire suppression system automatically activated");
    strcpy(alert.data.autoActivation.trigger, "FIRE_DETECTED");
    
    // Count auto-activated pumps
    alert.data.autoActivation.activatedPumpCount = 0;
    const char* sectorNames[4] = {"NORTH", "SOUTH", "EAST", "WEST"};
    float sensorValues[4] = {ir_s1, ir_s2, ir_s3, ir_s4};
    
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_AUTO_ACTIVE) {
            int idx = alert.data.autoActivation.activatedPumpCount++;
            alert.data.autoActivation.activatedPumps[idx].pumpId = i + 1;
            strncpy(alert.data.autoActivation.activatedPumps[idx].pumpName, pumps[i].name, 15);
            strncpy(alert.data.autoActivation.activatedPumps[idx].sector, sectorNames[i], 15);
            alert.data.autoActivation.activatedPumps[idx].temperature = sensorValues[i];
            strcpy(alert.data.autoActivation.activatedPumps[idx].state, "AUTO_ACTIVE");
        }
    }
    
    strncpy(alert.data.autoActivation.currentProfile, profiles[currentProfile].name, 63);
    alert.data.autoActivation.waterLevel = level_s;
    
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

/**
 * @brief Alert for PCA9555 I/O expander failure
 * CRITICAL: Without PCA9555, no pumps can be controlled!
 */
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

/**
 * @brief Alert when pump hardware state doesn't match command
 * CRITICAL: Pump thinks it's on but hardware says otherwise!
 */
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

/**
 * @brief Alert for current sensor fault
 * Important for verifying pump operation
 */
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

/**
 * @brief Alert for critical battery voltage
 * System may shut down soon!
 */
void send_alert_battery_critical(float batteryVoltage, int estimatedRuntime) {
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

/**
 * @brief Alert for pump state corruption
 * System detected invalid state value!
 */
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

void task_pump_management(void *parameter) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    // Track previous states to detect changes
    static PumpState prev_states[4] = {PUMP_OFF, PUMP_OFF, PUMP_OFF, PUMP_OFF};
    static bool prev_manual_mode[4] = {false, false, false, false};
    
    for (;;) {
        if (xSemaphoreTake(mutexPumpState, pdMS_TO_TICKS(10)) == pdTRUE) {
            update_pump_states();
            
            // Detect pump state changes
            bool shadow_update_needed = false;
            
            for (int i = 0; i < 4; i++) {
                // Calculate current manualMode state
                bool current_manual_mode = false;
                if (pumps[i].state == PUMP_MANUAL_ACTIVE && !startAllPumpsActive) {
                    if (pumps[i].activationSource == ACTIVATION_SOURCE_SHADOW_SINGLE ||
                        pumps[i].activationSource == ACTIVATION_SOURCE_MANUAL_SINGLE) {
                        current_manual_mode = true;
                    }
                }
                
                // Detect manual mode changes (especially true -> false when timer expires)
                if (current_manual_mode != prev_manual_mode[i]) {
                    printf("\n[PUMP] Pump %d manualMode changed: %s -> %s\n", 
                           i, prev_manual_mode[i] ? "true" : "false",
                           current_manual_mode ? "true" : "false");
                    
                    // Update tracking variable
                    last_shadow_manual_mode[i] = current_manual_mode;
                    prev_manual_mode[i] = current_manual_mode;
                    shadow_update_needed = true;
                }
                
                // Also track general state changes for logging
                if (pumps[i].state != prev_states[i]) {
                    printf("\n[PUMP] Pump %d state changed: %d -> %d\n", 
                           i, prev_states[i], pumps[i].state);
                    prev_states[i] = pumps[i].state;
                }
            }
            
            xSemaphoreGive(mutexPumpState);
            
            // Trigger shadow update if needed
            if (shadow_update_needed) {
                printf("\n[PUMP] Triggering event-driven shadow update\n");
                vTaskDelay(pdMS_TO_TICKS(100));
                update_shadow_state();
            }
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
                    if (xSemaphoreTake(mutexPumpState, portMAX_DELAY) == pdTRUE) {
                        if (xSemaphoreTake(mutexWaterState, pdMS_TO_TICKS(10)) == pdTRUE) {
                            manual_activate_all_pumps();
                            
                            // Set startAllPumps as active for local commands too
                            startAllPumpsActive = true;
                            startAllPumpsActivationTime = xTaskGetTickCount();
                            
                            xSemaphoreGive(mutexWaterState);
                        }
                        xSemaphoreGive(mutexPumpState);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        update_shadow_state();  // Send acknowledgement
                    }
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

/**
 * @brief Get current active network name for logging
 */
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
                printf("\n[STATE] INIT");
                wifi_consecutive_failures = 0;
                current_state = STATE_WIFI_CONNECTING;
                last_state_change = current_time;
                break;
                
            case STATE_WIFI_CONNECTING:
                if (is_wifi_connected()) {
                    printf("\n[STATE] WiFi Connected");
                    
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
                        reconnect_wifi();
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
                        current_state = STATE_REGISTERING;
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
                            if (!try_wifi_reconnection()) {
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
                            if (try_wifi_reconnection()) {
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
                                
                                if (try_wifi_reconnection()) {
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
                        if (try_wifi_reconnection()) {
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
    
    printf("\n=== STATUS REPORT #%d ===\n", display_count);
    printf("Thing: %s | Provisioned: %s\n", thing_name, is_provisioned ? "YES" : "NO");
    printf("MQTT Connected: %s\n", mqtt_connected ? "YES" : "NO");
    
    printf("Time Synced: %s\n", time_manager_is_synced() ? "YES" : "NO");
    char timestamp[32];
    if (time_manager_get_timestamp(timestamp, sizeof(timestamp)) == ESP_OK) {
        printf("Current Time (UTC): %s\n", timestamp);
    }
    
    // UPDATED: Show network status
    printf("\nNETWORK STATUS:\n");
    printf("Active Network: %s\n", get_current_network_name());
    
    printf("\nWIFI STATUS:\n");
    char ip_address[16];
    get_wifi_ip_address(ip_address, sizeof(ip_address));
    printf("Connected: %s | IP: %s | SSID: %s\n",
           is_wifi_connected() ? "YES" : "NO",
           ip_address,
           get_current_wifi_ssid());
    
#if GSM_ENABLED
    printf("\nGSM STATUS:\n");
    printf("Connected: %s | Signal: %d\n",
           gsm_manager_is_connected() ? "YES" : "NO",
           gsm_manager_get_signal_quality());
#endif
    
    // ADDED: Show startAllPumps status
    printf("startAllPumps Active: %s\n", startAllPumpsActive ? "YES" : "NO");
    if (startAllPumpsActive) {
        TickType_t elapsed = xTaskGetTickCount() - startAllPumpsActivationTime;
        printf("  Active for: %u seconds\n", (unsigned int)(elapsed * portTICK_PERIOD_MS / 1000));
    }
    
    const char* profileName = "Unknown";
    if (currentProfile >= WILDLAND_STANDARD && currentProfile <= CONTINUOUS_FEED) {
        profileName = profiles[currentProfile].name;
    }
    
    printf("Current Profile: %d (%s)\n", convert_profile_enum_to_number(currentProfile), profileName);
    printf("Emergency Stop: %s\n", emergencyStopActive ? "ACTIVE" : "INACTIVE");
    printf("Water Lockout: %s\n", waterLockout ? "YES" : "NO");
    printf("Continuous Feed: %s\n", continuousWaterFeed ? "YES" : "NO");
    
    printf("\nPUMP STATUS:\n");
    for (int i = 0; i < 4; i++) {
        const char* state_str = get_pump_state_string(i);
        const char* stop_reason_str = get_stop_reason_string(pumps[i].lastStopReason);
        const char* activation_str = get_activation_source_string(pumps[i].activationSource);
        printf("Pump %d (%s): State=%s, Running=%s, Source=%s, StopReason=%s\n",
               i+1, pumps[i].name, state_str,
               pumps[i].isRunning ? "YES" : "NO",
               activation_str,
               stop_reason_str);
    }
    
    printf("\nSENSOR STATUS:\n");
    printf("Water Level: %.1f%%\n", level_s);
    printf("IR Sensors: N=%.1f%%, S=%.1f%%, E=%.1f%%, W=%.1f%%\n", 
           ir_s1, ir_s2, ir_s3, ir_s4);
    printf("Battery: %.2fV | Solar: %.2fV\n", bat_v, sol_v);
    
    // ADDED: Show fire detection type info
    FireDetectionInfo* fireInfo = get_fire_detection_info();
    printf("\nFIRE DETECTION STATUS:\n");
    printf("Fire Type: %s\n", get_fire_detection_type_string(fireInfo->type));
    printf("Active Sectors: %d (%s)\n", 
           fireInfo->activeSectorCount,
           fireInfo->activeSectorNames[0] ? fireInfo->activeSectorNames : "none");
    
    printf("\nSYSTEM STATUS:\n");
    printf("Suppression Active: %s\n", is_suppression_active() ? "YES" : "NO");
    printf("Door: %s\n", doorOpen ? "OPEN" : "CLOSED");
    if (doorOpen) {
        unsigned long openTime = (xTaskGetTickCount() * portTICK_PERIOD_MS - doorOpenTime) / 1000;
        printf("Door open for: %lu seconds\n", openTime);
    }
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
    esp_log_level_set("*", ESP_LOG_INFO);
    
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
    printf("\n[INIT] Initializing time manager...");
    esp_err_t time_init = time_manager_init();
    if (time_init != ESP_OK) {
        printf("\n[INIT] WARNING: Time manager init failed: %s", esp_err_to_name(time_init));
    } else {
        printf("\n[INIT] Time manager initialized (UTC mode)");
    }
    
    // Initialize SPIFFS
    spiffs_init();
    
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

    
    // Initialize hardware
    init_fire_suppression_system();
    #if GSM_ENABLED
    printf("\n[INIT] ========================================");
    printf("\n[INIT]       INITIALIZING GSM FALLBACK       ");
    printf("\n[INIT] ========================================");
    
    esp_err_t gsm_ret = gsm_manager_init();
    if (gsm_ret == ESP_OK) {
        printf("\n[INIT] GSM manager initialized successfully");
        printf("\n[INIT] GSM fallback: ENABLED and READY");
        
        // Test signal quality
        int signal = gsm_manager_get_signal_quality();
        if (signal > 0 && signal < 99) {
            printf("\n[INIT] GSM signal detected: %d dBm", signal);
        } else {
            printf("\n[INIT] No GSM signal (will retry when needed)");
        }
    } else {
        printf("\n[INIT] GSM manager initialization FAILED");
        printf("\n[INIT] Error: %s", esp_err_to_name(gsm_ret));
        printf("\n[INIT] GSM fallback DISABLED due to init failure");
    }
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

