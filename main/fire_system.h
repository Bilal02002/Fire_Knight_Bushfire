#ifndef FIRE_SYSTEM_H
#define FIRE_SYSTEM_H

// ========================================
// INCLUDES
// ========================================
#include "clsPCA9555.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <math.h>
#include <stdbool.h>

// ========================================
// SYSTEM CONSTANTS
// ========================================
#define LOG_BUFFER_SIZE             256
#define LOW_LEVEL_THRESHOLD         20.0
#define AUTO_RESUME_LEVEL           30.0
#define GRACE_PERIOD_TIME           20000
#define WATER_STABILITY_TIME        30000
#define FLAME_CONFIRMATION_TIME     2000
#define COOLDOWN_TIME               30000
#define MANUAL_SINGLE_PUMP_TIME     120000  // 2 minutes
#define MANUAL_ALL_PUMPS_TIME       90000   // 90 seconds
#define SENSOR_HEALTH_INTERVAL      300000
#define DOOR_CHECK_INTERVAL         500
#define DOOR_ALERT_DELAY            300000

// UNIFIED FIRE DETECTION THRESHOLD - Used by all fire detection logic
#define FIRE_THRESHOLD              50.0

// ========================================
// PCA9555 CONFIGURATION
// ========================================
#define PCA9555_I2C_ADDRESS         0x21
#define PCA9555_I2C_PORT            I2C_NUM_0
#define PCA9555_I2C_SDA_GPIO        21
#define PCA9555_I2C_SCL_GPIO        22
#define EXTEND_30S                  (30 * 1000)
#define EXTEND_2MIN                 (120 * 1000)
#define EXTEND_5MIN                 (300 * 1000)
#define EXTEND_10MIN                (600 * 1000)

// ========================================
// TYPE DEFINITIONS
// ========================================

// Pump State Enum
typedef enum {
    PUMP_OFF,
    PUMP_AUTO_ACTIVE,
    PUMP_MANUAL_ACTIVE,
    PUMP_COOLDOWN,
    PUMP_DISABLED
} PumpState;

// System Profile Enum
typedef enum {
    WILDLAND_STANDARD,
    WILDLAND_HIGH_WIND,
    INDUSTRIAL_HYDROCARBON,
    CRITICAL_ASSET,
    CONTINUOUS_FEED
} SystemProfile;

// Command Type Enum
typedef enum {
    CMD_MANUAL_PUMP,
    CMD_MANUAL_ALL_PUMPS,
    CMD_STOP_PUMP,
    CMD_STOP_ALL_PUMPS,
    CMD_EXTEND_TIME,
    CMD_CHANGE_PROFILE,
    CMD_GET_STATUS
} CommandType;

// Stop Reason Enum
typedef enum {
    STOP_REASON_NONE,
    STOP_REASON_MANUAL,
    STOP_REASON_TIMEOUT, 
    STOP_REASON_AUTO_TIMEOUT,
    STOP_REASON_RUN_CAP,
    STOP_REASON_WATER_LOCKOUT,
    STOP_REASON_EMERGENCY_STOP,
    STOP_REASON_SHADOW_COMMAND,
    STOP_REASON_SENSOR_FAULT
} StopReason;

// Activation Source Enum
typedef enum {
    ACTIVATION_SOURCE_NONE,
    ACTIVATION_SOURCE_AUTO,
    ACTIVATION_SOURCE_MANUAL_SINGLE,
    ACTIVATION_SOURCE_MANUAL_ALL,
    ACTIVATION_SOURCE_SHADOW_SINGLE,
    ACTIVATION_SOURCE_SHADOW_ALL
} ActivationSource;

// Fire Detection Type Enum
typedef enum {
    FIRE_TYPE_NONE,              // No fire detected
    FIRE_TYPE_SINGLE_SECTOR,     // Only 1 sector has fire (N, S, E, or W)
    FIRE_TYPE_MULTIPLE_SECTORS,  // 2-3 sectors have fire
    FIRE_TYPE_FULL_SYSTEM        // All 4 sectors have fire
} FireDetectionType;

