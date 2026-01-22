#include "fire_system.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

// ========================================
// GLOBAL VARIABLE DEFINITIONS
// ========================================
#define DIODE_DROP 0.3f
#define REVERSE_RATIO 12.11f

// ADC Handles
adc_oneshot_unit_handle_t adc1_handle = NULL;
adc_cali_handle_t adc_cali_handle = NULL;

// Hardware Constants
const float VREF = 3.3;
const int ADC_RES = 4095;
const float BIAS_VOLTAGE = 1.65;
const float R_SHUNT = 33.0;
const float SCALE_RATIO = 0.0005;
const unsigned long SAMPLE_WINDOW = 1000;

// Pin Definitions
const gpio_num_t CAMERA_ON_OFF = GPIO_NUM_32;
const float CAMERA_FIRE_THRESHOLD = 80.0;
const gpio_num_t s0 = GPIO_NUM_25;
const gpio_num_t s1 = GPIO_NUM_26;
const gpio_num_t s2 = GPIO_NUM_27;
const gpio_num_t SENSOR1_PIN = GPIO_NUM_34;
const gpio_num_t SENSOR2_PIN = GPIO_NUM_35;
const gpio_num_t MUX_OUTPUT_PIN = GPIO_NUM_39;
const gpio_num_t DOOR_SENSOR_PIN = GPIO_NUM_15;

// Water Level Configuration
uint8_t waterLevelChannels[4] = {1, 0, 2, 3};

// Variable Definitions
float adc_array1[8] = {0};
float adc_array2[8] = {0};
float waterLevels[4] = {0};

// Main Sensor Values
float level_s = 0;
float ir_s1 = 0, ir_s2 = 0, ir_s3 = 0, ir_s4 = 0;
float sol_v = 0, bat_v = 0;

// System State
SystemProfile currentProfile = WILDLAND_STANDARD;
bool systemArmed = true;
bool waterLockout = false;
bool continuousWaterFeed = false;
bool doorOpen = false;
unsigned long doorOpenTime = 0;

// Emergency Stop Variable
bool emergencyStopActive = false;

// Fire Detection Info
FireDetectionInfo currentFireInfo = {
    .type = FIRE_TYPE_NONE,
    .activeSectorCount = 0,
    .sectorsActive = {false, false, false, false},
    .activeSectorNames = "",
    .lastUpdateTime = 0
};

// Timing Variables
unsigned long lastSensorHealthCheck = 0;
unsigned long waterAboveResumeTime = 0;
bool waterStable = false;
unsigned long gracePeriodStartTime = 0;
float gracePeriodWaterLevel = 0;  // Track water level at grace period start
bool inGracePeriod = false;
unsigned long lastDoorCheck = 0;
unsigned long lastCurrentReadTime = 0;
unsigned long lastContinuousFeedCheck = 0;
float lastWaterLevelForFeed = 0;
int continuousFeedConfidence = 0;

// Arrays
CurrentSensor currentSensors[4];
ProfileConfig profiles[5];
PumpControl pumps[4];

// State preservation for emergency stop
static PumpState savedPumpStates[4] = {PUMP_OFF};
static bool savedRunningStates[4] = {false};
static unsigned long savedManualTimes[4] = {0};
static unsigned long savedManualDurations[4] = {0};

// Flame confirmation tracking
static unsigned long flameStartTime[4] = {0, 0, 0, 0};
static bool flameValidating[4] = {false, false, false, false};

// Water stability tracking
static float lastStableWaterLevel = 0;
static unsigned long stableStartTime = 0;

// PCA9555 Device
pca9555_t pca_dev;

// ========================================
// INITIALIZATION FUNCTIONS
// ========================================

void initialize_arrays(void) {
    printf("[INIT] Initializing system arrays...\n");
    
    // Initialize Current Sensors
    currentSensors[0] = (CurrentSensor){"CT1", MUX_OUTPUT_PIN, true, 6, 0.0, 0.0, false, 0};
    currentSensors[1] = (CurrentSensor){"CT2", MUX_OUTPUT_PIN, true, 7, 0.0, 0.0, false, 0};
    currentSensors[2] = (CurrentSensor){"CT3", SENSOR1_PIN, false, -1, 0.0, 0.0, false, 0};
    currentSensors[3] = (CurrentSensor){"CT4", SENSOR2_PIN, false, -1, 0.0, 0.0, false, 0};
    
    // ========================================
    // CORRECTED PROFILES (Matching Document Section 10)
    // ========================================
    
    // WILDLAND_STANDARD - Section 10.1
    profiles[WILDLAND_STANDARD] = (ProfileConfig){
        .autoModeFull = false,        // Sector mode only
        .noFlameTimeout = 60000,      // 60 seconds NFT
        .maxRunCapFull = 15000,      // 3 minutes (Full system)
        .maxRunCapSector = 20000,    // 6 minutes (Sector)
        .name = "Wildland-Standard",
        .cooldown = 30000
    };

    // WILDLAND_HIGH_WIND - Section 10.2
    profiles[WILDLAND_HIGH_WIND] = (ProfileConfig){
        .autoModeFull = true,         // Full system mode
        .noFlameTimeout = 45000,      // 45 seconds NFT
        .maxRunCapFull = 240000,      // 4 minutes (Full system)
        .maxRunCapSector = 480000,    // 8 minutes (Sector)
        .name = "Wildland-HighWind",
        .cooldown = 30000
    };

    // INDUSTRIAL_HYDROCARBON - Section 10.3
    profiles[INDUSTRIAL_HYDROCARBON] = (ProfileConfig){
        .autoModeFull = false,        // Sector mode only
        .noFlameTimeout = 60000,      // 60 seconds NFT
        .maxRunCapFull = 300000,      // 5 minutes (Full system)
        .maxRunCapSector = 600000,    // 10 minutes (Sector)
        .name = "Industrial-Hydrocarbon",
        .cooldown = 30000
    };

    // CRITICAL_ASSET - Section 10.4
    profiles[CRITICAL_ASSET] = (ProfileConfig){
        .autoModeFull = false,        // Sector mode only
        .noFlameTimeout = 60000,      // 60 seconds NFT
        .maxRunCapFull = 240000,      // 4 minutes (Full system)
        .maxRunCapSector = 480000,    // 8 minutes (Sector)
        .name = "Critical-Asset",
        .cooldown = 30000
    };

    // CONTINUOUS_FEED - Section 10.5
    profiles[CONTINUOUS_FEED] = (ProfileConfig){
        .autoModeFull = false,        // Sector mode only
        .noFlameTimeout = 60000,      // 60 seconds NFT
        .maxRunCapFull = 0,           // NO LIMIT (caps lifted)
        .maxRunCapSector = 0,         // NO LIMIT (caps lifted)
        .name = "Continuous-Feed",
        .cooldown = 0
    };
    
    
    // Initialize Pumps
for (int i = 0; i < 4; i++) {
    const char* pump_names[4] = {"North", "South", "East", "West"};
    pumps[i] = (PumpControl){
        .pin = GPIO_NUM_0,
        .sensorFault = false,
        .name = pump_names[i],
        .state = PUMP_OFF,
        .timerDuration = 0,              
        .protectionTimeRemaining = 0,    
        .flameFirstDetectedTime = 0,
        .flameConfirmed = false,
        .lastFlameSeenTime = 0,
        .pumpStartTime = 0,
        .cooldownStartTime = 0,
        .cooldownDuration = 0,
        .currentIRValue = 0.0,
        .manualMode = false,
        .manualStartTime = 0,
        .manualDuration = 0,
        .isRunning = false,
        .stateBeforeEmergency = PUMP_OFF,
        .wasRunningBeforeEmergency = false,
        .emergencyStopTime = 0,
        .lastStopReason = STOP_REASON_NONE,
        .activationSource = ACTIVATION_SOURCE_NONE,
        // 🆕 INITIALIZE TIMER FIELDS
        .timerProtected = false,
        .timerEndTime = 0,
        .originalDuration = 0,
        .stopPumpRequested = false,
        .activatedInFullSystemMode = false
    };
}
    
    printf("[INIT] Arrays initialized successfully with corrected profiles\n");
}

// ========================================
// TIMER PROTECTION FUNCTIONS
// ========================================

/**
 * @brief Start a protected timer for a pump
 * @param index Pump index (0-3)
 * @param duration Timer duration in milliseconds
 */
void start_timer_protection(int index, unsigned long duration) {
    if (index < 0 || index >= 4) return;
    
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    pumps[index].timerProtected = true;
    pumps[index].timerEndTime = now + duration;
    pumps[index].originalDuration = duration;
    pumps[index].timerDuration = duration;
    pumps[index].protectionTimeRemaining = duration;
    
    printf("[TIMER] %s: Timer protection started for %lu seconds\n", 
           pumps[index].name, duration/1000);
}

unsigned long get_duration_from_code(int code) {
    switch(code) {
        case 0:   return 30000;    // ✅ 30 seconds
        case 1:   return 120000;   // ✅ 2 minutes
        case 2:   return 300000;   // ✅ 5 minutes
        case 3:   return 600000;   // ✅ 10 minutes
        default:
            printf("[DURATION] Invalid code %d - ignoring\n", code);
            return 0;
    }
}


/**
 * @brief Get human-readable string for duration code
 * @param code Extension code (0=30s, 1=2min, 2=5min, 3=10min)
 * @return String description of duration
 */
const char* get_duration_code_string(int code) {
    switch(code) {
        case 0:   return "30 seconds";
        case 1:   return "2 minutes";
        case 2:   return "5 minutes";
        case 3:   return "10 minutes";
        default:  return "invalid";
    }
}

/**
 * @brief Check if pump's timer has expired
 * @param index Pump index (0-3)
 * @return true if timer expired, false otherwise
 */
bool is_timer_expired(int index) {
    if (index < 0 || index >= 4) return true;
    
    if (!pumps[index].timerProtected) {
        return true; // No timer active
    }
    
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return (now >= pumps[index].timerEndTime);
}

/**
 * @brief Get remaining time on pump's timer
 * @param index Pump index (0-3)
 * @return Remaining time in seconds, or 0 if no timer
 **/
 
 unsigned long get_timer_remaining(int index) {
    if (index < 0 || index >= 4) return 0;
    
    if (!pumps[index].timerProtected) {
        return 0;
    }
    
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (now >= pumps[index].timerEndTime) {
        return 0;
    }
    unsigned long remaining = (pumps[index].timerEndTime - now) / 1000;
    pumps[index].protectionTimeRemaining = remaining * 1000;
    
     return remaining;
}

/**
 * @brief Extend an active timer
 * @param index Pump index (0-3)
 * @param extensionTime Additional time in milliseconds
 */
