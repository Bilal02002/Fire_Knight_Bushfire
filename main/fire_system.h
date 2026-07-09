#ifndef FIRE_SYSTEM_H
#define FIRE_SYSTEM_H

#include "clsPCA9555.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// ========================================
// SYSTEM CONSTANTS
// ========================================
#define LOG_BUFFER_SIZE             256
#define LOW_LEVEL_THRESHOLD         20.0
#define AUTO_RESUME_LEVEL           30.0
#define GRACE_PERIOD_TIME           20000
#define FLAME_CONFIRMATION_TIME     2000
#define COOLDOWN_TIME               30000
#define MANUAL_SINGLE_PUMP_TIME     120000  // 2 minutes
#define MANUAL_ALL_PUMPS_TIME       90000   // 90 seconds
#define SENSOR_HEALTH_INTERVAL      300000
#define DOOR_CHECK_INTERVAL         500
#define DOOR_ALERT_DELAY            300000

// ========================================
// 0–20mA CURRENT OUTPUT INTERPRETATION
// Sensor wiring: 150Ω shunt resistor to GND on each Fire S1–S4 line.
// Measured voltage on ADC = I_sensor * 150Ω.
// ir_sX is stored as a percentage of 3.3V full-scale:
//   ir_sX = (adc_voltage / 3.3) * 100.0
// Therefore for a current I (mA):
//   V_shunt = I * 0.150          (Volts across 150Ω)
//   ir_pct  = (V_shunt / 3.3) * 100.0
//
//  0 mA  →  0.00 V  →  0.0 %   FAULT (wiring open / power lost)
//  2 mA  →  0.30 V  →  9.1 %   BIT FAULT (sensor self-test failed)
//  4 mA  →  0.60 V  → 18.2 %   NORMAL (healthy, no fire)
// 16 mA  →  2.40 V  → 72.7 %   AUX BIT / Manual test in progress
// 20 mA  →  3.00 V  → 90.9 %   FIRE ALARM
//
// Bands (with ±1 mA guard-band each side):
#define IR_PCT_FAULT_LOW            0.0f    // 0 mA  – open-circuit fault
#define IR_PCT_FAULT_HIGH           4.5f    // up to ~1 mA  (guard band)
#define IR_PCT_BIT_FAULT_LOW        4.6f    // ~1 mA
#define IR_PCT_BIT_FAULT_HIGH       13.6f   // ~3 mA  (centred on 2 mA / 9.1 %)
#define IR_PCT_NORMAL_LOW           13.7f   // ~3 mA
#define IR_PCT_NORMAL_HIGH          27.3f   // ~6 mA  (centred on 4 mA / 18.2 %)
#define IR_PCT_WARNING_LOW          27.4f   // ~6 mA  – rising toward alarm
#define IR_PCT_WARNING_HIGH         63.6f   // ~14 mA (pre-alarm band)
#define IR_PCT_AUX_BIT_LOW          63.7f   // ~14 mA
#define IR_PCT_AUX_BIT_HIGH         81.8f   // ~18 mA (centred on 16 mA / 72.7 %)
#define IR_PCT_FIRE_THRESHOLD       81.9f   // ≥ ~18 mA → FIRE ALARM  (was 50.0)

// Legacy alias – kept so any code referencing FIRE_THRESHOLD still compiles.
// The new check_automatic_activation() logic uses IR_PCT_FIRE_THRESHOLD directly.
#define FIRE_THRESHOLD              IR_PCT_FIRE_THRESHOLD

// ========================================
// PCA9555 CONFIGURATION
// ========================================
#define PCA9555_I2C_ADDRESS         0x21
#define PCA9555_I2C_PORT            I2C_NUM_0
#define PCA9555_I2C_SDA_GPIO        21
#define PCA9555_I2C_SCL_GPIO        22

// ========================================
// TYPE DEFINITIONS
// ========================================

typedef enum {
    STOP_REASON_START_ALL_NONE = 0,
    STOP_REASON_START_ALL_TIMER_EXPIRED,
    STOP_REASON_START_ALL_EMERGENCY_STOP,
    STOP_REASON_START_ALL_WATER_LOCKOUT
} start_all_pumps_stop_reason_t;
extern start_all_pumps_stop_reason_t startAllPumps_stop_reason;

// External declarations for main.c variables
extern bool startAllPumpsActive;
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
    STOP_REASON_SENSOR_FAULT,
    STOP_REASON_PROFILE_CHANGE
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