// Fire Detection Info Structure
typedef struct {
    FireDetectionType type;          // Current fire detection type
    int activeSectorCount;           // Number of sectors with active fire (0-4)
    bool sectorsActive[4];           // Which sectors have fire [N, S, E, W]
    char activeSectorNames[64];      // String listing active sectors e.g. "N,S" or "N,S,E,W"
    unsigned long lastUpdateTime;    // Timestamp of last fire status update
} FireDetectionInfo;

// System Command Structure
typedef struct {
    CommandType type;
    int pumpIndex;
    unsigned long value;
    SystemProfile profileValue;
} SystemCommand;

// Profile Configuration Structure
typedef struct {
    bool autoModeFull;
    unsigned long noFlameTimeout;
    unsigned long maxRunCapFull;
    unsigned long maxRunCapSector;
    const char* name;
    unsigned long cooldown;
} ProfileConfig;

// Pump Control Structure

typedef struct {
    gpio_num_t pin;
    bool sensorFault;
    const char* name;
    PumpState state;
    unsigned long timerDuration;           
    unsigned long protectionTimeRemaining; 
    unsigned long flameFirstDetectedTime;
    bool flameConfirmed;
    unsigned long lastFlameSeenTime;
    unsigned long pumpStartTime;
    unsigned long cooldownStartTime;
    float currentIRValue;
    bool manualMode;
    unsigned long manualStartTime;
    unsigned long manualDuration;
    unsigned long cooldownDuration;  
    bool isRunning;
    
    // Emergency stop fields
    PumpState stateBeforeEmergency;
    bool wasRunningBeforeEmergency;
    unsigned long emergencyStopTime;
    StopReason lastStopReason;
    
    ActivationSource activationSource;
    
    // TIMER PROTECTION FIELDS:
    bool timerProtected;
    unsigned long timerEndTime;
    unsigned long originalDuration;
    
    // 🆕 NEW: Stop pump flag for shadow
    bool stopPumpRequested;
    bool activatedInFullSystemMode;
} PumpControl;


// Current Sensor Structure
typedef struct {
    const char* name;
    gpio_num_t pin;
    bool isMux;
    int muxChannel;
    float currentValue;
    float averageValue;
    bool fault;
    unsigned long lastReadTime;
} CurrentSensor;

// Pump Status Structure for Shadow Reporting
typedef struct {
    int pumpIndex;
    const char* name;
    const char* state;
    bool isRunning;
    bool manualMode;
    unsigned long runningTimeSeconds;
    unsigned long remainingTimeSeconds;
    float irValue;
    bool sensorFault;
} PumpStatusReport;

// ========================================
// EXTERN VARIABLE DECLARATIONS
// ========================================

// Duration code helpers
unsigned long get_duration_from_code(int code);
const char* get_duration_code_string(int code);

// Camera pin
extern const gpio_num_t CAMERA_ON_OFF;
extern const float CAMERA_FIRE_THRESHOLD;

// Sensor Data Arrays
extern float adc_array1[8];
extern float adc_array2[8];
extern float waterLevels[4];

// Main Sensor Values
extern float level_s;
extern float ir_s1, ir_s2, ir_s3, ir_s4;
extern float sol_v, bat_v;

// System State
extern SystemProfile currentProfile;
extern bool systemArmed;
extern bool waterLockout;
extern bool continuousWaterFeed;
extern bool doorOpen;
extern unsigned long doorOpenTime;

// Emergency Stop Variable
extern bool emergencyStopActive;

// Fire Detection Info
extern FireDetectionInfo currentFireInfo;

// Hardware Control
extern pca9555_t pca_dev;

// Arrays
extern PumpControl pumps[4];
extern ProfileConfig profiles[5];
extern CurrentSensor currentSensors[4];

// Timing Variables
extern unsigned long lastSensorHealthCheck;
extern unsigned long waterAboveResumeTime;
extern bool waterStable;
extern unsigned long gracePeriodStartTime;
extern bool inGracePeriod;
extern unsigned long lastDoorCheck;
extern unsigned long lastCurrentReadTime;