void extend_timer_protection(int index, unsigned long extensionTime) {
    if (index < 0 || index >= 4) return;
    
    if (!pumps[index].timerProtected) {
        printf("[TIMER] %s: No active timer to extend\n", pumps[index].name);
        return;
    }
    
    pumps[index].timerEndTime += extensionTime;
    
    unsigned long remaining = get_timer_remaining(index);
    printf("[TIMER] %s: Timer extended by %lu seconds (New remaining: %lu seconds)\n",
           pumps[index].name, extensionTime/1000, remaining);
}

/**
 * @brief Stop timer protection (called when pump stops)
 * @param index Pump index (0-3)
 */
void stop_timer_protection(int index) {
    if (index < 0 || index >= 4) return;
    
    pumps[index].timerProtected = false;
    pumps[index].timerEndTime = 0;
    pumps[index].originalDuration = 0;
    
    // RESET EXTENSION TRACKING IN MAIN.C (via extern)
    // Note: You'll need to add this as extern in fire_system.c:
    // extern int previous_extend_time[4];
    // previous_extend_time[index] = -1;
    
    printf("\n[TIMER] Timer protection stopped for %s\n", pumps[index].name);
}


// ========================================
// CONTINUOUS FEED DETECTION
// ========================================

void detect_continuous_feed(void) {
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Check every 10 seconds
    if (now - lastContinuousFeedCheck < 10000) {
        return;
    }
    
    lastContinuousFeedCheck = now;
    
    float currentLevel = level_s;
    static float feedCheckLevels[6] = {0};  // Track last 6 readings (1 minute)
    static int feedCheckIndex = 0;
    
    // Store current level
    feedCheckLevels[feedCheckIndex] = currentLevel;
    feedCheckIndex = (feedCheckIndex + 1) % 6;
    
    // Check for consistent water replenishment
    bool consistentReplenishment = true;
    for (int i = 0; i < 5; i++) {
        if (feedCheckLevels[(feedCheckIndex + i) % 6] == 0) {
            consistentReplenishment = false;
            break;
        }
        if (i > 0) {
            // Check if levels are stable or increasing
            float prev = feedCheckLevels[(feedCheckIndex + i - 1) % 6];
            float curr = feedCheckLevels[(feedCheckIndex + i) % 6];
            if (curr < prev - 2.0) {  // More than 2% decrease
                consistentReplenishment = false;
                break;
            }
        }
    }
    
    if (consistentReplenishment && !continuousWaterFeed) {
        continuousWaterFeed = true;
        printf("[FEED] CONTINUOUS WATER FEED DETECTED - MCRC lifted\n");
        continuousFeedConfidence = 6;  // Max confidence
    } 
    else if (!consistentReplenishment && continuousWaterFeed) {
        continuousFeedConfidence--;
        if (continuousFeedConfidence <= 0) {
            continuousWaterFeed = false;
            printf("[FEED] Continuous feed LOST - MCRC restored\n");
        }
    }
    
    lastWaterLevelForFeed = currentLevel;
}

// ========================================
// HARDWARE CONTROL FUNCTIONS
// ========================================

void pump_control(unsigned int pumpNum, bool state) {
    if (pumpNum < 1 || pumpNum > 4) {
        printf("[FIRE_SYSTEM] ERROR: Invalid pump number %d\n", pumpNum);
        return;
    }
    
    int index = pumpNum - 1;
    set_pump_hardware(index, state);
}

void all_off(void) {
    char log_msg[LOG_BUFFER_SIZE];
    
    esp_err_t ret1 = pca9555_set_port0_output(&pca_dev, 0x00);
    esp_err_t ret2 = pca9555_set_port1_output(&pca_dev, 0x00);
    
    if (ret1 == ESP_OK && ret2 == ESP_OK) {
		
	} else {
        snprintf(log_msg, LOG_BUFFER_SIZE, "[FIRE_SYSTEM] PCA9555 shutdown failed: Port0=%s, Port1=%s", 
                esp_err_to_name(ret1), esp_err_to_name(ret2));
        printf("%s\n", log_msg);
    }
    
    for (int i = 0; i < 4; i++) {
        pumps[i].isRunning = false;
    }
}

void apply_system_profile(SystemProfile newProfile) {
    printf("\n[FIRE_SYSTEM] ===== APPLYING PROFILE CHANGE =====\n");
    printf("[FIRE_SYSTEM] Switching from profile %d to %d\n", 
           currentProfile, newProfile);
    printf("[FIRE_SYSTEM] From: %s\n", profiles[currentProfile].name);
    printf("[FIRE_SYSTEM] To:   %s\n", profiles[newProfile].name);
    
    SystemProfile oldProfile = currentProfile;
    currentProfile = newProfile;
    
    ProfileConfig* newConfig = &profiles[newProfile];
    
    printf("[FIRE_SYSTEM] New Configuration:\n");
    printf("[FIRE_SYSTEM] - Auto Mode Full: %s\n", 
           newConfig->autoModeFull ? "YES (All pumps)" : "NO (Sector only)");
    printf("[FIRE_SYSTEM] - No Flame Timeout: %lu ms (%lu seconds)\n", 
           newConfig->noFlameTimeout, newConfig->noFlameTimeout/1000);
    printf("[FIRE_SYSTEM] - Max Run Cap Full: %lu ms (%lu minutes)\n", 
           newConfig->maxRunCapFull, newConfig->maxRunCapFull/60000);
    printf("[FIRE_SYSTEM] - Max Run Cap Sector: %lu ms (%lu minutes)\n", 
           newConfig->maxRunCapSector, newConfig->maxRunCapSector/60000);
    
    if (oldProfile != newProfile) {
        if (newProfile == CONTINUOUS_FEED) {
            continuousWaterFeed = true;
            printf("[FIRE_SYSTEM] Continuous water feed ENABLED (profile)\n");
        } else if (oldProfile == CONTINUOUS_FEED) {
            // Only disable if not detected by hardware
            if (continuousFeedConfidence < 3) {
                continuousWaterFeed = false;
                printf("[FIRE_SYSTEM] Continuous water feed DISABLED\n");
            }
        }
        
        if (newConfig->autoModeFull) {
            printf("[FIRE_SYSTEM] FULL-SYSTEM MODE: All pumps will activate on fire detection\n");
        } else {
            printf("[FIRE_SYSTEM] SECTOR MODE: Only affected pump will activate\n");
        }
        
        if (is_suppression_active()) {
            printf("[FIRE_SYSTEM] Stopping all active pumps due to profile change\n");
            stop_all_pumps("profile_change");
        }
    }
    
    printf("[FIRE_SYSTEM] Profile application COMPLETE\n");
    printf("[FIRE_SYSTEM] =====================================\n\n");
}

void set_pump_hardware(int index, bool state) {
    if (index < 0 || index >= 4) {
        printf("[PUMP] ERROR: Invalid pump index %d\n", index);
        return;
    }
    
    if (emergencyStopActive && state) {
        printf("[PUMP] BLOCKED: Cannot activate %s - Emergency stop active\n", pumps[index].name);
        return;
    }
    
    printf("[PUMP] Setting %s to %s\n", pumps[index].name, state ? "ON" : "OFF");
    
    uint8_t pca_pin;
    switch(index) {
        case 0: pca_pin = 3; break;  // North
        case 1: pca_pin = 2; break;  // South
        case 2: pca_pin = 1; break;  // East
        case 3: pca_pin = 0; break;  // West
        default: return;
    }
    
    esp_err_t ret = pca9555_set_pin_state(&pca_dev, 1, pca_pin, state);
    
    if (ret != ESP_OK) {
        printf("[PUMP] CONTROL FAILED: %s - %s\n", pumps[index].name, esp_err_to_name(ret));
        return;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    uint8_t port_state;
    ret = pca9555_read_port1_output(&pca_dev, &port_state);
    
    if (ret == ESP_OK) {
        bool actual_state = (port_state & (1 << pca_pin)) != 0;
        pumps[index].isRunning = actual_state;
        
        if (actual_state != state) {
            printf("[PUMP] VERIFICATION FAILED: %s commanded %s but PCA shows %s (Port 1, Pin %d)\n", 
                   pumps[index].name, 
                   state ? "ON" : "OFF",
                   actual_state ? "ON" : "OFF",
                   pca_pin);
            
            // 🆕 SEND CRITICAL ALERT
            send_alert_hardware_control_fail(index, "HW_VERIFY_FAIL");
                   
            printf("[PUMP] Attempting recovery...\n");
            pca9555_set_pin_state(&pca_dev, 1, pca_pin, state);
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            printf("[PUMP] SUCCESS: %s is %s (Port 1, Pin %d)\n", 
                   pumps[index].name, 
                   state ? "ON" : "OFF",
                   pca_pin);
        }
    } else {
        printf("[PUMP] READBACK FAILED: Cannot read PCA9555 - %s\n", esp_err_to_name(ret));
        pumps[index].isRunning = state;
    }
}

// ========================================
// EMERGENCY STOP FUNCTIONS
// ========================================

void save_current_pump_states(void) {
    printf("[EMERGENCY] Saving current pump states...\n");
    
    for (int i = 0; i < 4; i++) {
        savedPumpStates[i] = pumps[i].state;
        savedRunningStates[i] = pumps[i].isRunning;
        
        if (pumps[i].state == PUMP_MANUAL_ACTIVE) {
            savedManualTimes[i] = pumps[i].manualStartTime;
            savedManualDurations[i] = pumps[i].manualDuration;
        } else {
            savedManualTimes[i] = 0;
            savedManualDurations[i] = 0;
        }
        
        pumps[i].stateBeforeEmergency = pumps[i].state;
        pumps[i].wasRunningBeforeEmergency = pumps[i].isRunning;
        
        printf("[EMERGENCY] Pump %d: State=%d, Running=%d\n", 
               i+1, savedPumpStates[i], savedRunningStates[i]);
    }
}


void emergency_stop_all_pumps(StopReason reason) {
    char log_msg[LOG_BUFFER_SIZE];
    
    printf("[EMERGENCY] ===== EMERGENCY STOP ACTIVATED =====\n");
    printf("[EMERGENCY] Reason: %d (%s)\n", reason, get_stop_reason_string(reason));
    
    if (reason == STOP_REASON_EMERGENCY_STOP || reason == STOP_REASON_SHADOW_COMMAND) {
        save_current_pump_states();
    }
    
    const char* reason_str = get_stop_reason_string(reason);
    
    // ✅ FORCE STOP ALL PUMPS - Including timer-protected ones
    printf("[EMERGENCY] Stopping ALL pumps (including timer-protected)\n");
    
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state != PUMP_OFF && pumps[i].state != PUMP_DISABLED) {
            
            // Log if we're overriding a timer
            if (pumps[i].timerProtected && !is_timer_expired(i)) {
                unsigned long remaining = get_timer_remaining(i);
                printf("[EMERGENCY] Overriding timer protection on %s (%lu sec remaining)\n",
                       pumps[i].name, remaining);
            }
            
            pumps[i].lastStopReason = reason;
            pumps[i].emergencyStopTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            
            // Deactivate will now allow emergency stop to override timer
            deactivate_pump(i, reason_str);
        }
    }
    
    emergencyStopActive = true;
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
             "[EMERGENCY] All pumps stopped. Reason: %s", reason_str);
    printf("%s\n", log_msg);
    printf("[EMERGENCY] ====================================\n");
}