// 0–20mA Signal State – decoded from ir_sX percentage value
typedef enum {
    FLAME_SIGNAL_FAULT     = 0,  // 0 mA  – open-circuit / power lost
    FLAME_SIGNAL_BIT_FAULT = 1,  // 2 mA  – sensor self-test failure
    FLAME_SIGNAL_NORMAL    = 2,  // 4 mA  – healthy, no fire
    FLAME_SIGNAL_WARNING   = 3,  // 4–16 mA rising – pre-alarm / warning
    FLAME_SIGNAL_AUX_BIT   = 4,  // 16 mA – manual / auxiliary BIT in progress
    FLAME_SIGNAL_FIRE      = 5   // 20 mA – fire alarm confirmed
} FlameSignalState;

// Fire Detection Info Structure — tracks which sector(s) currently have confirmed fire.
// (Previously classified fires as SINGLE/MULTIPLE/FULL_SYSTEM; that classification has
// been removed in favor of simply tracking which sectors are detected.)
typedef struct {
    bool detectedSectors[4];         // Which sectors currently have confirmed fire [N, S, E, W]
    int  detectedSectorCount;        // Number of sectors with active fire (0-4)
    char detectedSectorNames[64];    // String listing active sectors e.g. "N,S" or "N,S,E,W"
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
    char mode[8];              // "sector" or "full" — runtime-configurable via shadow;
                                // defaults from autoModeFull whenever the profile is applied
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
    
    // Timer protection fields
    bool timerProtected;
    unsigned long timerEndTime;
    unsigned long originalDuration;

    // 🆕 HARDWARE TIMER FIELDS (esp_timer - backed by the ESP32 hardware
    // timer peripheral, NOT a millis()/tick-polled software counter).
    // hwTimerHandle fires hwTimerExpired precisely at expiry via a real
    // hardware alarm/callback; the existing polling loop in
    // task_pump_management() then safely deactivates the pump under its
    // normal mutex-protected flow.
    esp_timer_handle_t hwTimerHandle;
    volatile bool hwTimerExpired;
    
    // Stop pump flag for shadow
    bool stopPumpRequested;
    bool activatedInFullSystemMode;
    int flameConfirmationDurationMs;
    char flameDetectionTime[30];       // wall clock time when flame was first confirmed
    unsigned long activationCapMs;     // profile cap (ms) snapshotted at pump start time

    // 0–20mA decoded state fields
    FlameSignalState flameSignalState; // current decoded current-output state
    int bitFaultCount;                 // consecutive BIT-fault readings (debounce)
    bool inAuxBit;                     // true while a manual/auxiliary BIT is in progress
    unsigned long auxBitStartTime;     // timestamp when AuxBIT was first detected
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

// Inverter Voltages (computed from adc_array2[2..5] * 360)
extern float volt1, volt2, volt3, volt4;

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
extern PumpState savedPumpStates[4];
extern bool savedRunningStates[4];

// ========================================
// FUNCTION DECLARATIONS
// ========================================

extern void send_alert_pca9555_fail(const char*, const char*);
extern void send_alert_hardware_control_fail(int, const char*);
extern void send_alert_current_sensor_fault(int, float);
extern void send_alert_ir_sensor_fault(int, float);
extern void send_alert_state_corruption(int, int);

// 0–20mA Signal Decoding Functions
FlameSignalState decode_flame_signal(float irPct);
const char* get_flame_signal_state_string(FlameSignalState state);
void update_flame_signal_states(void);
bool is_sensor_in_bit_fault(int index);
bool is_sensor_in_aux_bit(int index);

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

// Fire Detection Functions
void update_fire_detection_info(void);
int get_active_fire_sector_count(void);
const char* get_active_sectors_string(void);
bool is_sector_on_fire(int sectorIndex);
FireDetectionInfo* get_fire_detection_info(void);

// Suppression Mode Functions (sector vs full — per active profile, shadow-configurable)
void set_suppression_mode(const char* mode);
const char* get_suppression_mode(void);

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
void manual_activate_all_pumps_with_duration(unsigned long durationMs);
void manual_stop_pump(int index);
void stop_all_pumps(const char* reason);
void extend_manual_runtime(int index, unsigned long extensionTime);

// Shadow-Integrated Manual Control Functions
bool can_activate_pump_manually(int index);
bool shadow_manual_activate_all_pumps(void);
bool shadow_manual_activate_all_pumps_with_duration(unsigned long durationMs);
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