// ========================================
// FUNCTION DECLARATIONS
// ========================================


extern void send_alert_pca9555_fail(const char*, const char*);
extern void send_alert_hardware_control_fail(int, const char*);
extern void send_alert_current_sensor_fault(int, float);
extern void send_alert_state_corruption(int, int);

// Initialization Functions
void init_fire_suppression_system(void);
void initialize_arrays(void);
void init_current_sensors(void);
void init_door_sensor(void);

// Main System Functions
void update_fire_suppression_system(void);
void get_sensor_data(void);
void apply_system_profile(SystemProfile newProfile);

// Camera Functions
bool get_camera_status(void);
bool is_camera_active(void);

// Fire Detection & Safety Functions
void check_automatic_activation(void);
void check_water_lockout(void);
void check_sensor_health(void);
bool is_sensor_healthy(int index);

// Fire Detection Type Functions
void update_fire_detection_info(void);
FireDetectionType get_fire_detection_type(void);
int get_active_fire_sector_count(void);
const char* get_fire_detection_type_string(FireDetectionType type);
const char* get_active_sectors_string(void);
bool is_sector_on_fire(int sectorIndex);
FireDetectionInfo* get_fire_detection_info(void);

// Pump Control Functions
void activate_pump(int index, bool activateAll);
void deactivate_pump(int index, const char* reason);
void set_pump_hardware(int index, bool state);
void update_pump_states(void);
void pump_control(unsigned int pumpNum, bool state);
void all_off(void);

// Manual Control Functions
void manual_activate_pump(int index);
void manual_activate_all_pumps(void);
void manual_stop_pump(int index);
void stop_all_pumps(const char* reason);
void extend_manual_runtime(int index, unsigned long extensionTime);

// Shadow-Integrated Manual Control Functions
bool can_activate_pump_manually(int index);
bool shadow_manual_activate_pump(int index);
bool shadow_manual_activate_all_pumps(void);
bool shadow_manual_stop_pump(int index);
bool shadow_manual_stop_all_pumps(void);
void process_shadow_emergency_stop(bool stopCommand);
bool shadow_manual_activate_pump_with_duration(int index, unsigned long durationMs);
// Pump Status Reporting Functions
void get_pump_status_report(int index, PumpStatusReport* report);
void get_all_pumps_status(PumpStatusReport reports[4]);
unsigned long get_pump_running_time(int index);
unsigned long get_pump_remaining_time(int index);

// Emergency Stop Functions
void emergency_stop_all_pumps(StopReason reason);
void restore_pumps_after_emergency(void);
bool is_emergency_stop_active(void);
void set_emergency_stop(bool enable, StopReason reason);
void save_current_pump_states(void);
StopReason get_pump_stop_reason(int index);
const char* get_stop_reason_string(StopReason reason);
const char* get_activation_source_string(ActivationSource source);

// System Reset Functions
void reset_system_to_defaults(void);

// Sensor Functions
void set_mux_channel(int channel);
float measure_current(int adc_channel);
float read_current_sensor(int index);
void read_all_current_sensors(void);
void check_current_sensor_faults(void);
void print_current_sensor_status(void);

// Monitoring & Status Functions
void detect_continuous_feed(void);
void check_door_status(void);
void update_camera_on_off(void);
const char* get_pump_state_string(int index);
bool is_suppression_active(void);

// Event Callback Functions
void on_pump_activated(int index, bool isManual);
void on_pump_deactivated(int index, const char* reason);
void on_water_lockout_activated(void);
void on_water_lockout_released(void);
void on_flame_confirmed(int sensorIndex);

// ========================================
// TIMER PROTECTION FUNCTIONS
// ========================================
void start_timer_protection(int index, unsigned long duration);
bool is_timer_expired(int index);
unsigned long get_timer_remaining(int index);
void extend_timer_protection(int index, unsigned long extensionTime);
void stop_timer_protection(int index);
bool shadow_manual_stop_pump_override_timer(int index);
void process_stop_pump_request(int index);
#endif // FIRE_SYSTEM_H