void restore_pumps_after_emergency(void) {
    char log_msg[LOG_BUFFER_SIZE];
    
    printf("[EMERGENCY] ===== RESTORING PUMPS AFTER EMERGENCY =====\n");
    
    if (!emergencyStopActive) {
        printf("[EMERGENCY] No emergency stop active\n");
        return;
    }
    
    emergencyStopActive = false;
    
    bool waterLocked = waterLockout;
    for (int i = 0; i < 4; i++) {
        if (waterLocked) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[EMERGENCY] Pump %d remains stopped due to WATER LOCKOUT",
                    i+1);
            printf("%s\n", log_msg);
            continue;
        }
        
        if (pumps[i].sensorFault) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[EMERGENCY] Pump %d remains stopped due to SENSOR FAULT",
                    i+1);
            printf("%s\n", log_msg);
            continue;
        }
        
        PumpState targetState = savedPumpStates[i];
        
        switch(targetState) {
            case PUMP_AUTO_ACTIVE:
                pumps[i].state = PUMP_OFF;
                pumps[i].flameFirstDetectedTime = 0;
                pumps[i].flameConfirmed = false;
                printf("[EMERGENCY] Pump %d restored to AUTO mode (will reactivate if fire detected)\n", i+1);
                break;
                
            case PUMP_MANUAL_ACTIVE:
                if (savedManualDurations[i] > 0) {
                    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    unsigned long elapsed = now - savedManualTimes[i];
                    unsigned long remaining = (elapsed < savedManualDurations[i]) ? 
                                            savedManualDurations[i] - elapsed : 0;
                    
                    if (remaining > 0) {
                        pumps[i].state = PUMP_MANUAL_ACTIVE;
                        pumps[i].manualMode = true;
                        pumps[i].manualStartTime = now - (savedManualDurations[i] - remaining);
                        pumps[i].manualDuration = savedManualDurations[i];
                        pumps[i].pumpStartTime = savedManualTimes[i];
                        set_pump_hardware(i, true);
                        
                        snprintf(log_msg, LOG_BUFFER_SIZE,
                                "[EMERGENCY] Pump %d restored to MANUAL mode (%lu seconds remaining)",
                                i+1, remaining/1000);
                        printf("%s\n", log_msg);
                        on_pump_activated(i, true);
                    } else {
                        pumps[i].state = PUMP_OFF;
                        printf("[EMERGENCY] Pump %d manual time expired\n", i+1);
                    }
                } else {
                    pumps[i].state = PUMP_OFF;
                }
                break;
                
            case PUMP_COOLDOWN:
                pumps[i].state = PUMP_OFF;
                pumps[i].cooldownStartTime = 0;
                printf("[EMERGENCY] Pump %d cooldown reset\n", i+1);
                break;
                
            case PUMP_OFF:
            case PUMP_DISABLED:
                printf("[EMERGENCY] Pump %d remains OFF\n", i+1);
                break;
                
            default:
                pumps[i].state = PUMP_OFF;
                printf("[EMERGENCY] Pump %d set to OFF (unknown previous state)\n", i+1);
                break;
        }
    }
    
    printf("[EMERGENCY] ===============================================\n");
}

bool is_emergency_stop_active(void) {
    return emergencyStopActive;
}

void set_emergency_stop(bool enable, StopReason reason) {
    if (enable && !emergencyStopActive) {
        emergency_stop_all_pumps(reason);
    } else if (!enable && emergencyStopActive) {
        restore_pumps_after_emergency();
    }
}

StopReason get_pump_stop_reason(int index) {
    if (index < 0 || index >= 4) {
        return STOP_REASON_NONE;
    }
    return pumps[index].lastStopReason;
}

const char* get_stop_reason_string(StopReason reason) {
    switch(reason) {
        case STOP_REASON_NONE: return "none";
        case STOP_REASON_MANUAL: return "manual_stop";
        case STOP_REASON_AUTO_TIMEOUT: return "no_flame_timeout";
        case STOP_REASON_RUN_CAP: return "max_run_cap_expired";
        case STOP_REASON_WATER_LOCKOUT: return "water_lockout";
        case STOP_REASON_EMERGENCY_STOP: return "emergency_stop";
        case STOP_REASON_SHADOW_COMMAND: return "shadow_command";
        case STOP_REASON_SENSOR_FAULT: return "sensor_fault";
        default: return "unknown";
    }
}

void process_shadow_emergency_stop(bool stopCommand) {
    printf("[SHADOW] Processing emergency stop command: %s\n", 
           stopCommand ? "STOP" : "RESUME");
    
    if (stopCommand) {
        set_emergency_stop(true, STOP_REASON_SHADOW_COMMAND);
    } else {
        set_emergency_stop(false, STOP_REASON_SHADOW_COMMAND);
    }
}

// ========================================
// CURRENT SENSOR FUNCTIONS
// ========================================

void init_current_sensors(void) {
    char log_msg[LOG_BUFFER_SIZE];
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR1_PIN) | (1ULL << SENSOR2_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    gpio_config_t mux_conf = {
        .pin_bit_mask = (1ULL << s0) | (1ULL << s1) | (1ULL << s2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&mux_conf);
    
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &adc1_handle);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, "[FIRE_SYSTEM] ADC unit init failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
        return;
    }
    
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &channel_config);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, "[FIRE_SYSTEM] ADC channel 0 (GPIO36) config failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
    }
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_6, &channel_config);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, "[FIRE_SYSTEM] ADC channel 6 config failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
    }
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_7, &channel_config);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, "[FIRE_SYSTEM] ADC channel 7 config failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
    }
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &channel_config);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, "[FIRE_SYSTEM] ADC channel 3 config failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
    }
    
    if (adc_cali_handle) {
        adc_cali_delete_scheme_line_fitting(adc_cali_handle);
        adc_cali_handle = NULL;
    }
    
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    
    ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle);
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, "[FIRE_SYSTEM] ADC calibration init failed: %s", esp_err_to_name(ret));
        printf("%s\n", log_msg);
        adc_cali_handle = NULL;
    } 
    
    for (int i = 0; i < 4; i++) {
        currentSensors[i].currentValue = 0.0;
        currentSensors[i].averageValue = 0.0;
        currentSensors[i].fault = false;
        currentSensors[i].lastReadTime = 0;
    }
    
}

void set_mux_channel(int channel) {
    gpio_set_level(s0, (channel & 0x01));
    gpio_set_level(s1, (channel & 0x02) >> 1);
    gpio_set_level(s2, (channel & 0x04) >> 2);
    vTaskDelay(pdMS_TO_TICKS(10));
}

float measure_current(int adc_channel) {
    TickType_t startTime = xTaskGetTickCount();
    uint32_t maxValue = 0;
    uint32_t minValue = ADC_RES;

    while ((xTaskGetTickCount() - startTime) * portTICK_PERIOD_MS < SAMPLE_WINDOW) {
        int adcVal = 0;
        
        esp_err_t ret = adc_oneshot_read(adc1_handle, adc_channel, &adcVal);
        if (ret != ESP_OK) {
            printf("[CURRENT] ADC read error: %s\n", esp_err_to_name(ret));
            adcVal = 0;
        }
        
        if (adcVal > maxValue) maxValue = adcVal;
        if (adcVal < minValue) minValue = adcVal;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    float Vmax = (maxValue * VREF) / ADC_RES - BIAS_VOLTAGE;
    float Vmin = (minValue * VREF) / ADC_RES - BIAS_VOLTAGE;
    float Vpp = Vmax - Vmin;
    float Vpeak = Vpp / 2.0;
    float Vrms = Vpeak * 0.7071;
    float I_mapped = Vrms / R_SHUNT;
    float I_load = I_mapped / SCALE_RATIO;

    return I_load;
}

float read_current_sensor(int index) {
    CurrentSensor* sensor = &currentSensors[index];
    char log_msg[LOG_BUFFER_SIZE];
    
    if (sensor->isMux) {
        set_mux_channel(sensor->muxChannel);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    
    int adc_channel;
    if (sensor->pin == GPIO_NUM_34) 
        adc_channel = ADC_CHANNEL_6;
    else if (sensor->pin == GPIO_NUM_35) 
        adc_channel = ADC_CHANNEL_7;
    else 
        adc_channel = ADC_CHANNEL_3;
    
    if (adc1_handle == NULL) {
        snprintf(log_msg, LOG_BUFFER_SIZE, "[FIRE_SYSTEM] ERROR: ADC not initialized for sensor %s", sensor->name);
        printf("%s\n", log_msg);
        sensor->fault = true;
        return 0.0;
    }
    
    float current = measure_current(adc_channel);
    
    if (sensor->averageValue == 0.0) {
        sensor->averageValue = current;
    } else {
        sensor->averageValue = 0.9 * sensor->averageValue + 0.1 * current;
    }
    
    sensor->currentValue = current;
    sensor->lastReadTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return current;
}

void read_all_current_sensors(void) {
    static unsigned long lastReadTime = 0;
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (now - lastReadTime < 100) {
        return;
    }
    lastReadTime = now;
    
    for (int i = 0; i < 4; i++) {
        read_current_sensor(i);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void check_current_sensor_faults(void) {
    char log_msg[LOG_BUFFER_SIZE];
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    static unsigned long lastFaultCheck = 0;
    
    if (now - lastFaultCheck < 2000) {
        return;
    }
    lastFaultCheck = now;
    
    for (int i = 0; i < 4; i++) {
        CurrentSensor* sensor = &currentSensors[i];
        bool previousFault = sensor->fault;
        
        sensor->fault = (sensor->currentValue < -0.1 || sensor->currentValue > 10.0);
        
        if ((now - sensor->lastReadTime) > 5000) {
            sensor->fault = true;
        }
        
        if (adc1_handle == NULL) {
            sensor->fault = true;
        }
        
        if (sensor->fault && !previousFault) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[FIRE_SYSTEM] Current Sensor %s FAULT DETECTED: %.3f A", 
                    sensor->name, sensor->currentValue);
            printf("%s\n", log_msg);
            // 🆕 SEND FAULT ALERT
           // extern void send_alert_current_sensor_fault(int, float);
            send_alert_current_sensor_fault(i, sensor->currentValue);
        } else if (!sensor->fault && previousFault) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[FIRE_SYSTEM] Current Sensor %s FAULT CLEARED", 
                    sensor->name);
            printf("%s\n", log_msg);
        }
    }
}

// ========================================
// COMPLETE SENSOR DATA ACQUISITION
// ========================================

void get_sensor_data(void) {
   
    if (adc1_handle == NULL) {
        printf("[SENSOR] ERROR: ADC not initialized\n");
        return;
    }
    
    for (int i = 0; i < 8; i++) {
        adc_array1[i] = 0;
        adc_array2[i] = 0;
    }

    for (uint8_t channel = 0; channel < 8; channel++) {
        set_mux_channel(channel);
        vTaskDelay(pdMS_TO_TICKS(5));

        float sum1 = 0, sum2 = 0;
        const int samples = 10;

        for (int i = 0; i < samples; i++) {
            int raw_adc1 = 0, raw_adc2 = 0;
            
            esp_err_t ret1 = adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &raw_adc1);
            if (ret1 != ESP_OK) {
                raw_adc1 = 0;
            }
            
            if (channel < 6) {
                esp_err_t ret2 = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &raw_adc2);
                if (ret2 != ESP_OK) {
                    raw_adc2 = 0;
                }
                sum2 += raw_adc2;
            }
            
            sum1 += raw_adc1;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (adc_cali_handle) {
            int voltage_mv1 = 0, voltage_mv2 = 0;
            
            adc_cali_raw_to_voltage(adc_cali_handle, (int)(sum1 / samples), &voltage_mv1);
            adc_array1[channel] = voltage_mv1 / 1000.0;
            
            if (channel < 6) {
                adc_cali_raw_to_voltage(adc_cali_handle, (int)(sum2 / samples), &voltage_mv2);
                adc_array2[channel] = voltage_mv2 / 1000.0;
            }
        } else {
            adc_array1[channel] = (sum1 / samples) * (VREF / ADC_RES);
            if (channel < 6)
                adc_array2[channel] = (sum2 / samples) * (VREF / ADC_RES);
        }
    }

    read_all_current_sensors();

    for (int i = 0; i < 4; i++) {
        set_mux_channel(waterLevelChannels[i]);
        vTaskDelay(pdMS_TO_TICKS(5));

        float sum = 0;
        const int samples = 10;

        for (int j = 0; j < samples; j++) {
            int raw_adc = 0;
            esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &raw_adc);
            
            if (ret != ESP_OK) {
                raw_adc = 0;
            }
            
            sum += raw_adc;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        float voltage = 0;
        if (adc_cali_handle) {
            int voltage_mv = 0;
            adc_cali_raw_to_voltage(adc_cali_handle, (int)(sum / samples), &voltage_mv);
            voltage = voltage_mv / 1000.0;
        } else {
            voltage = (sum / samples) * (VREF / ADC_RES);
        }

        float levelPercent = (voltage - 0.7) / (3.0 - 0.7) * 100.0;
        levelPercent = levelPercent < 0.0 ? 0.0 : (levelPercent > 100.0 ? 100.0 : levelPercent);

        waterLevels[i] = levelPercent;
    }

    level_s = (waterLevels[0] + waterLevels[1] + waterLevels[2] + waterLevels[3]) / 4.0;

    ir_s1 = (adc_array1[4] / 3.3) * 100.0;
    ir_s2 = (adc_array1[5] / 3.3) * 100.0;
    ir_s3 = (adc_array1[6] / 3.3) * 100.0;
    ir_s4 = (adc_array1[7] / 3.3) * 100.0;

    sol_v = (adc_array2[0] * REVERSE_RATIO) + DIODE_DROP;
    bat_v = (adc_array2[1] * REVERSE_RATIO) + DIODE_DROP;

    check_current_sensor_faults();

    pumps[0].currentIRValue = ir_s1;
    pumps[1].currentIRValue = ir_s2;
    pumps[2].currentIRValue = ir_s3;
    pumps[3].currentIRValue = ir_s4;

    check_water_lockout();
    detect_continuous_feed();  // Added continuous feed detection
}

// ========================================
// CORRECTED WATER LOCKOUT MANAGEMENT
// ========================================

void check_water_lockout(void) {
    char log_msg[LOG_BUFFER_SIZE];
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // ========================================
    // SECTION 4.1: LOW WATER DETECTION
    // ========================================
    if (level_s < LOW_LEVEL_THRESHOLD) {
        
        // ========================================
        // SECTION 4.3: CONTINUOUS FEED GRACE PERIOD
        // ========================================
        if (continuousWaterFeed && !waterLockout && !inGracePeriod) {
            inGracePeriod = true;
            gracePeriodStartTime = now;
            gracePeriodWaterLevel = level_s;
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[WATER] Low water (%.1f%%) - Starting 20s grace period for continuous feed",
                    level_s);
            printf("%s\n", log_msg);
            return;
        }

        // ========================================
        // SECTION 4.3: GRACE PERIOD CHECK
        // ========================================
        if (inGracePeriod) {
            // Check if water is recovering during grace period
            if (level_s > gracePeriodWaterLevel + 5.0) {
                printf("[WATER] Water recovering during grace period: %.1f%% -> %.1f%%\n",
                       gracePeriodWaterLevel, level_s);
                gracePeriodWaterLevel = level_s;
                gracePeriodStartTime = now;
            }
            
            // Check if grace period expired without recovery
            if (now - gracePeriodStartTime >= GRACE_PERIOD_TIME) {
                if (!waterLockout) {
                    waterLockout = true;
                    inGracePeriod = false;
                    printf("[WATER] GRACE PERIOD EXPIRED - LOCKOUT ACTIVATED\n");
                    
                    // ✅ FORCE STOP ALL PUMPS (including timer-protected)
                    printf("[WATER] Stopping ALL pumps due to water lockout\n");
                    for (int i = 0; i < 4; i++) {
                        if (pumps[i].state == PUMP_AUTO_ACTIVE || 
                            pumps[i].state == PUMP_MANUAL_ACTIVE) {
                            
                            if (pumps[i].timerProtected && !is_timer_expired(i)) {
                                unsigned long remaining = get_timer_remaining(i);
                                printf("[WATER] Overriding timer on %s (%lu sec remaining)\n",
                                       pumps[i].name, remaining);
                            }
                            
                            deactivate_pump(i, "water_lockout_grace_expired");
                        }
                    }
                    on_water_lockout_activated();
                }
            }
        }

        // ========================================
        // SECTION 4.1: NON-CONTINUOUS FEED LOCKOUT
        // ========================================
        if (!continuousWaterFeed && !waterLockout) {
            waterLockout = true;
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[WATER] WATER LOCKOUT ACTIVATED - Level: %.1f%%", level_s);
            printf("%s\n", log_msg);
            
            // ✅ FORCE STOP ALL PUMPS
            printf("[WATER] Stopping ALL pumps due to low water\n");
            for (int i = 0; i < 4; i++) {
                if (pumps[i].state == PUMP_AUTO_ACTIVE || 
                    pumps[i].state == PUMP_MANUAL_ACTIVE) {
                    
                    if (pumps[i].timerProtected && !is_timer_expired(i)) {
                        unsigned long remaining = get_timer_remaining(i);
                        printf("[WATER] Overriding timer on %s (%lu sec remaining)\n",
                               pumps[i].name, remaining);
                    }
                    
                    deactivate_pump(i, "water_lockout");
                }
            }
            on_water_lockout_activated();
        }

        waterStable = false;
        waterAboveResumeTime = 0;
        stableStartTime = 0;

    } 
    // ========================================
    // SECTION 4.2: WATER RECOVERY ABOVE AUTO RESUME
    // ========================================
    else if (level_s > AUTO_RESUME_LEVEL) {
        inGracePeriod = false;

        if (waterLockout) {
            // ========================================
            // SECTION 4.2: 5-SECOND STABILITY CHECK
            // ========================================
            if (fabs(level_s - lastStableWaterLevel) < 2.0) {
                if (stableStartTime == 0) {
                    stableStartTime = now;
                    snprintf(log_msg, LOG_BUFFER_SIZE, 
                            "[WATER] Water stable at %.1f%%, starting 5s stability check", 
                            level_s);
                    printf("%s\n", log_msg);
                } else if (now - stableStartTime >= 5000) {
                    waterLockout = false;
                    stableStartTime = 0;
                    snprintf(log_msg, LOG_BUFFER_SIZE, 
                            "[WATER] Water stable for 5s, LOCKOUT RELEASED - Level: %.1f%%", 
                            level_s);
                    printf("%s\n", log_msg);
                    on_water_lockout_released();
                }
            } else {
                stableStartTime = 0;
                lastStableWaterLevel = level_s;
                snprintf(log_msg, LOG_BUFFER_SIZE, 
                        "[WATER] Water unstable: %.1f%% -> %.1f%%, resetting stability timer",
                        lastStableWaterLevel, level_s);
                printf("%s\n", log_msg);
            }
        }
    } 
    else {
        waterAboveResumeTime = 0;
        waterStable = false;
        stableStartTime = 0;
    }
}

// ========================================
// CORRECTED AUTOMATIC FIRE DETECTION
// ========================================

void check_automatic_activation(void) {
    char log_msg[LOG_BUFFER_SIZE];
    if (waterLockout || !systemArmed) return;

    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // First, check each sensor for flame confirmation
    for (int i = 0; i < 4; i++) {
        if (pumps[i].sensorFault) {
            printf("[FIRE] %s: Sensor fault, ignoring activation\n", pumps[i].name);
            continue;
        }
        if (pumps[i].state == PUMP_MANUAL_ACTIVE || pumps[i].state == PUMP_COOLDOWN) {
            continue;
        }

        bool flameDetected = pumps[i].currentIRValue > FIRE_THRESHOLD;

        // ========================================
        // SECTION 2(A): 2-SECOND FLAME CONFIRMATION
        // ========================================
        if (flameDetected) {
            pumps[i].lastFlameSeenTime = now;
            
            if (!flameValidating[i]) {
                // First detection - start 2-second timer
                flameStartTime[i] = now;
                flameValidating[i] = true;
                snprintf(log_msg, LOG_BUFFER_SIZE, 
                        "[FIRE] %s: Flame detected (%.1f%%) - Starting 2s confirmation",
                        pumps[i].name, pumps[i].currentIRValue);
                printf("%s\n", log_msg);
            } 
            else if (now - flameStartTime[i] >= 2000) {  // 2 seconds confirmed
                if (!pumps[i].flameConfirmed) {
                    pumps[i].flameConfirmed = true;
                    pumps[i].flameFirstDetectedTime = now;
                    snprintf(log_msg, LOG_BUFFER_SIZE, 
                            "[FIRE] %s: FLAME CONFIRMED (persisted 2+ seconds)",
                            pumps[i].name);
                    printf("%s\n", log_msg);
                    on_flame_confirmed(i);
                }
            }
        } 
        else {
            // Flame lost
            if (flameValidating[i]) {
                snprintf(log_msg, LOG_BUFFER_SIZE, 
                        "[FIRE] %s: Flame lost before confirmation (< 2s)", 
                        pumps[i].name);
                printf("%s\n", log_msg);
                flameValidating[i] = false;
            }
            pumps[i].flameConfirmed = false;
            pumps[i].flameFirstDetectedTime = 0;
        }
    }
    
    // ========================================
    // DECISION LOGIC: Check which pumps should activate
    // ========================================
    ProfileConfig* profile = &profiles[currentProfile];
    
    // Count how many pumps have confirmed flame
    int confirmedCount = 0;
    for (int i = 0; i < 4; i++) {
        if (pumps[i].flameConfirmed) {
            confirmedCount++;
        }
    }
    
    // Check if we should activate in FULL SYSTEM mode
    bool shouldActivateFullSystem = (confirmedCount == 4) && profile->autoModeFull;
    
    if (shouldActivateFullSystem) {
        // ========================================
        // FULL-SYSTEM MODE: All 4 sensors confirmed
        // ========================================
       // Activate ALL pumps using activate_pump() with activateAll = true
        // Find first pump with confirmed flame to trigger the activation
        for (int i = 0; i < 4; i++) {
            if (pumps[i].flameConfirmed && pumps[i].state != PUMP_AUTO_ACTIVE) {
                activate_pump(i, true);  // true = activate ALL pumps
                break;  // Only need to call once
            }
        }
    } else if (confirmedCount > 0) {
        // ========================================
        // SECTOR MODE: Activate only pumps with confirmed flame
        // ========================================
        //printf("[FIRE] SECTOR MODE: %d sensor(s) confirm fire\n", confirmedCount);
        
        for (int i = 0; i < 4; i++) {
            if (pumps[i].flameConfirmed && pumps[i].state != PUMP_AUTO_ACTIVE) {
                activate_pump(i, false);  // false = activate only this pump
            }
        }
    }
}

// ========================================
// CORRECTED PUMP STATE MANAGEMENT
// ========================================

void update_pump_states(void) {
    char log_msg[LOG_BUFFER_SIZE];
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (emergencyStopActive) {
        static unsigned long lastEmergencyCheck = 0;
        if (now - lastEmergencyCheck > 5000) {
            printf("[PUMP] Emergency stop active - pump state updates suspended\n");
            lastEmergencyCheck = now;
        }
        return;
    }

    for (int i = 0; i < 4; i++) {
        
        if (pumps[i].stopPumpRequested) {
            process_stop_pump_request(i);
            continue;
        }
        
        // COOLDOWN STATE
        if (pumps[i].state == PUMP_COOLDOWN) {
            if (pumps[i].cooldownDuration == 0) {
                pumps[i].cooldownDuration = 15000 + (rand() % 15001);
            }
            
            unsigned long cooldownElapsed = now - pumps[i].cooldownStartTime;
            if (cooldownElapsed >= pumps[i].cooldownDuration) {
                // ✅ COOLDOWN COMPLETE - SYSTEM RE-ARMS
                pumps[i].state = PUMP_OFF;
                pumps[i].cooldownStartTime = 0;
                pumps[i].cooldownDuration = 0;
                
                printf("[COOLDOWN] %s: Cooldown complete - SYSTEM RE-ARMED\n", pumps[i].name);
                printf("[COOLDOWN] %s: Ready for new activation if flame detected\n", pumps[i].name);
                
                // ✅ System will automatically check for flame and reactivate in check_automatic_activation()
            }
            continue;
        }

        // TIMER-PROTECTED PUMPS
        if (pumps[i].timerProtected) {
            if (is_timer_expired(i)) {
                snprintf(log_msg, LOG_BUFFER_SIZE, 
                        "[TIMER] %s: Timer expired - Stopping pump", 
                        pumps[i].name);
                printf("%s\n", log_msg);
                deactivate_pump(i, "timer_expired");
                continue;
            }
            
            unsigned long remaining = get_timer_remaining(i);
            
            if (pumps[i].state == PUMP_MANUAL_ACTIVE && 
                pumps[i].currentIRValue > 80.0) {
                printf("[TIMER] %s: Fire detected, transitioning to AUTO mode (timer continues: %lu sec)\n",
                       pumps[i].name, remaining);
                pumps[i].state = PUMP_AUTO_ACTIVE;
                pumps[i].activationSource = ACTIVATION_SOURCE_AUTO;
            }
            
            static unsigned long lastTimerLog[4] = {0};
            if (now - lastTimerLog[i] > 10000) {
                printf("[TIMER] %s: PROTECTED - %lu seconds remaining (State: %s)\n",
                       pumps[i].name, remaining, get_pump_state_string(i));
                lastTimerLog[i] = now;
            }
            
            continue;
        }

        // MANUAL MODE (WITHOUT TIMER)
        if (pumps[i].state == PUMP_MANUAL_ACTIVE && !pumps[i].timerProtected) {
            printf("[MANUAL] WARNING: %s in manual mode WITHOUT timer protection (legacy mode)\n",
                   pumps[i].name);
            
            unsigned long manualElapsed = now - pumps[i].manualStartTime;
            if (manualElapsed >= pumps[i].manualDuration) {
                snprintf(log_msg, LOG_BUFFER_SIZE, 
                        "[MANUAL] %s: Manual timer expired (legacy)", 
                        pumps[i].name);
                printf("%s\n", log_msg);
                deactivate_pump(i, "manual_timer_expired");
            }
            continue;
        }

        // ========================================
        // AUTO MODE - MCRC ENFORCEMENT
        // ========================================
        if (pumps[i].state != PUMP_AUTO_ACTIVE) continue;

        // NFT Check
        unsigned long noFlameTimeout = 60000;
        if (currentProfile == WILDLAND_HIGH_WIND) {
            noFlameTimeout = 45000;
        }
        
        unsigned long timeSinceFlame = now - pumps[i].lastFlameSeenTime;
        if (timeSinceFlame >= noFlameTimeout) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[NFT] %s: No flame for %lus - Stopping", 
                    pumps[i].name, noFlameTimeout/1000);
            printf("%s\n", log_msg);
            deactivate_pump(i, "no_flame_timeout");
            continue;
        }

        // ✅ CORRECTED: MCRC Check - Uses ONLY current activation runtime
        ProfileConfig* profile = &profiles[currentProfile];
        unsigned long maxRunCap = profile->autoModeFull ? 
                                  profile->maxRunCapFull : profile->maxRunCapSector;
        
        if (continuousWaterFeed) {
            maxRunCap = 0; // No limit in continuous feed mode
        }

        // Calculate runtime for THIS activation only
        unsigned long runTime = now - pumps[i].pumpStartTime;
        
        if (maxRunCap > 0 && runTime >= maxRunCap) {
            const char* capType = pumps[i].activatedInFullSystemMode ? "Full" : "Sector";
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[MCRC] %s: Max run cap reached (%s: %lu/%lu sec) - Stopping", 
                    pumps[i].name, capType, runTime/1000, maxRunCap/1000);
            printf("%s\n", log_msg);
            deactivate_pump(i, "max_run_cap_expired");
            
            pumps[i].state = PUMP_COOLDOWN;
            pumps[i].cooldownStartTime = now;
            pumps[i].cooldownDuration = 15000 + (rand() % 15001);
        }
    }
}




void activate_pump(int index, bool activateAll) {
    char log_msg[LOG_BUFFER_SIZE];
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (emergencyStopActive) {
        printf("[PUMP] Activation blocked - Emergency stop active\n");
        return;
    }

    if (pumps[index].state == PUMP_AUTO_ACTIVE) return;

    if (activateAll) {
        for (int i = 0; i < 4; i++) {
            if (pumps[i].state == PUMP_OFF && !waterLockout) {
                pumps[i].state = PUMP_AUTO_ACTIVE;
                pumps[i].manualMode = false;
                pumps[i].pumpStartTime = now;
                pumps[i].lastFlameSeenTime = now;
                pumps[i].activationSource = ACTIVATION_SOURCE_AUTO;
                pumps[i].activatedInFullSystemMode = true;
                set_pump_hardware(i, true);
                snprintf(log_msg, LOG_BUFFER_SIZE, 
                        "[FIRE_SYSTEM] Pump %s ACTIVATED (Full-System Mode)", 
                        pumps[i].name);
                printf("%s\n", log_msg);
                on_pump_activated(i, false);
            }
        }
    } else {
        pumps[index].state = PUMP_AUTO_ACTIVE;
        pumps[index].manualMode = false;
        pumps[index].pumpStartTime = now;
        pumps[index].lastFlameSeenTime = now;
        pumps[index].activationSource = ACTIVATION_SOURCE_AUTO;
        pumps[index].activatedInFullSystemMode = false;
        set_pump_hardware(index, true);
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[FIRE_SYSTEM] Pump %s ACTIVATED (Sector Mode)", 
                pumps[index].name);
        printf("%s\n", log_msg);
        on_pump_activated(index, false);
    }
}

void deactivate_pump(int index, const char* reason) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) return;

    // ✅ CHECK TIMER PROTECTION - Only allow specific reasons to override
    if (pumps[index].timerProtected && !is_timer_expired(index)) {
        // Only these reasons can stop a timer-protected pump:
        bool allowedToStop = false;
        
        if (strstr(reason, "water_lockout") != NULL) {
            allowedToStop = true;
            printf("[PUMP] %s: Timer-protected but WATER LOCKOUT - forcing stop\n", 
                   pumps[index].name);
        }
        else if (strstr(reason, "emergency_stop") != NULL || 
                 strstr(reason, "shadow_command") != NULL) {
            allowedToStop = true;
            printf("[PUMP] %s: Timer-protected but EMERGENCY STOP - forcing stop\n", 
                   pumps[index].name);
        }
        else if (strstr(reason, "timer_expired") != NULL) {
            allowedToStop = true;
            printf("[PUMP] %s: Timer expired naturally\n", pumps[index].name);
        }
        
        if (!allowedToStop) {
            unsigned long remaining = get_timer_remaining(index);
            printf("[PUMP] %s: BLOCKED deactivation (reason: %s) - Timer protected (%lu sec remaining)\n",
                   pumps[index].name, reason, remaining);
            return; // ✅ BLOCK the stop
        }
    }

    unsigned long runTime = (xTaskGetTickCount() * portTICK_PERIOD_MS) - pumps[index].pumpStartTime;
    ActivationSource stoppedSource = pumps[index].activationSource;

    if (pumps[index].state != PUMP_COOLDOWN) {
        pumps[index].state = PUMP_OFF;
    }

    pumps[index].flameFirstDetectedTime = 0;
    pumps[index].flameConfirmed = false;
    pumps[index].manualMode = false;
    flameValidating[index] = false;
    
    pumps[index].activationSource = ACTIVATION_SOURCE_NONE;
    
    // ✅ STOP TIMER PROTECTION
    stop_timer_protection(index);

    set_pump_hardware(index, false);

    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[FIRE_SYSTEM] Pump %s STOPPED - Reason: %s (Ran %lu seconds) | Source: %s",
            pumps[index].name, reason, runTime/1000, 
            get_activation_source_string(stoppedSource));
    printf("%s\n", log_msg);
    
    on_pump_deactivated(index, reason);
}

void stop_all_pumps(const char* reason) {
    printf("[FIRE_SYSTEM] STOPPING ALL PUMPS - Reason: %s\n", reason);

    for (int i = 0; i < 4; i++) {
        if (pumps[i].state != PUMP_OFF && pumps[i].state != PUMP_DISABLED) {
            StopReason stopReason = STOP_REASON_MANUAL;
            
            if (strstr(reason, "water_lockout")) stopReason = STOP_REASON_WATER_LOCKOUT;
            else if (strstr(reason, "no_flame_timeout")) stopReason = STOP_REASON_AUTO_TIMEOUT;
            else if (strstr(reason, "max_run_cap_expired")) stopReason = STOP_REASON_RUN_CAP;
            else if (strstr(reason, "sensor_fault")) stopReason = STOP_REASON_SENSOR_FAULT;
            else if (strstr(reason, "emergency_stop")) stopReason = STOP_REASON_EMERGENCY_STOP;
            else if (strstr(reason, "shadow_command")) stopReason = STOP_REASON_SHADOW_COMMAND;
            else if (strstr(reason, "profile_change")) stopReason = STOP_REASON_MANUAL;
            
            pumps[i].lastStopReason = stopReason;
            pumps[i].emergencyStopTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            
            deactivate_pump(i, reason);
        }
    }
}

// ========================================
// STOP PUMP WITH TIMER OVERRIDE 
// ========================================

/**
 * @brief Process stop pump request from shadow
 * This function handles the stopPump parameter and overrides timer protection
 * @param index Pump index (0-3)
 */
void process_stop_pump_request(int index) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (index < 0 || index >= 4) {
        printf("[STOP] ERROR: Invalid pump index %d\n", index);
        return;
    }
    
    if (!pumps[index].stopPumpRequested) {
        return; // No stop request
    }
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[STOP] Processing stop request for %s", pumps[index].name);
    printf("%s\n", log_msg);
    
    // Check if pump is running
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[STOP] %s already stopped, clearing flag", pumps[index].name);
        printf("%s\n", log_msg);
        pumps[index].stopPumpRequested = false;
        return;
    }
    
    // Check if timer protected
    if (pumps[index].timerProtected && !is_timer_expired(index)) {
        unsigned long remaining = get_timer_remaining(index);
        snprintf(log_msg, LOG_BUFFER_SIZE,
                "[STOP] %s: Overriding timer protection (%lu sec remaining) - USER REQUESTED STOP",
                pumps[index].name, remaining);
        printf("%s\n", log_msg);
        
        // Stop timer protection
        stop_timer_protection(index);
    }
    
    // Stop the pump
    deactivate_pump(index, "user_stop_requested");
    
    // Clear the stop request flag
    pumps[index].stopPumpRequested = false;
    
    snprintf(log_msg, LOG_BUFFER_SIZE,
            "[STOP] %s stopped successfully via stopPump parameter", 
            pumps[index].name);
    printf("%s\n", log_msg);
}

/**
 * @brief Shadow command to stop pump with timer override
 * @param index Pump index (0-3)
 * @return true if successful, false otherwise
 */
bool shadow_manual_stop_pump_override_timer(int index) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (index < 0 || index >= 4) {
        printf("[SHADOW-STOP] ERROR: Invalid pump index %d\n", index);
        return false;
    }
    
    // Set the stop request flag
    pumps[index].stopPumpRequested = true;
    
    snprintf(log_msg, LOG_BUFFER_SIZE,
            "[SHADOW-STOP] Stop request set for %s", pumps[index].name);
    printf("%s\n", log_msg);
    
    // Process immediately
    process_stop_pump_request(index);
    
    return true;
}

// ========================================
// PRINT CURRENT SENSOR STATUS
// ========================================

void print_current_sensor_status(void) {
    printf("\n[CURRENT SENSORS] Detailed Status:\n");
    printf("----------------------------------\n");
    
    for (int i = 0; i < 4; i++) {
        printf("%s (%s):\n", currentSensors[i].name, pumps[i].name);
        printf("  Current: %.3f A | Average: %.3f A\n", 
               currentSensors[i].currentValue, currentSensors[i].averageValue);
        printf("  Fault: %s | Mux: %s", 
               currentSensors[i].fault ? "YES" : "NO",
               currentSensors[i].isMux ? "YES" : "NO");
        
        if (currentSensors[i].isMux) {
            printf(" | Channel: %d", currentSensors[i].muxChannel);
        }
        
        unsigned long time_since_read = xTaskGetTickCount() * portTICK_PERIOD_MS - currentSensors[i].lastReadTime;
        printf("\n  Last read: %lu ms ago\n", time_since_read);
    }
    printf("----------------------------------\n");
}

// ========================================
// CORRECTED MANUAL CONTROL FUNCTIONS
// ========================================

void manual_activate_pump(int index) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (emergencyStopActive) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[MANUAL] Manual activation BLOCKED for %s - Emergency stop active", 
                pumps[index].name);
        printf("%s\n", log_msg);
        return;
    }
    
    if (waterLockout) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[MANUAL] Manual activation BLOCKED for %s - Water lockout active", 
                pumps[index].name);
        printf("%s\n", log_msg);
        return;
    }

    // ========================================
    // SECTION 3.7: IMMEDIATE RESTART ALLOWED
    // ========================================
    if (pumps[index].state == PUMP_MANUAL_ACTIVE || 
        pumps[index].state == PUMP_AUTO_ACTIVE) {
        deactivate_pump(index, "manual_restart");
        vTaskDelay(pdMS_TO_TICKS(100));  // Brief pause for restart
    }

    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    pumps[index].state = PUMP_MANUAL_ACTIVE;
    pumps[index].manualMode = true;
    pumps[index].manualStartTime = now;
    pumps[index].manualDuration = MANUAL_SINGLE_PUMP_TIME;  // 2 minutes
    pumps[index].pumpStartTime = now;
    pumps[index].activationSource = ACTIVATION_SOURCE_MANUAL_SINGLE;
    
    // 🆕 START TIMER PROTECTION
    start_timer_protection(index, MANUAL_SINGLE_PUMP_TIME);
   
    set_pump_hardware(index, true);

    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[MANUAL] MANUAL ACTIVATION: %s (2 minutes, SINGLE)", 
            pumps[index].name);
    printf("%s\n", log_msg);
    on_pump_activated(index, true);
}

void manual_activate_all_pumps(void) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (emergencyStopActive) {
        printf("[MANUAL] Manual activation BLOCKED - Emergency stop active\n");
        return;
    }
    
    if (waterLockout) {
        printf("[MANUAL] Manual activation BLOCKED - Water lockout active\n");
        return;
    }

    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int activatedCount = 0;

    for (int i = 0; i < 4; i++) {
        // SECTION 3.7: Immediate restart allowed
        if (pumps[i].state == PUMP_AUTO_ACTIVE || 
            pumps[i].state == PUMP_MANUAL_ACTIVE) {
            deactivate_pump(i, "manual_all_override");
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        pumps[i].state = PUMP_MANUAL_ACTIVE;
        pumps[i].manualMode = true;
        pumps[i].manualStartTime = now;
        pumps[i].manualDuration = MANUAL_ALL_PUMPS_TIME;
        pumps[i].pumpStartTime = now;
        
        // SET MANUAL ALL ACTIVATION SOURCE
        pumps[i].activationSource = ACTIVATION_SOURCE_MANUAL_ALL;
        
         // 🆕 START TIMER PROTECTION FOR EACH PUMP
        start_timer_protection(i, MANUAL_ALL_PUMPS_TIME);

        set_pump_hardware(i, true);
        activatedCount++;
        on_pump_activated(i, true);
    }

    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[MANUAL] MANUAL ACTIVATION: ALL PUMPS (%d active, 90 seconds)", 
            activatedCount);
    printf("%s\n", log_msg);
}

void extend_manual_runtime(int index, unsigned long extensionTime) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (pumps[index].state != PUMP_MANUAL_ACTIVE) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[EXTEND] Cannot extend %s - Not in manual mode", 
                pumps[index].name);
        printf("%s\n", log_msg);
        return;
    }

    // Extend manual duration field
    pumps[index].manualDuration += extensionTime;

    // 🆕 EXTEND TIMER PROTECTION
    extend_timer_protection(index, extensionTime);
    
    unsigned long remaining = get_timer_remaining(index);
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[EXTEND] Extended %s by %lus (Total remaining: %lus)",
            pumps[index].name, 
            extensionTime/1000,
            remaining);
    printf("%s\n", log_msg);
}

void manual_stop_pump(int index) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[MANUAL] Pump %s already stopped", 
                pumps[index].name);
        printf("%s\n", log_msg);
        return;
    }

    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[MANUAL] MANUAL STOP: %s", 
            pumps[index].name);
    printf("%s\n", log_msg);
    deactivate_pump(index, "manual_stop");
}

// ========================================
// SHADOW-INTEGRATED MANUAL CONTROL FUNCTIONS
// ========================================

bool can_activate_pump_manually(int index) {
    if (index < 0 || index >= 4) {
        printf("[MANUAL] ERROR: Invalid pump index %d\n", index);
        return false;
    }
    
    if (emergencyStopActive) {
        printf("[MANUAL] Manual activation blocked for %s - Emergency stop active\n", 
               pumps[index].name);
        return false;
    }
    
    if (waterLockout) {
        printf("[MANUAL] Manual activation blocked for %s - Water lockout active\n", 
               pumps[index].name);
        return false;
    }
    
    return true;
}

bool shadow_manual_activate_pump(int index) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (!can_activate_pump_manually(index)) {
        return false;
    }
    
    manual_activate_pump(index); // This now starts timer automatically
    
    pumps[index].activationSource = ACTIVATION_SOURCE_SHADOW_SINGLE;
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
             "[SHADOW-MANUAL] Pump %s activated via shadow (2 min timer, PROTECTED)", 
             pumps[index].name);
    printf("%s\n", log_msg);
    
    return true;
}

/**
 * @brief Shadow command: Manually activate pump with custom duration
 * @param index Pump index (0-3)
 * @param durationMs Duration in milliseconds
 * @return true if successful, false otherwise
 */
bool shadow_manual_activate_pump_with_duration(int index, unsigned long durationMs) {
    char log_msg[LOG_BUFFER_SIZE];
    
    // Validate pump index
    if (index < 0 || index >= 4) {
        printf("[SHADOW-MANUAL] ERROR: Invalid pump index %d\n", index);
        return false;
    }
    
    // Check basic activation conditions
    if (!can_activate_pump_manually(index)) {
        return false;
    }
    
    // Check for water lockout
    if (waterLockout) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[SHADOW-MANUAL] Blocked: %s - Water lockout active (Level: %.1f%%)", 
                pumps[index].name, level_s);
        printf("%s\n", log_msg);
        return false;
    }
    
    // Check for emergency stop
    if (emergencyStopActive) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[SHADOW-MANUAL] Blocked: %s - Emergency stop active", 
                pumps[index].name);
        printf("%s\n", log_msg);
        return false;
    }
    
    // Stop pump if already running (immediate restart allowed)
    if (pumps[index].state == PUMP_MANUAL_ACTIVE || 
        pumps[index].state == PUMP_AUTO_ACTIVE) {
        snprintf(log_msg, LOG_BUFFER_SIZE,
                "[SHADOW-MANUAL] Restarting %s with new duration", 
                pumps[index].name);
        printf("%s\n", log_msg);
        deactivate_pump(index, "manual_restart");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Set pump to manual mode with custom duration
    pumps[index].state = PUMP_MANUAL_ACTIVE;
    pumps[index].manualMode = true;
    pumps[index].manualStartTime = now;
    pumps[index].manualDuration = durationMs;  // Custom duration
    pumps[index].pumpStartTime = now;
    pumps[index].activationSource = ACTIVATION_SOURCE_SHADOW_SINGLE;
    
    // Start timer protection with custom duration
    start_timer_protection(index, durationMs);
   
    // Turn on hardware
    set_pump_hardware(index, true);
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[SHADOW-MANUAL] Pump %s activated with %lu second timer (PROTECTED)", 
            pumps[index].name, (unsigned long)(durationMs/1000));
    printf("%s\n", log_msg);
    
    // Trigger callback
    on_pump_activated(index, true);
    
    return true;
}

bool shadow_manual_activate_all_pumps(void) {
    if (emergencyStopActive || waterLockout) {
        return false;
    }
    
    printf("[SHADOW-MANUAL] Activating ALL pumps with 90-second timers\n");
    
    manual_activate_all_pumps(); // This now starts timers automatically
    
    // Override activation sources
    for (int i = 0; i < 4; i++) {
        pumps[i].activationSource = ACTIVATION_SOURCE_SHADOW_ALL;
    }
    
    printf("[SHADOW-MANUAL] All pumps activated (90s timers, PROTECTED)\n");
    return true;
}

bool shadow_manual_stop_pump(int index) {
    char log_msg[LOG_BUFFER_SIZE];
    
    if (index < 0 || index >= 4) {
        printf("[SHADOW-MANUAL] ERROR: Invalid pump index %d\n", index);
        return false;
    }
    
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[SHADOW-MANUAL] Pump %s already stopped", 
                pumps[index].name);
        printf("%s\n", log_msg);
        return true;
    }
    
    manual_stop_pump(index);
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
             "[SHADOW-MANUAL] Pump %s stopped via shadow", 
             pumps[index].name);
    printf("%s\n", log_msg);
    
     if (pumps[index].timerProtected) {
        stop_timer_protection(index);
    }
    
    return true;
}

bool shadow_manual_stop_all_pumps(void) {
    printf("[SHADOW-MANUAL] Stopping all pumps via shadow\n");
    stop_all_pumps("shadow_manual_stop");
    return true;
}


// ========================================
// PUMP STATUS REPORTING FUNCTIONS
// ========================================

unsigned long get_pump_running_time(int index) {
    if (index < 0 || index >= 4) return 0;
    
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) {
        return 0;
    }
    
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long startTime = pumps[index].pumpStartTime;
    
    if (startTime == 0) return 0;
    
    return (now - startTime) / 1000;
}

unsigned long get_pump_remaining_time(int index) {
    if (index < 0 || index >= 4) return 0;
    
    // 🆕 USE TIMER-BASED REMAINING TIME
    if (pumps[index].timerProtected) {
        return get_timer_remaining(index);
    }
    
    // Fallback for non-timer pumps
    if (pumps[index].state != PUMP_MANUAL_ACTIVE) {
        return 0;
    }
    
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long elapsed = now - pumps[index].manualStartTime;
    
    if (elapsed >= pumps[index].manualDuration) {
        return 0;
    }
    
    return (pumps[index].manualDuration - elapsed) / 1000;
}



void get_pump_status_report(int index, PumpStatusReport* report) {
    if (index < 0 || index >= 4 || !report) return;
    
    report->pumpIndex = index;
    report->name = pumps[index].name;
    report->state = get_pump_state_string(index);
    report->isRunning = pumps[index].isRunning;
    report->manualMode = pumps[index].manualMode;
    report->runningTimeSeconds = get_pump_running_time(index);
    report->remainingTimeSeconds = get_pump_remaining_time(index);
    report->irValue = pumps[index].currentIRValue;
    report->sensorFault = pumps[index].sensorFault;
}

void get_all_pumps_status(PumpStatusReport reports[4]) {
    if (!reports) return;
    
    for (int i = 0; i < 4; i++) {
        get_pump_status_report(i, &reports[i]);
    }
}

// ========================================
// STATUS HELPERS
// ========================================

// Add this helper function to see activation sources
const char* get_activation_source_string(ActivationSource source) {
    switch(source) {
        case ACTIVATION_SOURCE_NONE: return "None";
        case ACTIVATION_SOURCE_AUTO: return "Auto";
        case ACTIVATION_SOURCE_MANUAL_SINGLE: return "Manual-Single";
        case ACTIVATION_SOURCE_MANUAL_ALL: return "Manual-All";
        case ACTIVATION_SOURCE_SHADOW_SINGLE: return "Shadow-Single";
        case ACTIVATION_SOURCE_SHADOW_ALL: return "Shadow-All";
        default: return "Unknown";
    }
}

const char* get_pump_state_string(int index) {
    if (index < 0 || index >= 4) return "INVALID-INDEX";
    
    if (pumps[index].state > 10) {
        // 🆕 SEND CRITICAL ALERT
        extern void send_alert_state_corruption(int, int);
        send_alert_state_corruption(index, pumps[index].state);
        
        return "CORRUPT-STATE";
    }
    
    if (waterLockout) return "DISABLED-WATER";
    if (emergencyStopActive) return "EMERGENCY-STOP";

    switch (pumps[index].state) {
        case PUMP_OFF: return "OFF";
        case PUMP_AUTO_ACTIVE: return "AUTO-ACTIVE";
        case PUMP_MANUAL_ACTIVE: return "MANUAL-ACTIVE";
        case PUMP_COOLDOWN: return "COOLDOWN";
        case PUMP_DISABLED: return "DISABLED";
        default: return "UNKNOWN";
    }
}

bool is_suppression_active(void) {
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_AUTO_ACTIVE || pumps[i].state == PUMP_MANUAL_ACTIVE) {
            return true;
        }
    }
    return false;
}



// ========================================
// CALLBACKS
// ========================================

void on_pump_activated(int index, bool isManual) {
    char log_msg[LOG_BUFFER_SIZE];
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
             "[FIRE_SYSTEM] Pump %s activated (%s)", 
             pumps[index].name, isManual ? "Manual" : "Auto");
    printf("%s\n", log_msg);
}

void on_pump_deactivated(int index, const char* reason) {
    char log_msg[LOG_BUFFER_SIZE];
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
             "[FIRE_SYSTEM] Pump %s deactivated - %s", 
             pumps[index].name, reason);
    printf("%s\n", log_msg);
}

void on_water_lockout_activated(void) {
    printf("[FIRE_SYSTEM] Water lockout activated - All manual buttons disabled\n");
}

void on_water_lockout_released(void) {
    printf("[FIRE_SYSTEM] Water lockout released - Manual and auto activation re-enabled\n");
}

void check_sensor_health(void) {
    char log_msg[LOG_BUFFER_SIZE];
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (now - lastSensorHealthCheck < SENSOR_HEALTH_INTERVAL) {
        return;
    }
    
    lastSensorHealthCheck = now;
    printf("[FIRE_SYSTEM] === SENSOR HEALTH CHECK ===\n");
    
    for (int i = 0; i < 4; i++) {
        bool healthy = is_sensor_healthy(i);
        
        if (!healthy && !pumps[i].sensorFault) {
            pumps[i].sensorFault = true;
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[FIRE_SYSTEM] SENSOR FAULT: %s IR sensor", 
                    pumps[i].name);
            printf("%s\n", log_msg);
            
            if (pumps[i].state == PUMP_AUTO_ACTIVE) {
                deactivate_pump(i, "sensor_fault");
            }
            
        } else if (healthy && pumps[i].sensorFault) {
            pumps[i].sensorFault = false;
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[FIRE_SYSTEM] Sensor %s RESTORED", 
                    pumps[i].name);
            printf("%s\n", log_msg);
        }
    }
    
    printf("[FIRE_SYSTEM] ===========================\n");
}

bool is_sensor_healthy(int index) {
    float irValue = pumps[index].currentIRValue;
    
    if (irValue < 0.0 || irValue > 105.0) {
        return false;
    }
    
    static float lastValues[4] = {-1, -1, -1, -1};
    static int stuckCount[4] = {0, 0, 0, 0};
    
    if (fabs(irValue - lastValues[index]) < 0.1) {
        stuckCount[index]++;
        if (stuckCount[index] > 10) {
            return false;
        }
    } else {
        stuckCount[index] = 0;
    }
    
    lastValues[index] = irValue;
    return true;
}

void on_flame_confirmed(int sensorIndex) {
    char log_msg[LOG_BUFFER_SIZE];
    
    snprintf(log_msg, LOG_BUFFER_SIZE, 
             "[FIRE_SYSTEM] Flame confirmed on %s (IR: %.1f%%) - Starting suppression", 
             pumps[sensorIndex].name, pumps[sensorIndex].currentIRValue);
    printf("%s\n", log_msg);
}

// ========================================
// DOOR MONITORING
// ========================================

void init_door_sensor(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DOOR_SENSOR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    doorOpen = (gpio_get_level(DOOR_SENSOR_PIN) == 0);
    
    
}

void update_camera_on_off(void) {
    bool fireDetected = false;

    for (int i = 0; i < 4; i++) {
        if (pumps[i].sensorFault) continue;

        if (pumps[i].currentIRValue > FIRE_THRESHOLD) {
            fireDetected = true;
            break;
        }
    }

    gpio_set_level(CAMERA_ON_OFF, fireDetected ? 1 : 0);
}

bool get_camera_status(void) {
    return gpio_get_level(CAMERA_ON_OFF) == 1;
}

bool is_camera_active(void) {
    return get_camera_status();
}

void check_door_status(void) {
    char log_msg[LOG_BUFFER_SIZE];
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (now - lastDoorCheck < DOOR_CHECK_INTERVAL) {
        return;
    }
    lastDoorCheck = now;
    
    bool currentState = (gpio_get_level(DOOR_SENSOR_PIN) == 0);
    
    if (currentState != doorOpen) {
        doorOpen = currentState;
        
        if (doorOpen) {
            doorOpenTime = now;
            printf("[FIRE_SYSTEM] Door OPENED\n");
        } else {
            unsigned long openDuration = (now - doorOpenTime) / 1000;
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[FIRE_SYSTEM] Door CLOSED (was open for %lu seconds)", 
                    openDuration);
            printf("%s\n", log_msg);
        }
    }
    
    static bool warningIssued = false;
    if (doorOpen && (now - doorOpenTime > DOOR_ALERT_DELAY)) {
        if (!warningIssued) {
            printf("[FIRE_SYSTEM] WARNING: Door has been open for over 5 minutes!\n");
            warningIssued = true;
        }
    } else {
        warningIssued = false;
    }
}

// ========================================
// SYSTEM INITIALIZATION
// ========================================

void init_fire_suppression_system(void) {
    char log_msg[LOG_BUFFER_SIZE];
    
    printf("\n");
    printf("========================================\n");
    printf("  GUARDIAN FIRE SUPPRESSION SYSTEM\n");
    printf("           INITIALIZING\n");
    printf("========================================\n");
    
    // Initialize arrays first
    initialize_arrays();
  
    snprintf(log_msg, LOG_BUFFER_SIZE, 
            "[FIRE_SYSTEM] Initializing PCA9555");
    printf("%s\n", log_msg);
    
    printf("%s\n", log_msg);
    
    esp_err_t ret = pca9555_init(&pca_dev, 
                                 PCA9555_I2C_ADDRESS,
                                 PCA9555_I2C_PORT,
                                 PCA9555_I2C_SDA_GPIO,
                                 PCA9555_I2C_SCL_GPIO);
    
    if (ret != ESP_OK) {
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[FIRE_SYSTEM] PCA9555 initialization failed: %s", 
                esp_err_to_name(ret));
        printf("%s\n", log_msg);
        snprintf(log_msg, LOG_BUFFER_SIZE, 
                "[FIRE_SYSTEM]  Check I2C wiring and address!");
        printf("%s\n", log_msg);
        
        // 🆕 SEND CRITICAL ALERT
        send_alert_pca9555_fail(esp_err_to_name(ret), 
            "PCA9555 I2C initialization failed - Check wiring and address");
    }else {
        printf("[FIRE_SYSTEM] PCA9555 initialized successfully\n");
        
        ret = pca9555_configure_all_outputs(&pca_dev);
        if (ret != ESP_OK) {
            snprintf(log_msg, LOG_BUFFER_SIZE, 
                    "[FIRE_SYSTEM] Failed to configure PCA9555 outputs: %s", 
                    esp_err_to_name(ret));
            printf("%s\n", log_msg);
        } else {
            printf("[FIRE_SYSTEM] PCA9555 ports configured as outputs\n");
        }
        
        all_off();
    }

    for (int i = 0; i < 4; i++) {
        pumps[i].sensorFault = false;
    }

    gpio_config_t camera_conf = {
        .pin_bit_mask = (1ULL << CAMERA_ON_OFF),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&camera_conf);
    gpio_set_level(CAMERA_ON_OFF, 0);

    init_current_sensors();
    init_door_sensor(); 

    printf("[FIRE_SYSTEM] Hardware initialized\n");
    printf("%s\n", log_msg);
    printf("[FIRE_SYSTEM] System ARMED and ready\n");
    printf("========================================\n\n");
}



void reset_system_to_defaults(void) {
    printf("\n[SYSTEM] ===== RESETTING SYSTEM TO DEFAULTS =====\n");
    
    // 1. Clear emergency stop if active
    if (emergencyStopActive) {
        printf("[SYSTEM] Clearing emergency stop...\n");
        emergencyStopActive = false;
    }
    
    // 2. Force stop ALL pumps (override all protections)
    printf("[SYSTEM] Stopping all pumps...\n");
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state != PUMP_OFF) {
            // Clear timer protection
            if (pumps[i].timerProtected) {
                stop_timer_protection(i);
            }
            
            // Force hardware OFF
            set_pump_hardware(i, false);
            
            // Reset pump state
            pumps[i].state = PUMP_OFF;
            pumps[i].isRunning = false;
            pumps[i].manualMode = false;
            pumps[i].flameConfirmed = false;
            pumps[i].flameFirstDetectedTime = 0;
            pumps[i].lastFlameSeenTime = 0;
            pumps[i].pumpStartTime = 0;
            pumps[i].cooldownStartTime = 0;
            pumps[i].cooldownDuration = 0;
            pumps[i].manualStartTime = 0;
            pumps[i].manualDuration = 0;
            pumps[i].activationSource = ACTIVATION_SOURCE_NONE;
            pumps[i].lastStopReason = STOP_REASON_NONE;
            pumps[i].stateBeforeEmergency = PUMP_OFF;
            pumps[i].wasRunningBeforeEmergency = false;
            pumps[i].emergencyStopTime = 0;
            
            printf("[SYSTEM] Pump %d (%s) reset to OFF\n", i+1, pumps[i].name);
        }
    }
    
    // 3. Reset profile to default (WILDLAND_STANDARD)
    printf("[SYSTEM] Resetting profile to WILDLAND_STANDARD...\n");
    currentProfile = WILDLAND_STANDARD;
    
    // 4. Clear water lockout (allow system to re-evaluate)
    if (waterLockout) {
        printf("[SYSTEM] Clearing water lockout...\n");
        waterLockout = false;
        waterStable = false;
        waterAboveResumeTime = 0;
        inGracePeriod = false;
        gracePeriodStartTime = 0;
    }
    
    // 5. Reset continuous feed detection
    continuousWaterFeed = false;
    continuousFeedConfidence = 0;
    lastContinuousFeedCheck = 0;
    
    // 6. Arm the system
    systemArmed = true;
    
    // 7. Reset sensor fault flags
    for (int i = 0; i < 4; i++) {
        pumps[i].sensorFault = false;
    }
    
    // 8. Reset flame validation states
    for (int i = 0; i < 4; i++) {
        flameStartTime[i] = 0;
        flameValidating[i] = false;
    }
    
    printf("[SYSTEM] ===== SYSTEM RESET COMPLETE =====\n");
    printf("[SYSTEM] - All pumps: OFF\n");
    printf("[SYSTEM] - Profile: WILDLAND_STANDARD\n");
    printf("[SYSTEM] - Emergency Stop: Cleared\n");
    printf("[SYSTEM] - Water Lockout: Cleared\n");
    printf("[SYSTEM] - System Armed: YES\n");
    printf("[SYSTEM] ==========================================\n\n");
}

// ========================================
// FIRE DETECTION TYPE FUNCTIONS
// ========================================

/**
 * @brief Get the string representation of a fire detection type
 * @param type The FireDetectionType enum value
 * @return String describing the fire type
 */
const char* get_fire_detection_type_string(FireDetectionType type) {
    switch(type) {
        case FIRE_TYPE_NONE:             return "NONE";
        case FIRE_TYPE_SINGLE_SECTOR:    return "SINGLE_SECTOR";
        case FIRE_TYPE_MULTIPLE_SECTORS: return "MULTIPLE_SECTORS";
        case FIRE_TYPE_FULL_SYSTEM:      return "FULL_SYSTEM";
        default:                         return "UNKNOWN";
    }
}

/**
 * @brief Update the fire detection info based on current sensor readings
 *        Call this function periodically or when fire status changes
 */
void update_fire_detection_info(void) {
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Get current IR sensor values
    float sensorValues[4] = {ir_s1, ir_s2, ir_s3, ir_s4};
    const char* sectorNames[4] = {"N", "S", "E", "W"};
    
    // Reset fire info
    int activeCount = 0;
    currentFireInfo.activeSectorNames[0] = '\0';
    
    // Check each sector - uses global FIRE_THRESHOLD from fire_system.h
    for (int i = 0; i < 4; i++) {
        currentFireInfo.sectorsActive[i] = (sensorValues[i] > FIRE_THRESHOLD);
        if (currentFireInfo.sectorsActive[i]) {
            activeCount++;
            
            // Build sector names string
            if (strlen(currentFireInfo.activeSectorNames) > 0) {
                strncat(currentFireInfo.activeSectorNames, ",", 
                       sizeof(currentFireInfo.activeSectorNames) - strlen(currentFireInfo.activeSectorNames) - 1);
            }
            strncat(currentFireInfo.activeSectorNames, sectorNames[i],
                   sizeof(currentFireInfo.activeSectorNames) - strlen(currentFireInfo.activeSectorNames) - 1);
        }
    }
    
    currentFireInfo.activeSectorCount = activeCount;
    currentFireInfo.lastUpdateTime = now;
    
    // Determine fire detection type
    switch(activeCount) {
        case 0:
            currentFireInfo.type = FIRE_TYPE_NONE;
            break;
        case 1:
            currentFireInfo.type = FIRE_TYPE_SINGLE_SECTOR;
            break;
        case 4:
            currentFireInfo.type = FIRE_TYPE_FULL_SYSTEM;
            break;
        default:  // 2 or 3 sectors
            currentFireInfo.type = FIRE_TYPE_MULTIPLE_SECTORS;
            break;
    }
}

/**
 * @brief Get the current fire detection type
 * @return Current FireDetectionType enum value
 */
FireDetectionType get_fire_detection_type(void) {
    update_fire_detection_info();
    return currentFireInfo.type;
}

/**
 * @brief Get the number of sectors currently on fire
 * @return Number of active fire sectors (0-4)
 */
int get_active_fire_sector_count(void) {
    update_fire_detection_info();
    return currentFireInfo.activeSectorCount;
}

/**
 * @brief Get a comma-separated string of active fire sector names
 * @return String like "N", "N,S", "N,S,E,W" etc.
 */
const char* get_active_sectors_string(void) {
    update_fire_detection_info();
    return currentFireInfo.activeSectorNames;
}

/**
 * @brief Check if a specific sector is on fire
 * @param sectorIndex Sector index (0=N, 1=S, 2=E, 3=W)
 * @return true if sector has fire detected
 */
bool is_sector_on_fire(int sectorIndex) {
    if (sectorIndex < 0 || sectorIndex >= 4) return false;
    update_fire_detection_info();
    return currentFireInfo.sectorsActive[sectorIndex];
}

/**
 * @brief Get pointer to the current fire detection info structure
 * @return Pointer to FireDetectionInfo struct
 */
FireDetectionInfo* get_fire_detection_info(void) {
    return &currentFireInfo;
}

// ========================================
// MAIN UPDATE FUNCTION
// ========================================

void update_fire_suppression_system(void) {
    get_sensor_data();
    check_door_status(); 
    check_sensor_health();
    
    // Update fire detection type info
    update_fire_detection_info();
    
    if (systemArmed && !waterLockout && !emergencyStopActive) {
        check_automatic_activation();
    }
    
    update_camera_on_off();
    update_pump_states();
}


