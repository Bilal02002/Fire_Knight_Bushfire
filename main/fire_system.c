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
#include "esp_log.h"

// ========================================
// GLOBAL VARIABLE DEFINITIONS
// ========================================
#define DIODE_DROP 0.3f
#define REVERSE_RATIO 12.11f
#define INVERTER_DIVIDER_RATIO  11.0f   // (10K + 1K) / 1K
#define CURRENT_CALIBRATION_FACTOR 0.65f 
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

// Inverter Voltages (4 inverters)
// Derived from adc_array2[2..5] scaled by x360 (same as bushfire_mqtt_v2_final)
float volt1 = 0, volt2 = 0, volt3 = 0, volt4 = 0;

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
    .detectedSectors = {false, false, false, false},
    .detectedSectorCount = 0,
    .detectedSectorNames = "",
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
PumpState savedPumpStates[4] = {PUMP_OFF};
bool savedRunningStates[4] = {false};
static unsigned long savedManualTimes[4] = {0};
static unsigned long savedManualDurations[4] = {0};

// Flame confirmation tracking
static unsigned long flameStartTime[4] = {0, 0, 0, 0};
static bool flameValidating[4] = {false, false, false, false};

// Water stability tracking
static float lastStableWaterLevel = 0;
static unsigned long stableStartTime = 0;
   
pca9555_t pca_dev;

// ========================================
// INITIALIZATION FUNCTIONS
// ========================================

void initialize_arrays(void) {
    ESP_LOGI("FIRE_SYSTEM", "Initializing system arrays...");
    
    // Initialize Current Sensors
    currentSensors[0] = (CurrentSensor){"CT1", MUX_OUTPUT_PIN, true, 6, 0.0, 0.0, false, 0};
    currentSensors[1] = (CurrentSensor){"CT2", MUX_OUTPUT_PIN, true, 7, 0.0, 0.0, false, 0};
    currentSensors[2] = (CurrentSensor){"CT3", SENSOR1_PIN, false, -1, 0.0, 0.0, false, 0};
    currentSensors[3] = (CurrentSensor){"CT4", SENSOR2_PIN, false, -1, 0.0, 0.0, false, 0};
    
// 10.1 Wildland – Standard
    profiles[WILDLAND_STANDARD] = (ProfileConfig){
        .autoModeFull    = false,     // (A) Auto Mode: Sector
        .mode            = "sector",  // Default mode derived from autoModeFull
        .noFlameTimeout  = 60000,     // (B) No-Flame Timeout: 60 seconds
        .maxRunCapSector = 360000,    // (C) Max Run Cap Sector: 6 minutes //360000
        .maxRunCapFull   = 180000,    // (D) Max Run Cap Full:   3 minutes //180000
        .name            = "Wildland-Standard",
        .cooldown        = 30000
    };

    // 10.2 Wildland – High Wind
    profiles[WILDLAND_HIGH_WIND] = (ProfileConfig){
        .autoModeFull    = true,      // (A) Auto Mode: Full
        .mode            = "full",    // Default mode derived from autoModeFull
        .noFlameTimeout  = 45000,     // (B) No-Flame Timeout: 45 seconds
        .maxRunCapSector = 480000,    // (C) Max Run Cap Sector: 8 minutes
        .maxRunCapFull   = 240000,    // (D) Max Run Cap Full:   4 minutes
        .name            = "Wildland-HighWind",
        .cooldown        = 30000
    };

    // 10.3 Industrial / Mining – Hydrocarbon
    profiles[INDUSTRIAL_HYDROCARBON] = (ProfileConfig){
        .autoModeFull    = false,     // (A) Auto Mode: Sector
        .mode            = "sector",  // Default mode derived from autoModeFull
        .noFlameTimeout  = 60000,     // (B) No-Flame Timeout: 60 seconds
        .maxRunCapSector = 600000,    // (C) Max Run Cap Sector: 10 minutes
        .maxRunCapFull   = 300000,    // (D) Max Run Cap Full:   5 minutes
        .name            = "Industrial-Hydrocarbon",
        .cooldown        = 30000
    };

    // 10.4 Critical Asset
    profiles[CRITICAL_ASSET] = (ProfileConfig){
        .autoModeFull    = false,     // (A) Auto Mode: Sector
        .mode            = "sector",  // Default mode derived from autoModeFull
        .noFlameTimeout  = 60000,     // (B) No-Flame Timeout: 60 seconds
        .maxRunCapSector = 480000,    // (C) Max Run Cap Sector: 8 minutes
        .maxRunCapFull   = 240000,    // (D) Max Run Cap Full:   4 minutes
        .name            = "Critical-Asset",
        .cooldown        = 30000
    };

    // 10.5 Continuous-Feed Site
    profiles[CONTINUOUS_FEED] = (ProfileConfig){
        .autoModeFull    = false,     // (A) Auto Mode: Sector
        .mode            = "sector",  // Default mode derived from autoModeFull
        .noFlameTimeout  = 60000,     // (B) No-Flame Timeout: 60 seconds
        .maxRunCapSector = 0,         // (C) Caps lifted - no limit
        .maxRunCapFull   = 0,         // (D) Caps lifted - no limit
        .name            = "Continuous-Feed",
        .cooldown        = 0
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
        .hwTimerHandle = NULL,
        .hwTimerExpired = false,
        .stopPumpRequested = false,
        .activatedInFullSystemMode = false,
        // 0–20mA decoded state fields
        .flameSignalState = FLAME_SIGNAL_NORMAL,
        .bitFaultCount = 0,
        .inAuxBit = false,
        .auxBitStartTime = 0
    };
}
    
    ESP_LOGI("FIRE_SYSTEM", "Arrays initialized successfully with corrected profiles");
}

// ========================================
// TIMER PROTECTION FUNCTIONS
// ========================================
//
// These use ESP-IDF's esp_timer API, which is backed by the ESP32's
// hardware timer/RTC peripheral. Each pump gets its own one-shot hardware
// timer that fires an interrupt-driven callback at the exact microsecond
// its duration expires (setting hwTimerExpired), instead of the previous
// approach of comparing xTaskGetTickCount()*portTICK_PERIOD_MS in a
// polling loop (a software "millis()"-style counter).
//
// The callback itself only sets a flag - it deliberately does NOT touch
// pump state or hardware directly, because esp_timer callbacks run in a
// separate FreeRTOS task (esp_timer service task) outside of
// mutexPumpState/mutexWaterState. Actual deactivation still happens in
// task_pump_management()'s normal mutex-protected flow, which now checks
// the hardware timer's flag via is_timer_expired() instead of polling
// tick-based time.

static void pump_hw_timer_callback(void* arg) {
    int index = (int)(intptr_t)arg;
    if (index < 0 || index >= 4) return;
    pumps[index].hwTimerExpired = true;
}

// Creates (once) the hardware timer object for a pump. Safe to call
// repeatedly - only actually creates the underlying esp_timer the first time.
static bool ensure_pump_hw_timer_created(int index) {
    if (pumps[index].hwTimerHandle != NULL) {
        return true;
    }

    const esp_timer_create_args_t timerArgs = {
        .callback = &pump_hw_timer_callback,
        .arg = (void*)(intptr_t)index,
        .dispatch_method = ESP_TIMER_TASK,
        .name = pumps[index].name
    };

    esp_err_t err = esp_timer_create(&timerArgs, &pumps[index].hwTimerHandle);
    if (err != ESP_OK) {
        ESP_LOGE("FIRE_SYSTEM", "%s: Failed to create hardware timer (err=%d)",
                 pumps[index].name, err);
        pumps[index].hwTimerHandle = NULL;
        return false;
    }
    return true;
}

void start_timer_protection(int index, unsigned long duration) {
    if (index < 0 || index >= 4) return;

    if (!ensure_pump_hw_timer_created(index)) {
        // Fall back is not possible without a hardware timer; mark
        // unprotected rather than silently using software timing.
        pumps[index].timerProtected = false;
        return;
    }

    // Stop any timer already running for this pump before (re)starting.
    esp_timer_stop(pumps[index].hwTimerHandle); // no-op / harmless if not running

    int64_t nowUs = esp_timer_get_time();       // hardware timer, microseconds since boot
    unsigned long now = (unsigned long)(nowUs / 1000);

    pumps[index].timerProtected = true;
    pumps[index].timerEndTime = now + duration; // ms, hardware-clock based
    pumps[index].originalDuration = duration;
    pumps[index].timerDuration = duration;
    pumps[index].protectionTimeRemaining = duration;
    pumps[index].hwTimerExpired = false;

    esp_err_t err = esp_timer_start_once(pumps[index].hwTimerHandle, (uint64_t)duration * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE("FIRE_SYSTEM", "%s: Failed to start hardware timer (err=%d)",
                 pumps[index].name, err);
    }

    ESP_LOGI("FIRE_SYSTEM", "%s: Hardware timer protection started for %lu seconds",
             pumps[index].name, duration/1000);
}

bool is_timer_expired(int index) {
    if (index < 0 || index >= 4) return true;
    
    if (!pumps[index].timerProtected) {
        return true; // No timer active
    }

    // Primary signal: the hardware timer's own callback firing.
    if (pumps[index].hwTimerExpired) {
        return true;
    }

    // Secondary check against the hardware clock, in case the callback's
    // dispatch to the esp_timer task hasn't been scheduled yet.
    int64_t nowUs = esp_timer_get_time();
    unsigned long now = (unsigned long)(nowUs / 1000);
    return (now >= pumps[index].timerEndTime);
}

 unsigned long get_timer_remaining(int index) {
    if (index < 0 || index >= 4) return 0;
    
    if (!pumps[index].timerProtected) {
        return 0;
    }

    int64_t nowUs = esp_timer_get_time();
    unsigned long now = (unsigned long)(nowUs / 1000);
    
    if (now >= pumps[index].timerEndTime) {
        return 0;
    }
    unsigned long remaining = (pumps[index].timerEndTime - now) / 1000;
    pumps[index].protectionTimeRemaining = remaining * 1000;
    
     return remaining;
}

void extend_timer_protection(int index, unsigned long extensionTime) {
    if (index < 0 || index >= 4) return;
    
    if (!pumps[index].timerProtected) {
        ESP_LOGI("FIRE_SYSTEM", "%s: No active timer to extend", pumps[index].name);
        return;
    }

    pumps[index].timerEndTime += extensionTime;

    // Re-arm the hardware timer for the new remaining duration so the
    // callback still fires at the correct (extended) expiry moment.
    if (pumps[index].hwTimerHandle != NULL) {
        esp_timer_stop(pumps[index].hwTimerHandle); // harmless if already fired/stopped

        int64_t nowUs = esp_timer_get_time();
        unsigned long now = (unsigned long)(nowUs / 1000);
        unsigned long remainingMs = (pumps[index].timerEndTime > now)
                                         ? (pumps[index].timerEndTime - now)
                                         : 1; // guard against 0 (esp_timer requires > 0)

        pumps[index].hwTimerExpired = false;
        esp_err_t err = esp_timer_start_once(pumps[index].hwTimerHandle, (uint64_t)remainingMs * 1000ULL);
        if (err != ESP_OK) {
            ESP_LOGE("FIRE_SYSTEM", "%s: Failed to re-arm hardware timer on extend (err=%d)",
                     pumps[index].name, err);
        }
    }

    unsigned long remaining = get_timer_remaining(index);
    ESP_LOGI("FIRE_SYSTEM", "%s: Timer extended by %lu seconds (New remaining: %lu seconds)",
             pumps[index].name, extensionTime/1000, remaining);
}

void stop_timer_protection(int index) {
    if (index < 0 || index >= 4) return;

    if (pumps[index].hwTimerHandle != NULL) {
        esp_timer_stop(pumps[index].hwTimerHandle); // harmless if not running
    }

    pumps[index].timerProtected = false;
    pumps[index].timerEndTime = 0;
    pumps[index].originalDuration = 0;
    pumps[index].hwTimerExpired = false;
    ESP_LOGI("FIRE_SYSTEM", "Hardware timer protection stopped for %s", pumps[index].name);
}


// ========================================
// CONTINUOUS FEED DETECTION
// ========================================

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

    static float feedCheckLevels[6] = {0};  // Track last 6 readings (1 minute)
    static int feedCheckIndex = 0;

    // ── Gate: only meaningful while at least one pump is consuming water ──
    // If no pump is running:
    //   - Rising water   = tank being refilled, not continuous feed
    //   - Stable water   = idle, tells us nothing
    //   - Falling water  = leak/drain, not continuous feed
    // In all idle cases we reset the buffer so stale readings don't
    // carry over into the next active suppression window.
    bool anyPumpRunning = false;
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state == PUMP_AUTO_ACTIVE ||
            pumps[i].state == PUMP_MANUAL_ACTIVE) {
            anyPumpRunning = true;
            break;
        }
    }

    if (!anyPumpRunning) {
        // Reset buffer so the next active window starts fresh
        for (int i = 0; i < 6; i++) feedCheckLevels[i] = 0;
        feedCheckIndex = 0;

        // If auto-detection had previously set continuousWaterFeed,
        // clear it now that no pump is running (profile-based setting
        // is handled separately in apply_system_profile and is preserved
        // because currentProfile == CONTINUOUS_FEED will re-assert it).
        if (continuousWaterFeed && currentProfile != CONTINUOUS_FEED) {
            continuousFeedConfidence--;
            if (continuousFeedConfidence <= 0) {
                continuousWaterFeed = false;
                continuousFeedConfidence = 0;
                ESP_LOGI("FIRE_SYSTEM", "Continuous feed CLEARED - no pumps running");
            }
        }
        return;
    }

    // ── At least one pump is running - evaluate water level behaviour ──
    float currentLevel = level_s;

    // Store current level
    feedCheckLevels[feedCheckIndex] = currentLevel;
    feedCheckIndex = (feedCheckIndex + 1) % 6;

    // Need a full buffer (6 readings = 1 minute) before making a decision
    bool bufferFull = true;
    for (int i = 0; i < 6; i++) {
        if (feedCheckLevels[i] == 0) {
            bufferFull = false;
            break;
        }
    }

    if (!bufferFull) {
        ESP_LOGI("FIRE_SYSTEM", "Continuous feed: accumulating readings...");
        return;
    }

    // Check if water level is stable or rising despite pump(s) consuming it
    bool consistentReplenishment = true;
    for (int i = 1; i < 6; i++) {
        float prev = feedCheckLevels[(feedCheckIndex + i - 1) % 6];
        float curr = feedCheckLevels[(feedCheckIndex + i) % 6];
        if (curr < prev - 2.0f) {  // More than 2% drop between readings
            consistentReplenishment = false;
            break;
        }
    }

    if (consistentReplenishment && !continuousWaterFeed) {
        continuousWaterFeed = true;
        continuousFeedConfidence = 6;
        ESP_LOGI("FIRE_SYSTEM", "CONTINUOUS WATER FEED DETECTED - MCRC lifted");
    }
    else if (!consistentReplenishment && continuousWaterFeed &&
             currentProfile != CONTINUOUS_FEED) {
        // Only auto-clear if not set by profile
        continuousFeedConfidence--;
        if (continuousFeedConfidence <= 0) {
            continuousWaterFeed = false;
            continuousFeedConfidence = 0;
            ESP_LOGI("FIRE_SYSTEM", "Continuous feed LOST - MCRC restored");
        }
    }

    lastWaterLevelForFeed = currentLevel;
}

// ========================================
// HARDWARE CONTROL FUNCTIONS
// ========================================

void pump_control(unsigned int pumpNum, bool state) {
    if (pumpNum < 1 || pumpNum > 4) {
        ESP_LOGE("FIRE_SYSTEM", "ERROR: Invalid pump number %d", pumpNum);
        return;
    }
    
    int index = pumpNum - 1;
    set_pump_hardware(index, state);
}

void all_off(void) {
    esp_err_t ret1 = pca9555_set_port0_output(&pca_dev, 0x00);
    esp_err_t ret2 = pca9555_set_port1_output(&pca_dev, 0x00);
    
    if (ret1 == ESP_OK && ret2 == ESP_OK) {
        ESP_LOGI("FIRE_SYSTEM", "All PCA9555 outputs set to OFF");
    } else {
        ESP_LOGE("FIRE_SYSTEM", "PCA9555 shutdown failed: Port0=%s, Port1=%s", 
                 esp_err_to_name(ret1), esp_err_to_name(ret2));
    }
    
    for (int i = 0; i < 4; i++) {
        pumps[i].isRunning = false;
    }
}

void apply_system_profile(SystemProfile newProfile) {
    ESP_LOGI("FIRE_SYSTEM", "===== APPLYING PROFILE CHANGE =====");
    ESP_LOGI("FIRE_SYSTEM", "Switching from profile %d to %d", 
             currentProfile, newProfile);
    ESP_LOGI("FIRE_SYSTEM", "From: %s", profiles[currentProfile].name);
    ESP_LOGI("FIRE_SYSTEM", "To:   %s", profiles[newProfile].name);
    
    SystemProfile oldProfile = currentProfile;
    currentProfile = newProfile;
    
    ProfileConfig* newConfig = &profiles[newProfile];
    
    if (oldProfile != newProfile) {
        // Reset mode to this profile's declared default. Any shadow override that was
        // applied to the previous profile does NOT carry over - each profile always
        // (re)loads with mode = autoModeFull ? "full" : "sector" the moment it's applied.
        strncpy(newConfig->mode, newConfig->autoModeFull ? "full" : "sector",
                sizeof(newConfig->mode) - 1);
        newConfig->mode[sizeof(newConfig->mode) - 1] = '\0';
    }
    
    ESP_LOGI("FIRE_SYSTEM", "New Configuration:");
    ESP_LOGI("FIRE_SYSTEM", "- Suppression Mode: %s", newConfig->mode);
    ESP_LOGI("FIRE_SYSTEM", "- No Flame Timeout: %lu ms (%lu seconds)", 
             newConfig->noFlameTimeout, newConfig->noFlameTimeout/1000);
    ESP_LOGI("FIRE_SYSTEM", "- Max Run Cap Full: %lu ms (%lu minutes)", 
             newConfig->maxRunCapFull, newConfig->maxRunCapFull/60000);
    ESP_LOGI("FIRE_SYSTEM", "- Max Run Cap Sector: %lu ms (%lu minutes)", 
             newConfig->maxRunCapSector, newConfig->maxRunCapSector/60000);
    
    if (oldProfile != newProfile) {
        if (newProfile == CONTINUOUS_FEED) {
            continuousWaterFeed = true;
            ESP_LOGI("FIRE_SYSTEM", "Continuous water feed ENABLED (profile)");
        } else if (oldProfile == CONTINUOUS_FEED) {
            // Only disable if not detected by hardware
            if (continuousFeedConfidence < 3) {
                continuousWaterFeed = false;
                ESP_LOGI("FIRE_SYSTEM", "Continuous water feed DISABLED");
            }
        }
        
        if (strcmp(newConfig->mode, "full") == 0) {
            ESP_LOGI("FIRE_SYSTEM", "FULL-SYSTEM MODE: All pumps will activate on fire detection");
        } else {
            ESP_LOGI("FIRE_SYSTEM", "SECTOR MODE: Only affected pump will activate");
        }
        
        if (is_suppression_active()) {
            ESP_LOGI("FIRE_SYSTEM", "Stopping all active pumps due to profile change");
            stop_all_pumps("profile_change");
        }
    }
    
    ESP_LOGI("FIRE_SYSTEM", "Profile application COMPLETE");
    ESP_LOGI("FIRE_SYSTEM", "=====================================");
}

// ========================================
// SUPPRESSION MODE FUNCTIONS (sector vs full)
// ========================================
// The mode belongs to the currently active profile. It defaults to
// autoModeFull ? "full" : "sector" whenever a profile is (re)applied
// (see apply_system_profile above), and can be overridden at runtime
// via the device shadow using set_suppression_mode(). Switching to a
// different profile and back resets it to that profile's default again.

void set_suppression_mode(const char* mode) {
    if (!mode) return;
    if (strcmp(mode, "full") == 0) {
        strncpy(profiles[currentProfile].mode, "full", sizeof(profiles[currentProfile].mode) - 1);
    } else {
        strncpy(profiles[currentProfile].mode, "sector", sizeof(profiles[currentProfile].mode) - 1);
    }
    profiles[currentProfile].mode[sizeof(profiles[currentProfile].mode) - 1] = '\0';
    ESP_LOGI("FIRE_SYSTEM", "Suppression mode for %s set to: %s",
             profiles[currentProfile].name, profiles[currentProfile].mode);
}

const char* get_suppression_mode(void) {
    return profiles[currentProfile].mode;
}

void set_pump_hardware(int index, bool state) {
    if (index < 0 || index >= 4) {
        ESP_LOGE("FIRE_SYSTEM", "ERROR: Invalid pump index %d", index);
        return;
    }
    
    if (emergencyStopActive && state) {
        ESP_LOGI("FIRE_SYSTEM", "BLOCKED: Cannot activate %s - Emergency stop active", pumps[index].name);
        return;
    }
    
    ESP_LOGI("FIRE_SYSTEM", "Setting %s to %s", pumps[index].name, state ? "ON" : "OFF");
    
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
        ESP_LOGE("FIRE_SYSTEM", "CONTROL FAILED: %s - %s", pumps[index].name, esp_err_to_name(ret));
        return;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    uint8_t port_state;
    ret = pca9555_read_port1_output(&pca_dev, &port_state);
    
    if (ret == ESP_OK) {
        bool actual_state = (port_state & (1 << pca_pin)) != 0;
        pumps[index].isRunning = actual_state;
        
        if (actual_state != state) {
            ESP_LOGW("FIRE_SYSTEM", "VERIFICATION FAILED: %s commanded %s but PCA shows %s (Port 1, Pin %d)", 
                     pumps[index].name, 
                     state ? "ON" : "OFF",
                     actual_state ? "ON" : "OFF",
                     pca_pin);
                     
            ESP_LOGI("FIRE_SYSTEM", "Attempting recovery...");
            pca9555_set_pin_state(&pca_dev, 1, pca_pin, state);
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            ESP_LOGI("FIRE_SYSTEM", "SUCCESS: %s is %s (Port 1, Pin %d)", 
                     pumps[index].name, 
                     state ? "ON" : "OFF",
                     pca_pin);
        }
    } else {
        ESP_LOGE("FIRE_SYSTEM", "READBACK FAILED: Cannot read PCA9555 - %s", esp_err_to_name(ret));
        pumps[index].isRunning = state;
    }
}

// ========================================
// EMERGENCY STOP FUNCTIONS
// ========================================

void save_current_pump_states(void) {
    ESP_LOGI("FIRE_SYSTEM", "Saving current pump states...");
    
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
        
        ESP_LOGI("FIRE_SYSTEM", "Pump %d: State=%d, Running=%d", 
                 i+1, savedPumpStates[i], savedRunningStates[i]);
    }
}


// ==========================================
// UPDATED: emergency_stop_all_pumps()
// ==========================================

void emergency_stop_all_pumps(StopReason reason) {
    ESP_LOGI("FIRE_SYSTEM", "===== EMERGENCY STOP ACTIVATED =====");
    ESP_LOGI("FIRE_SYSTEM", "Reason: %d (%s)", reason, get_stop_reason_string(reason));
    
      if (startAllPumpsActive) {
        startAllPumps_stop_reason = STOP_REASON_START_ALL_EMERGENCY_STOP;
        ESP_LOGI("FIRE_SYSTEM", "Captured stop reason: EMERGENCY_STOP");
    }
    if (reason == STOP_REASON_EMERGENCY_STOP || reason == STOP_REASON_SHADOW_COMMAND) {
        save_current_pump_states();
    }
    
    const char* reason_str = get_stop_reason_string(reason);
    
    // ✅ FORCE STOP ALL PUMPS - Including timer-protected ones
    ESP_LOGI("FIRE_SYSTEM", "Stopping ALL pumps (including timer-protected)");
    
    for (int i = 0; i < 4; i++) {
        if (pumps[i].state != PUMP_OFF && pumps[i].state != PUMP_DISABLED) {
            
            // Log if we're overriding a timer
            if (pumps[i].timerProtected && !is_timer_expired(i)) {
                unsigned long remaining = get_timer_remaining(i);
                ESP_LOGI("FIRE_SYSTEM", "Overriding timer protection on %s (%lu sec remaining)",
                         pumps[i].name, remaining);
            }
            
            pumps[i].lastStopReason = reason;
            pumps[i].emergencyStopTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            
            // Deactivate will now allow emergency stop to override timer
            deactivate_pump(i, reason_str);
        }
    }
    
    emergencyStopActive = true;
    
    // ✅ NEW: Clear startAllPumpsActive flag (declared as extern in main.c)
    
    if (startAllPumpsActive) {
        ESP_LOGI("FIRE_SYSTEM", "Emergency stop - clearing startAllPumpsActive flag");
        startAllPumpsActive = false;
    }
    
    ESP_LOGI("FIRE_SYSTEM", "All pumps stopped. Reason: %s", reason_str);
    ESP_LOGI("FIRE_SYSTEM", "====================================");
}

void restore_pumps_after_emergency(void) {
    ESP_LOGI("FIRE_SYSTEM", "===== RESTORING PUMPS AFTER EMERGENCY =====");
    
    if (!emergencyStopActive) {
        ESP_LOGI("FIRE_SYSTEM", "No emergency stop active");
        return;
    }
    
    emergencyStopActive = false;
    
    bool waterLocked = waterLockout;
    for (int i = 0; i < 4; i++) {
        if (waterLocked) {
            ESP_LOGI("FIRE_SYSTEM", "Pump %d remains stopped due to WATER LOCKOUT", i+1);
            continue;
        }
        
        if (pumps[i].sensorFault) {
            ESP_LOGI("FIRE_SYSTEM", "Pump %d remains stopped due to SENSOR FAULT", i+1);
            continue;
        }
        
        PumpState targetState = savedPumpStates[i];
        
        switch(targetState) {
            case PUMP_AUTO_ACTIVE:
                pumps[i].state = PUMP_OFF;
                pumps[i].flameFirstDetectedTime = 0;
                pumps[i].flameConfirmed = false;
                ESP_LOGI("FIRE_SYSTEM", "Pump %d restored to AUTO mode (will reactivate if fire detected)", i+1);
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
                        
                        ESP_LOGI("FIRE_SYSTEM", "Pump %d restored to MANUAL mode (%lu seconds remaining)",
                                 i+1, remaining/1000);
                        on_pump_activated(i, true);
                    } else {
                        pumps[i].state = PUMP_OFF;
                        ESP_LOGI("FIRE_SYSTEM", "Pump %d manual time expired", i+1);
                    }
                } else {
                    pumps[i].state = PUMP_OFF;
                }
                break;
                
            case PUMP_COOLDOWN:
                pumps[i].state = PUMP_OFF;
                pumps[i].cooldownStartTime = 0;
                ESP_LOGI("FIRE_SYSTEM", "Pump %d cooldown reset", i+1);
                break;
                
            case PUMP_OFF:
            case PUMP_DISABLED:
                ESP_LOGI("FIRE_SYSTEM", "Pump %d remains OFF", i+1);
                break;
                
            default:
                pumps[i].state = PUMP_OFF;
                ESP_LOGI("FIRE_SYSTEM", "Pump %d set to OFF (unknown previous state)", i+1);
                break;
        }
    }
    
    ESP_LOGI("FIRE_SYSTEM", "===============================================");
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
    ESP_LOGI("FIRE_SYSTEM", "Processing emergency stop command: %s", 
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
        ESP_LOGE("FIRE_SYSTEM", "ADC unit init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE("FIRE_SYSTEM", "ADC channel 0 (GPIO36) config failed: %s", esp_err_to_name(ret));
    }
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_6, &channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE("FIRE_SYSTEM", "ADC channel 6 config failed: %s", esp_err_to_name(ret));
    }
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_7, &channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE("FIRE_SYSTEM", "ADC channel 7 config failed: %s", esp_err_to_name(ret));
    }
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE("FIRE_SYSTEM", "ADC channel 3 config failed: %s", esp_err_to_name(ret));
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
        ESP_LOGE("FIRE_SYSTEM", "ADC calibration init failed: %s", esp_err_to_name(ret));
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
    double sumVoltage = 0.0;
    double sumSquares = 0.0;
    uint32_t sampleCount = 0;

    while ((xTaskGetTickCount() - startTime) * portTICK_PERIOD_MS < SAMPLE_WINDOW) {
        int adcVal = 0;

        esp_err_t ret = adc_oneshot_read(adc1_handle, adc_channel, &adcVal);
        if (ret != ESP_OK) {
            continue;
        }

        float v = ((float)adcVal * VREF) / ADC_RES;

        sumVoltage += (double)v;
        sumSquares += (double)v * (double)v;
        sampleCount++;
    }

    if (sampleCount == 0) {
        return 0.0;
    }

    double meanV      = sumVoltage / sampleCount;
    double meanSquare = sumSquares / sampleCount;
    double variance   = meanSquare - (meanV * meanV);

    if (variance < 0.0) variance = 0.0;

    float Vrms    = sqrtf((float)variance);
    
    // APPLY CALIBRATION FACTOR FOR VOLTAGE DIVIDER
    float actual_Vrms = Vrms / CURRENT_CALIBRATION_FACTOR;
    
    float I_mapped = actual_Vrms / R_SHUNT;
    float I_load  = I_mapped / SCALE_RATIO;

    return I_load;
}

float read_current_sensor(int index) {
    CurrentSensor* sensor = &currentSensors[index];
    
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
        ESP_LOGE("FIRE_SYSTEM", "ERROR: ADC not initialized for sensor %s", sensor->name);
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
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    static unsigned long lastFaultCheck = 0;
    
    if (now - lastFaultCheck < 2000) {
        return;
    }
    lastFaultCheck = now;
    
    for (int i = 0; i < 4; i++) {
        CurrentSensor* sensor = &currentSensors[i];
        bool previousFault = sensor->fault;
        
        // Pump rated current is 12.5A; allow headroom for startup/inrush current
        sensor->fault = (sensor->currentValue < -0.1 || sensor->currentValue > 25.0);
        
        if ((now - sensor->lastReadTime) > 5000) {
            sensor->fault = true;
        }
        
        if (adc1_handle == NULL) {
            sensor->fault = true;
        }
        
        if (sensor->fault && !previousFault) {
            ESP_LOGW("FIRE_SYSTEM", "Current Sensor %s FAULT DETECTED: %.2f A", 
                     sensor->name, sensor->currentValue);
            // 🆕 SEND FAULT ALERT
            // extern void send_alert_current_sensor_fault(int, float);
            send_alert_current_sensor_fault(i, sensor->currentValue);
        } else if (!sensor->fault && previousFault) {
            ESP_LOGI("FIRE_SYSTEM", "Current Sensor %s FAULT CLEARED", sensor->name);
        }
    }
}

// ========================================
// COMPLETE SENSOR DATA ACQUISITION
// ========================================

void get_sensor_data(void) {
    if (adc1_handle == NULL) {
        ESP_LOGE("FIRE_SYSTEM", "ERROR: ADC not initialized");
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

    // Inverter voltages: adc_array2 channels 2-5, scaled x360
    // Matches original bushfire_mqtt_v2_final.ino:  voltN = adc_array2[n] * 360
    // adc_array2 holds calibrated voltage in volts (0-3.3V), so x360 gives
    // the inverter AC output voltage in the 0-~396V range.
	
	volt1 = adc_array2[2] * INVERTER_DIVIDER_RATIO;
	volt2 = adc_array2[3] * INVERTER_DIVIDER_RATIO;
	volt3 = adc_array2[4] * INVERTER_DIVIDER_RATIO;
	volt4 = adc_array2[5] * INVERTER_DIVIDER_RATIO;

    check_current_sensor_faults();

    pumps[0].currentIRValue = ir_s1;
    pumps[1].currentIRValue = ir_s2;
    pumps[2].currentIRValue = ir_s3;
    pumps[3].currentIRValue = ir_s4;

    check_water_lockout();
    detect_continuous_feed();  // Added continuous feed detection

    // Decode 0–20 mA current states for all fire sensors and update
    // sensorFault, BIT-fault debounce, and AuxBIT suppression flags.
    update_flame_signal_states();
}

// ========================================
// CORRECTED WATER LOCKOUT MANAGEMENT
// ========================================

void check_water_lockout(void) {
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
            ESP_LOGI("FIRE_SYSTEM", "Low water (%.2f%%) - Starting 20s grace period for continuous feed", level_s);
            return;
        }

        // ========================================
        // SECTION 4.3: GRACE PERIOD CHECK
        // ========================================
        if (inGracePeriod) {
            // Check if water is recovering during grace period
            if (level_s > gracePeriodWaterLevel + 5.0) {
                ESP_LOGI("FIRE_SYSTEM", "Water recovering during grace period: %.2f%% -> %.2f%%",
                         gracePeriodWaterLevel, level_s);
                gracePeriodWaterLevel = level_s;
                gracePeriodStartTime = now;
            }
            
            // Check if grace period expired without recovery
            if (now - gracePeriodStartTime >= GRACE_PERIOD_TIME) {
                if (!waterLockout) {
                    // ✅ CAPTURE STOP REASON **FIRST** - before any state changes
                    if (startAllPumpsActive) {
                        startAllPumps_stop_reason = STOP_REASON_START_ALL_WATER_LOCKOUT;
                        ESP_LOGI("FIRE_SYSTEM", "✅ Captured stop reason: WATER_LOCKOUT (grace period expired)");
                    }
                    
                    waterLockout = true;
                    inGracePeriod = false;
                    ESP_LOGI("FIRE_SYSTEM", "GRACE PERIOD EXPIRED - LOCKOUT ACTIVATED");
                    
                    // ✅ FORCE STOP ALL PUMPS (including timer-protected)
                    ESP_LOGI("FIRE_SYSTEM", "Stopping ALL pumps due to water lockout");
                    for (int i = 0; i < 4; i++) {
                        if (pumps[i].state == PUMP_AUTO_ACTIVE || 
                            pumps[i].state == PUMP_MANUAL_ACTIVE) {
                            
                            if (pumps[i].timerProtected && !is_timer_expired(i)) {
                                unsigned long remaining = get_timer_remaining(i);
                                ESP_LOGI("FIRE_SYSTEM", "Overriding timer on %s (%lu sec remaining)",
                                         pumps[i].name, remaining);
                            }
                            
                            deactivate_pump(i, "water_lockout_grace_expired");
                        }
                    }
                    
                    // ✅ Clear startAllPumpsActive flag AFTER capturing reason
                    if (startAllPumpsActive) {
                        ESP_LOGI("FIRE_SYSTEM", "Water lockout - clearing startAllPumpsActive flag");
                        startAllPumpsActive = false;
                    }
                    on_water_lockout_activated();
                }
            }
        }

        // ========================================
        // SECTION 4.1: NON-CONTINUOUS FEED LOCKOUT
        // ========================================
        if (!continuousWaterFeed && !waterLockout) {
            // ✅ CAPTURE STOP REASON **FIRST** - before any state changes
            if (startAllPumpsActive) {
                startAllPumps_stop_reason = STOP_REASON_START_ALL_WATER_LOCKOUT;
                ESP_LOGI("FIRE_SYSTEM", "✅ Captured stop reason: WATER_LOCKOUT");
            }
            
            waterLockout = true;
            ESP_LOGI("FIRE_SYSTEM", "WATER LOCKOUT ACTIVATED - Level: %.2f%%", level_s);
            
            // ✅ FORCE STOP ALL PUMPS
            ESP_LOGI("FIRE_SYSTEM", "Stopping ALL pumps due to low water");
            for (int i = 0; i < 4; i++) {
                if (pumps[i].state == PUMP_AUTO_ACTIVE || 
                    pumps[i].state == PUMP_MANUAL_ACTIVE) {
                    
                    if (pumps[i].timerProtected && !is_timer_expired(i)) {
                        unsigned long remaining = get_timer_remaining(i);
                        ESP_LOGI("FIRE_SYSTEM", "Overriding timer on %s (%lu sec remaining)",
                                 pumps[i].name, remaining);
                    }
                    
                    deactivate_pump(i, "water_lockout");
                }
            }
            
            // ✅ Clear startAllPumpsActive flag AFTER capturing reason
            if (startAllPumpsActive) {
                ESP_LOGI("FIRE_SYSTEM", "Water lockout - clearing startAllPumpsActive flag");
                startAllPumpsActive = false;
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
                    ESP_LOGI("FIRE_SYSTEM", "Water stable at %.2f%%, starting 5s stability check", level_s);
                } else if (now - stableStartTime >= 5000) {
                    waterLockout = false;
                    stableStartTime = 0;
                    ESP_LOGI("FIRE_SYSTEM", "Water stable for 5s, LOCKOUT RELEASED - Level: %.2f%%", level_s);
                    on_water_lockout_released();
                }
            } else {
                stableStartTime = 0;
                lastStableWaterLevel = level_s;
                ESP_LOGI("FIRE_SYSTEM", "Water unstable: %.2f%% -> %.2f%%, resetting stability timer",
                         lastStableWaterLevel, level_s);
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

// ========================================
// 0–20mA CURRENT-OUTPUT SIGNAL DECODING
// ========================================

/**
 * Decode a raw ir_sX percentage value (0–100 %) into the detector's
 * published 0–20 mA current states (Table 8 of the 40/40I datasheet).
 *
 * The 150 Ω shunt on each Fire Sx line converts:
 *   0 mA  →  0.00 V  →  0.0 %   FAULT
 *   2 mA  →  0.30 V  →  9.1 %   BIT FAULT
 *   4 mA  →  0.60 V  → 18.2 %   NORMAL
 * 4–16 mA →  rising  → WARNING  (pre-alarm)
 *  16 mA  →  2.40 V  → 72.7 %   AUX BIT (manual BIT in progress)
 *  20 mA  →  3.00 V  → 90.9 %   FIRE ALARM
 */
FlameSignalState decode_flame_signal(float irPct) {
    if (irPct <= IR_PCT_FAULT_HIGH)          return FLAME_SIGNAL_FAULT;
    if (irPct <= IR_PCT_BIT_FAULT_HIGH)      return FLAME_SIGNAL_BIT_FAULT;
    if (irPct <= IR_PCT_NORMAL_HIGH)         return FLAME_SIGNAL_NORMAL;
    if (irPct <= IR_PCT_WARNING_HIGH)        return FLAME_SIGNAL_WARNING;
    if (irPct <= IR_PCT_AUX_BIT_HIGH)        return FLAME_SIGNAL_AUX_BIT;
    return FLAME_SIGNAL_FIRE;                // >= IR_PCT_FIRE_THRESHOLD
}

const char* get_flame_signal_state_string(FlameSignalState state) {
    switch (state) {
        case FLAME_SIGNAL_FAULT:     return "FAULT(0mA)";
        case FLAME_SIGNAL_BIT_FAULT: return "BIT_FAULT(2mA)";
        case FLAME_SIGNAL_NORMAL:    return "NORMAL(4mA)";
        case FLAME_SIGNAL_WARNING:   return "WARNING(pre-alarm)";
        case FLAME_SIGNAL_AUX_BIT:   return "AUX_BIT(16mA)";
        case FLAME_SIGNAL_FIRE:      return "FIRE_ALARM(20mA)";
        default:                     return "UNKNOWN";
    }
}

/**
 * Call once per sensor-data cycle (after get_sensor_data) to:
 *  1. Decode each sensor's current ir_sX value into a FlameSignalState.
 *  2. Manage BIT-fault debounce counter (3 consecutive readings required
 *     before marking sensorFault = true, per section 2.6.2).
 *  3. Track AuxBIT window so the fire-detection logic can ignore
 *     transient dips that occur during a 1-minute automatic BIT cycle.
 *  4. Auto-clear sensorFault when the signal returns to NORMAL or FIRE.
 */
void update_flame_signal_states(void) {
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    float irValues[4] = {ir_s1, ir_s2, ir_s3, ir_s4};

    for (int i = 0; i < 4; i++) {
        FlameSignalState newState = decode_flame_signal(irValues[i]);
        pumps[i].flameSignalState = newState;

        switch (newState) {

        case FLAME_SIGNAL_FAULT:
            // Open-circuit / power loss – detector cannot detect fire.
            // Debounce: require 3 consecutive fault readings.
            pumps[i].bitFaultCount++;
            if (pumps[i].bitFaultCount >= 3 && !pumps[i].sensorFault) {
                pumps[i].sensorFault = true;
                ESP_LOGE("FIRE_SYSTEM",
                         "%s: OPEN-CIRCUIT FAULT detected (0 mA) – sensor offline",
                         pumps[i].name);
                // Use the dedicated IR sensor alert (CRITICAL – cannot detect fire)
                send_alert_ir_sensor_fault(i, irValues[i]);
            }
            break;

        case FLAME_SIGNAL_BIT_FAULT:
            // Detector self-test failed (2 mA) – may still detect fire
            // but reliability is reduced (section 2.6.2 of datasheet).
            pumps[i].bitFaultCount++;
            if (pumps[i].bitFaultCount >= 3 && !pumps[i].sensorFault) {
                pumps[i].sensorFault = true;
                ESP_LOGW("FIRE_SYSTEM",
                         "%s: BIT FAULT detected (2 mA) – self-test failed (count=%d)",
                         pumps[i].name, pumps[i].bitFaultCount);
                // Use the dedicated IR sensor alert (WARNING severity – sensor degraded)
                send_alert_ir_sensor_fault(i, irValues[i]);
            }
            // LED will flash yellow at 4 Hz on the detector itself.
            break;

        case FLAME_SIGNAL_NORMAL:
            // 4 mA – healthy, no fire. Reset fault state.
            pumps[i].bitFaultCount = 0;
            pumps[i].inAuxBit = false;
            if (pumps[i].sensorFault) {
                pumps[i].sensorFault = false;
                ESP_LOGI("FIRE_SYSTEM",
                         "%s: Signal restored to NORMAL (4 mA) – fault cleared",
                         pumps[i].name);
            }
            break;

        case FLAME_SIGNAL_WARNING:
            // Pre-alarm: fire detected, changing to Warning state (Table 8).
            // Not yet a full 20 mA alarm – log for monitoring but do NOT
            // activate pumps here; confirmation logic in check_automatic_activation
            // will handle activation once the signal reaches FIRE level.
            pumps[i].bitFaultCount = 0;
            pumps[i].inAuxBit = false;
            if (pumps[i].sensorFault) {
                pumps[i].sensorFault = false;
            }
            static unsigned long lastWarnLog[4] = {0,0,0,0};
            if (now - lastWarnLog[i] > 2000) {
                ESP_LOGW("FIRE_SYSTEM",
                         "%s: WARNING / pre-alarm (%.1f %%) – fire detected, rising",
                         pumps[i].name, irValues[i]);
                lastWarnLog[i] = now;
            }
            break;

        case FLAME_SIGNAL_AUX_BIT:
            // 16 mA – manual or automatic BIT in progress (Tables 10–13).
            // Automatic BIT runs every 1 minute and lasts a few seconds.
            // Suppress false-alarm during this window.
            pumps[i].bitFaultCount = 0;
            if (!pumps[i].inAuxBit) {
                pumps[i].inAuxBit = true;
                pumps[i].auxBitStartTime = now;
                ESP_LOGI("FIRE_SYSTEM",
                         "%s: BIT in progress (16 mA) – suppressing fire detection",
                         pumps[i].name);
            }
            break;

        case FLAME_SIGNAL_FIRE:
            // 20 mA – Fire Alarm state confirmed by detector.
            pumps[i].bitFaultCount = 0;
            pumps[i].inAuxBit = false;
            if (pumps[i].sensorFault) {
                pumps[i].sensorFault = false;
            }
            // Actual pump activation is handled in check_automatic_activation().
            break;

        default:
            break;
        }

        // Safety: if AuxBIT has been running for more than 10 seconds,
        // clear the flag (BIT should complete well within that window).
        if (pumps[i].inAuxBit && (now - pumps[i].auxBitStartTime > 10000)) {
            pumps[i].inAuxBit = false;
            ESP_LOGI("FIRE_SYSTEM", "%s: AuxBIT window expired – resuming normal detection",
                     pumps[i].name);
        }
    }
}

bool is_sensor_in_bit_fault(int index) {
    return (pumps[index].flameSignalState == FLAME_SIGNAL_BIT_FAULT ||
            pumps[index].flameSignalState == FLAME_SIGNAL_FAULT);
}

bool is_sensor_in_aux_bit(int index) {
    return pumps[index].inAuxBit;
}

void check_automatic_activation(void) {
    if (waterLockout || !systemArmed) return;

    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // First, check each sensor for flame confirmation
    for (int i = 0; i < 4; i++) {
        if (pumps[i].sensorFault) {
            ESP_LOGI("FIRE_SYSTEM", "%s: Sensor fault, ignoring activation", pumps[i].name);
            continue;
        }
        if (pumps[i].state == PUMP_MANUAL_ACTIVE || pumps[i].state == PUMP_COOLDOWN) {
            continue;
        }

        // Skip fire detection if a BIT (automatic or manual) is in progress –
        // the detector briefly changes its output and must not trigger a false alarm.
        if (pumps[i].inAuxBit) {
            continue;
        }

        // Use the decoded 0–20 mA state rather than a raw percentage threshold.
        // Only FLAME_SIGNAL_FIRE (≥ ~18 mA / 20 mA alarm output) triggers activation.
        // FLAME_SIGNAL_WARNING is logged by update_flame_signal_states() but does
        // not yet activate pumps – it is a pre-alarm indicator only.
        bool flameDetected = (pumps[i].flameSignalState == FLAME_SIGNAL_FIRE);

        // ========================================
        // SECTION 2(A): 2-SECOND FLAME CONFIRMATION
        // ========================================
        if (flameDetected) {
            pumps[i].lastFlameSeenTime = now;
            
            if (!flameValidating[i]) {
                // First detection - start 2-second timer
                flameStartTime[i] = now;
                flameValidating[i] = true;
                ESP_LOGI("FIRE_SYSTEM", "%s: Flame detected (%.2f%%) - Starting 2s confirmation",
                         pumps[i].name, pumps[i].currentIRValue);
            } 
            else if (now - flameStartTime[i] >= 2000) {
                if (!pumps[i].flameConfirmed) {
                    pumps[i].flameConfirmed = true;
                    pumps[i].flameFirstDetectedTime = now;
                    pumps[i].flameConfirmationDurationMs = (int)(now - flameStartTime[i]);

                    // Store wall clock time of confirmation as readable string
                    extern const char* get_custom_timestamp(void);
                    strncpy(pumps[i].flameDetectionTime, get_custom_timestamp(),
                            sizeof(pumps[i].flameDetectionTime) - 1);

                    ESP_LOGI("FIRE_SYSTEM", "%s: FLAME CONFIRMED (persisted 2+ seconds)", pumps[i].name);
                    on_flame_confirmed(i);
                }
            }
        } 
        else {
            // Flame lost
            if (flameValidating[i]) {
                ESP_LOGI("FIRE_SYSTEM", "%s: Flame lost before confirmation (< 2s)", pumps[i].name);
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
    
    // Check the active profile's suppression mode ("sector" or "full")
    bool isFullMode = (strcmp(profile->mode, "full") == 0);

    if (isFullMode && confirmedCount > 0) {
        // ========================================
        // FULL MODE: ANY confirmed sector activates ALL 4 pumps
        // ========================================
        // Activate ALL pumps using activate_pump() with activateAll = true
        // Find first pump with confirmed flame to trigger the activation
        for (int i = 0; i < 4; i++) {
            if (pumps[i].flameConfirmed && pumps[i].state != PUMP_AUTO_ACTIVE) {
                activate_pump(i, true);  // true = activate ALL pumps
                break;  // Only need to call once
            }
        }
    } else if (!isFullMode && confirmedCount > 0) {
        // ========================================
        // SECTOR MODE: Activate only pumps with confirmed flame
        // ========================================

        // When ALL 4 sectors confirm fire, the full-system run cap applies
        // even in sector mode - each sector activates individually, but if
        // all 4 happen to ignite simultaneously the longer/shorter Full cap
        // is used instead of the per-sector cap.
        bool allSectorsOnFire = (confirmedCount == 4);

        for (int i = 0; i < 4; i++) {
            if (pumps[i].flameConfirmed && pumps[i].state != PUMP_AUTO_ACTIVE) {
                activate_pump(i, false);  // activate this pump individually

                // Override cap: if all 4 sectors confirmed, use maxRunCapFull
                if (allSectorsOnFire) {
                    pumps[i].activatedInFullSystemMode = true;
                    pumps[i].activationCapMs = profiles[currentProfile].maxRunCapFull;
                    ESP_LOGI("FIRE_SYSTEM",
                             "%s: All 4 sectors on fire - applying Full cap (%lu sec)",
                             pumps[i].name,
                             profiles[currentProfile].maxRunCapFull / 1000);
                }
            }
        }
    }
}

// ========================================
// CORRECTED PUMP STATE MANAGEMENT
// ========================================

void update_pump_states(void) {
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (emergencyStopActive) {
        static unsigned long lastEmergencyCheck = 0;
        if (now - lastEmergencyCheck > 5000) {
            ESP_LOGI("FIRE_SYSTEM", "Emergency stop active - pump state updates suspended");
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
                pumps[i].cooldownDuration = profiles[currentProfile].cooldown;
            }
            
            unsigned long cooldownElapsed = now - pumps[i].cooldownStartTime;
            if (cooldownElapsed >= pumps[i].cooldownDuration) {
                // ✅ COOLDOWN COMPLETE - SYSTEM RE-ARMS
                pumps[i].state = PUMP_OFF;
                pumps[i].cooldownStartTime = 0;
                pumps[i].cooldownDuration = 0;
                
                ESP_LOGI("FIRE_SYSTEM", "%s: Cooldown complete - SYSTEM RE-ARMED", pumps[i].name);
                ESP_LOGI("FIRE_SYSTEM", "%s: Ready for new activation if flame detected", pumps[i].name);
                
                // ✅ System will automatically check for flame and reactivate in check_automatic_activation()
            }
            continue;
        }

        // TIMER-PROTECTED PUMPS
        if (pumps[i].timerProtected) {
            if (is_timer_expired(i)) {
                ESP_LOGI("FIRE_SYSTEM", "%s: Timer expired - Stopping pump", pumps[i].name);
                deactivate_pump(i, "timer_expired");
                continue;
            }
            
            unsigned long remaining = get_timer_remaining(i);
            
            if (pumps[i].state == PUMP_MANUAL_ACTIVE && 
                pumps[i].currentIRValue > 80.0) {
                ESP_LOGI("FIRE_SYSTEM", "%s: Fire detected, transitioning to AUTO mode (timer continues: %lu sec)",
                         pumps[i].name, remaining);
                pumps[i].state = PUMP_AUTO_ACTIVE;
                pumps[i].activationSource = ACTIVATION_SOURCE_AUTO;
            }
            
            static unsigned long lastTimerLog[4] = {0};
            if (now - lastTimerLog[i] > 10000) {
                ESP_LOGI("FIRE_SYSTEM", "%s: PROTECTED - %lu seconds remaining (State: %s)",
                         pumps[i].name, remaining, get_pump_state_string(i));
                lastTimerLog[i] = now;
            }
            
            continue;
        }

        // MANUAL MODE (WITHOUT TIMER)
        if (pumps[i].state == PUMP_MANUAL_ACTIVE && !pumps[i].timerProtected) {
          
            unsigned long manualElapsed = now - pumps[i].manualStartTime;
            if (manualElapsed >= pumps[i].manualDuration) {
                ESP_LOGI("FIRE_SYSTEM", "%s: Manual timer expired (legacy)", pumps[i].name);
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
            ESP_LOGI("FIRE_SYSTEM", "%s: No flame for %lus - Stopping", 
                     pumps[i].name, noFlameTimeout/1000);
            deactivate_pump(i, "no_flame_timeout");
            continue;
        }

        // ✅ CORRECTED: MCRC Check - Uses ONLY current activation runtime
        // Use activatedInFullSystemMode (snapshotted at pump start) not profile->autoModeFull.
        // This correctly applies maxRunCapFull when all 4 sectors are burning,
        // even on profiles where autoModeFull = false (e.g. WILDLAND_STANDARD).
        unsigned long maxRunCap = pumps[i].activatedInFullSystemMode ?
                                  profiles[currentProfile].maxRunCapFull :
                                  profiles[currentProfile].maxRunCapSector;

        if (continuousWaterFeed) {
            maxRunCap = 0; // No limit in continuous feed mode
        }

        // Calculate runtime for THIS activation only
        unsigned long runTime = now - pumps[i].pumpStartTime;
        
        if (maxRunCap > 0 && runTime >= maxRunCap) {
            const char* capType = pumps[i].activatedInFullSystemMode ? "Full" : "Sector";
            ESP_LOGI("FIRE_SYSTEM", "%s: Max run cap reached (%s: %lu/%lu sec) - Stopping", 
                     pumps[i].name, capType, runTime/1000, maxRunCap/1000);
            deactivate_pump(i, "max_run_cap_expired");
            
            pumps[i].state = PUMP_COOLDOWN;
            pumps[i].cooldownStartTime = now;
            pumps[i].cooldownDuration = profiles[currentProfile].cooldown;
            // Clear flame state so alert_task does not re-read stale confirmed data
            pumps[i].flameConfirmed = false;
            pumps[i].flameConfirmationDurationMs = 0;
            pumps[i].flameDetectionTime[0] = '\0';
        }
    }
}




void activate_pump(int index, bool activateAll) {
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (emergencyStopActive) {
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
                pumps[i].activationCapMs = profiles[currentProfile].maxRunCapFull;  // snapshot cap
                set_pump_hardware(i, true);
                ESP_LOGI("FIRE_SYSTEM", "Pump %s ACTIVATED (Full-System Mode)", pumps[i].name);
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
        pumps[index].activationCapMs = profiles[currentProfile].maxRunCapSector;    // snapshot cap
        set_pump_hardware(index, true);
        ESP_LOGI("FIRE_SYSTEM", "Pump %s ACTIVATED (Sector Mode)", pumps[index].name);
        on_pump_activated(index, false);
    }
}

// ==========================================
// UPDATED: deactivate_pump() - Allow critical conditions to override timer
// ==========================================

void deactivate_pump(int index, const char* reason) {
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) return;

    // ✅ CHECK TIMER PROTECTION - Only allow specific reasons to override
    if (pumps[index].timerProtected && !is_timer_expired(index)) {
        // Only these reasons can stop a timer-protected pump:
        bool allowedToStop = false;
        
        if (strstr(reason, "water_lockout") != NULL) {
            allowedToStop = true;
            ESP_LOGI("FIRE_SYSTEM", "%s: Timer-protected but WATER LOCKOUT - forcing stop", 
                     pumps[index].name);
        }
        else if (strstr(reason, "emergency_stop") != NULL || 
                 strstr(reason, "shadow_command") != NULL) {
            allowedToStop = true;
            ESP_LOGI("FIRE_SYSTEM", "%s: Timer-protected but EMERGENCY STOP - forcing stop", 
                     pumps[index].name);
        }
        else if (strstr(reason, "timer_expired") != NULL) {
            allowedToStop = true;
            ESP_LOGI("FIRE_SYSTEM", "%s: Timer expired naturally", pumps[index].name);
        }
        else if (strstr(reason, "user_stop_requested") != NULL) {
            allowedToStop = true;
            ESP_LOGI("FIRE_SYSTEM", "%s: User stop request - overriding timer", 
                     pumps[index].name);
        }
        
        if (!allowedToStop) {
            unsigned long remaining = get_timer_remaining(index);
            ESP_LOGI("FIRE_SYSTEM", "%s: BLOCKED deactivation (reason: %s) - Timer protected (%lu sec remaining)",
                     pumps[index].name, reason, remaining);
            return; // ✅ BLOCK the stop
        }
    }

    unsigned long runTime = (xTaskGetTickCount() * portTICK_PERIOD_MS) - pumps[index].pumpStartTime;
    ActivationSource stoppedSource = pumps[index].activationSource;

    if (pumps[index].state != PUMP_COOLDOWN) {
        pumps[index].state = PUMP_OFF;
    }

    // ── Map reason string to lastStopReason enum ──
    // This ensures task_pump_management always reads a fresh, correct value
    // regardless of which code path called deactivate_pump.
    if (strstr(reason, "no_flame_timeout") != NULL) {
        pumps[index].lastStopReason = STOP_REASON_AUTO_TIMEOUT;
    } else if (strstr(reason, "max_run_cap_expired") != NULL) {
        pumps[index].lastStopReason = STOP_REASON_RUN_CAP;
    } else if (strstr(reason, "water_lockout") != NULL) {
        pumps[index].lastStopReason = STOP_REASON_WATER_LOCKOUT;
    } else if (strstr(reason, "emergency_stop") != NULL ||
               strstr(reason, "shadow_command") != NULL) {
        pumps[index].lastStopReason = STOP_REASON_EMERGENCY_STOP;
    } else if (strstr(reason, "sensor_fault") != NULL) {
        pumps[index].lastStopReason = STOP_REASON_SENSOR_FAULT;
    } else if (strstr(reason, "profile_change") != NULL) {
        pumps[index].lastStopReason = STOP_REASON_PROFILE_CHANGE;
    } else if (strstr(reason, "timer_expired") != NULL ||
               strstr(reason, "manual_timer_expired") != NULL) {
        pumps[index].lastStopReason = STOP_REASON_RUN_CAP;
    } else {
        // manual_stop, user_stop_requested, manual_restart, manual_all_override
        pumps[index].lastStopReason = STOP_REASON_MANUAL;
    }

    pumps[index].flameFirstDetectedTime = 0;
    pumps[index].flameConfirmed = false;
    pumps[index].manualMode = false;
    flameValidating[index] = false;
    
    pumps[index].activationSource = ACTIVATION_SOURCE_NONE;
    
    // ✅ STOP TIMER PROTECTION
    stop_timer_protection(index);

    set_pump_hardware(index, false);

    ESP_LOGI("FIRE_SYSTEM", "Pump %s STOPPED - Reason: %s (Ran %lu seconds) | Source: %s",
             pumps[index].name, reason, runTime/1000, 
             get_activation_source_string(stoppedSource));
    
    on_pump_deactivated(index, reason);
}

void stop_all_pumps(const char* reason) {
    ESP_LOGI("FIRE_SYSTEM", "STOPPING ALL PUMPS - Reason: %s", reason);

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


void process_stop_pump_request(int index) {
    if (index < 0 || index >= 4) {
        ESP_LOGE("FIRE_SYSTEM", "ERROR: Invalid pump index %d", index);
        return;
    }
    
    if (!pumps[index].stopPumpRequested) {
        return; // No stop request
    }
    
    ESP_LOGI("FIRE_SYSTEM", "Processing stop request for %s", pumps[index].name);
    
    // Check if pump is running
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) {
        ESP_LOGI("FIRE_SYSTEM", "%s already stopped, clearing flag", pumps[index].name);
        pumps[index].stopPumpRequested = false;
        return;
    }
    
    // Check if timer protected
    if (pumps[index].timerProtected && !is_timer_expired(index)) {
        unsigned long remaining = get_timer_remaining(index);
        ESP_LOGI("FIRE_SYSTEM", "%s: Overriding timer protection (%lu sec remaining) - USER REQUESTED STOP",
                 pumps[index].name, remaining);
        
        // Stop timer protection
        stop_timer_protection(index);
    }
    
    // Stop the pump
    deactivate_pump(index, "user_stop_requested");
    
    // Clear the stop request flag
    pumps[index].stopPumpRequested = false;
    
    ESP_LOGI("FIRE_SYSTEM", "%s stopped successfully via stopPump parameter", pumps[index].name);
}


bool shadow_manual_stop_pump_override_timer(int index) {
    if (index < 0 || index >= 4) {
        ESP_LOGE("FIRE_SYSTEM", "ERROR: Invalid pump index %d", index);
        return false;
    }
    
    // Set the stop request flag
    pumps[index].stopPumpRequested = true;
    
    ESP_LOGI("FIRE_SYSTEM", "Stop request set for %s", pumps[index].name);
    
    // Process immediately
    process_stop_pump_request(index);
    
    return true;
}

// ========================================
// PRINT CURRENT SENSOR STATUS
// ========================================

void print_current_sensor_status(void) {
    ESP_LOGI("FIRE_SYSTEM", "Current SENSORS] Detailed Status:");
    ESP_LOGI("FIRE_SYSTEM", "----------------------------------");
    
    for (int i = 0; i < 4; i++) {
        ESP_LOGI("FIRE_SYSTEM", "%s (%s):", currentSensors[i].name, pumps[i].name);
        ESP_LOGI("FIRE_SYSTEM", "  Current: %.2f A | Average: %.2f A", 
                 currentSensors[i].currentValue, currentSensors[i].averageValue);
        ESP_LOGI("FIRE_SYSTEM", "  Fault: %s | Mux: %s", 
                 currentSensors[i].fault ? "YES" : "NO",
                 currentSensors[i].isMux ? "YES" : "NO");
        
        if (currentSensors[i].isMux) {
            ESP_LOGI("FIRE_SYSTEM", " | Channel: %d", currentSensors[i].muxChannel);
        }
        
        unsigned long time_since_read = xTaskGetTickCount() * portTICK_PERIOD_MS - currentSensors[i].lastReadTime;
        ESP_LOGI("FIRE_SYSTEM", "  Last read: %lu ms ago", time_since_read);
    }
    ESP_LOGI("FIRE_SYSTEM", "----------------------------------");
}

// ========================================
// CORRECTED MANUAL CONTROL FUNCTIONS
// ========================================

void manual_activate_pump(int index) {
    if (emergencyStopActive) {
        ESP_LOGI("FIRE_SYSTEM", "Manual activation BLOCKED for %s - Emergency stop active", pumps[index].name);
        return;
    }
    
    if (waterLockout) {
        ESP_LOGI("FIRE_SYSTEM", "Manual activation BLOCKED for %s - Water lockout active", pumps[index].name);
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
    pumps[index].activationCapMs = MANUAL_SINGLE_PUMP_TIME;  // snapshot cap
    
    // 🆕 START TIMER PROTECTION
    start_timer_protection(index, MANUAL_SINGLE_PUMP_TIME);
   
    set_pump_hardware(index, true);

    ESP_LOGI("FIRE_SYSTEM", "MANUAL ACTIVATION: %s (2 minutes, SINGLE)", pumps[index].name);
    on_pump_activated(index, true);
}

// Activates all 4 pumps for a caller-supplied duration (ms).
// This is the common implementation used by both the default 90s
// manual-all-pumps path and the shadow-driven variable-duration path.
void manual_activate_all_pumps_with_duration(unsigned long durationMs) {
    if (emergencyStopActive) {
        ESP_LOGI("FIRE_SYSTEM", "Manual activation BLOCKED - Emergency stop active");
        return;
    }
    
    if (waterLockout) {
        ESP_LOGI("FIRE_SYSTEM", "Manual activation BLOCKED - Water lockout active");
        return;
    }

    if (durationMs == 0) {
        ESP_LOGI("FIRE_SYSTEM", "Manual activation BLOCKED - Invalid duration (0ms)");
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
        pumps[i].manualDuration = durationMs;
        pumps[i].pumpStartTime = now;
        
        // SET MANUAL ALL ACTIVATION SOURCE
        pumps[i].activationSource = ACTIVATION_SOURCE_MANUAL_ALL;
        pumps[i].activationCapMs = durationMs;  // snapshot cap
        
         // 🆕 START TIMER PROTECTION FOR EACH PUMP
        start_timer_protection(i, durationMs);

        set_pump_hardware(i, true);
        activatedCount++;
        on_pump_activated(i, true);
    }

    ESP_LOGI("FIRE_SYSTEM", "MANUAL ACTIVATION: ALL PUMPS (%d active, %lu seconds)",
             activatedCount, durationMs / 1000UL);
}

// Backwards-compatible default: activates all pumps for the fixed
// MANUAL_ALL_PUMPS_TIME (90s). Kept for local/manual button commands.
void manual_activate_all_pumps(void) {
    manual_activate_all_pumps_with_duration(MANUAL_ALL_PUMPS_TIME);
}

void extend_manual_runtime(int index, unsigned long extensionTime) {
    if (pumps[index].state != PUMP_MANUAL_ACTIVE) {
        ESP_LOGI("FIRE_SYSTEM", "Cannot extend %s - Not in manual mode", pumps[index].name);
        return;
    }

    // Extend manual duration field
    pumps[index].manualDuration += extensionTime;

    // 🆕 EXTEND TIMER PROTECTION
    extend_timer_protection(index, extensionTime);
    
    unsigned long remaining = get_timer_remaining(index);
    
    ESP_LOGI("FIRE_SYSTEM", "Extended %s by %lus (Total remaining: %lus)",
             pumps[index].name, extensionTime/1000, remaining);
}

void manual_stop_pump(int index) {
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) {
        ESP_LOGI("FIRE_SYSTEM", "Pump %s already stopped", pumps[index].name);
        return;
    }

    ESP_LOGI("FIRE_SYSTEM", "MANUAL STOP: %s", pumps[index].name);
    deactivate_pump(index, "manual_stop");
}

// ========================================
// SHADOW-INTEGRATED MANUAL CONTROL FUNCTIONS
// ========================================

bool can_activate_pump_manually(int index) {
    if (index < 0 || index >= 4) {
        ESP_LOGE("FIRE_SYSTEM", "ERROR: Invalid pump index %d", index);
        return false;
    }
    
    if (emergencyStopActive) {
        ESP_LOGI("FIRE_SYSTEM", "Manual activation blocked for %s - Emergency stop active", pumps[index].name);
        return false;
    }
    
    if (waterLockout) {
        ESP_LOGI("FIRE_SYSTEM", "Manual activation blocked for %s - Water lockout active", pumps[index].name);
        return false;
    }
    
    return true;
}

bool shadow_manual_activate_pump_with_duration(int index, unsigned long durationMs) {
    if (index < 0 || index >= 4) {
        ESP_LOGE("FIRE_SYSTEM", "ERROR: Invalid pump index %d", index);
        return false;
    }
    
    // Check basic activation conditions
    if (!can_activate_pump_manually(index)) {
        return false;
    }
    
    // Check for water lockout
    if (waterLockout) {
        ESP_LOGI("FIRE_SYSTEM", "Blocked: %s - Water lockout active (Level: %.2f%%)", 
                 pumps[index].name, level_s);
        return false;
    }
    
    // Check for emergency stop
    if (emergencyStopActive) {
        ESP_LOGI("FIRE_SYSTEM", "Blocked: %s - Emergency stop active", pumps[index].name);
        return false;
    }
    
    // Stop pump if already running (immediate restart allowed)
    if (pumps[index].state == PUMP_MANUAL_ACTIVE || 
        pumps[index].state == PUMP_AUTO_ACTIVE) {
        ESP_LOGI("FIRE_SYSTEM", "Restarting %s with new duration", pumps[index].name);
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
    
    ESP_LOGI("FIRE_SYSTEM", "Pump %s activated with %lu second timer (PROTECTED)", 
             pumps[index].name, (unsigned long)(durationMs/1000));
    
    // Trigger callback
    on_pump_activated(index, true);
    
    return true;
}

// Activates all pumps via shadow with a caller-supplied duration (ms).
// stop conditions are unchanged: timer expiry (this duration), water
// lockout, or emergency stop / manual stop.
bool shadow_manual_activate_all_pumps_with_duration(unsigned long durationMs) {
    if (emergencyStopActive || waterLockout) {
        return false;
    }

    if (durationMs == 0) {
        ESP_LOGI("FIRE_SYSTEM", "shadow_manual_activate_all_pumps_with_duration BLOCKED - Invalid duration (0ms)");
        return false;
    }

    ESP_LOGI("FIRE_SYSTEM", "Activating ALL pumps with %lu-second timers", durationMs / 1000UL);

    manual_activate_all_pumps_with_duration(durationMs); // This now starts timers automatically

    // Override activation sources
    for (int i = 0; i < 4; i++) {
        pumps[i].activationSource = ACTIVATION_SOURCE_SHADOW_ALL;
    }

    ESP_LOGI("FIRE_SYSTEM", "All pumps activated (%lus timers, PROTECTED)", durationMs / 1000UL);
    return true;
}

bool shadow_manual_activate_all_pumps(void) {
    return shadow_manual_activate_all_pumps_with_duration(MANUAL_ALL_PUMPS_TIME);
}

bool shadow_manual_stop_pump(int index) {
    if (index < 0 || index >= 4) {
        ESP_LOGE("FIRE_SYSTEM", "ERROR: Invalid pump index %d", index);
        return false;
    }
    
    if (pumps[index].state == PUMP_OFF || pumps[index].state == PUMP_DISABLED) {
        ESP_LOGI("FIRE_SYSTEM", "Pump %s already stopped", pumps[index].name);
        return true;
    }
    
    manual_stop_pump(index);
    
    ESP_LOGI("FIRE_SYSTEM", "Pump %s stopped via shadow", pumps[index].name);
    
     if (pumps[index].timerProtected) {
        stop_timer_protection(index);
    }
    
    return true;
}

bool shadow_manual_stop_all_pumps(void) {
    ESP_LOGI("FIRE_SYSTEM", "Stopping all pumps via shadow");
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
    ESP_LOGI("FIRE_SYSTEM", "Pump %s activated (%s)", pumps[index].name, isManual ? "Manual" : "Auto");
}

void on_pump_deactivated(int index, const char* reason) {
    ESP_LOGI("FIRE_SYSTEM", "Pump %s deactivated - %s", pumps[index].name, reason);
}

void on_water_lockout_activated(void) {
    ESP_LOGI("FIRE_SYSTEM", "Water lockout activated - All manual buttons disabled");
}

void on_water_lockout_released(void) {
    ESP_LOGI("FIRE_SYSTEM", "Water lockout released - Manual and auto activation re-enabled");
}

void check_sensor_health(void) {
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (now - lastSensorHealthCheck < SENSOR_HEALTH_INTERVAL) {
        return;
    }
    
    lastSensorHealthCheck = now;
    ESP_LOGI("FIRE_SYSTEM", "=== SENSOR HEALTH CHECK ===");
    
    for (int i = 0; i < 4; i++) {
        bool healthy = is_sensor_healthy(i);
        
        if (!healthy && !pumps[i].sensorFault) {
            pumps[i].sensorFault = true;
            ESP_LOGW("FIRE_SYSTEM", "SENSOR FAULT: %s IR sensor", pumps[i].name);
            
            if (pumps[i].state == PUMP_AUTO_ACTIVE) {
                deactivate_pump(i, "sensor_fault");
            }
            
        } else if (healthy && pumps[i].sensorFault) {
            pumps[i].sensorFault = false;
            ESP_LOGI("FIRE_SYSTEM", "Sensor %s RESTORED", pumps[i].name);
        }
    }
    
    ESP_LOGI("FIRE_SYSTEM", "===========================");
}

bool is_sensor_healthy(int index) {
    // Primary check: decoded 0–20 mA state must not be a fault state.
    // FLAME_SIGNAL_FAULT  = open circuit / power lost → cannot detect fire.
    // FLAME_SIGNAL_BIT_FAULT = self-test failure      → reliability degraded.
    FlameSignalState sig = pumps[index].flameSignalState;
    if (sig == FLAME_SIGNAL_FAULT || sig == FLAME_SIGNAL_BIT_FAULT) {
        return false;
    }

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
    ESP_LOGI("FIRE_SYSTEM", "Flame confirmed on %s (IR: %.2f%%) - Starting suppression", 
             pumps[sensorIndex].name, pumps[sensorIndex].currentIRValue);
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

        // Camera activates on WARNING (pre-alarm) or confirmed FIRE ALARM.
        // This gives an earlier visual record while still being above normal 4 mA.
        if (pumps[i].flameSignalState == FLAME_SIGNAL_FIRE ||
            pumps[i].flameSignalState == FLAME_SIGNAL_WARNING) {
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
            ESP_LOGI("FIRE_SYSTEM", "Door OPENED");
        } else {
            unsigned long openDuration = (now - doorOpenTime) / 1000;
            ESP_LOGI("FIRE_SYSTEM", "Door CLOSED (was open for %lu seconds)", openDuration);
        }
    }
    
    static bool warningIssued = false;
    if (doorOpen && (now - doorOpenTime > DOOR_ALERT_DELAY)) {
        if (!warningIssued) {
            ESP_LOGW("FIRE_SYSTEM", "WARNING: Door has been open for over 5 minutes!");
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
    ESP_LOGI("FIRE_SYSTEM", "========================================");
    ESP_LOGI("FIRE_SYSTEM", "  GUARDIAN FIRE SUPPRESSION SYSTEM");
    ESP_LOGI("FIRE_SYSTEM", "           INITIALIZING");
    ESP_LOGI("FIRE_SYSTEM", "========================================");
    
    // Initialize arrays first
    initialize_arrays();
  
    ESP_LOGI("FIRE_SYSTEM", "Initializing PCA9555");
    
    esp_err_t ret = pca9555_init(&pca_dev, 
                                 PCA9555_I2C_ADDRESS,
                                 PCA9555_I2C_PORT,
                                 PCA9555_I2C_SDA_GPIO,
                                 PCA9555_I2C_SCL_GPIO);
    
    if (ret != ESP_OK) {
        ESP_LOGE("FIRE_SYSTEM", "PCA9555 initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGE("FIRE_SYSTEM", "Check I2C wiring and address!");
        
        // 🆕 SEND CRITICAL ALERT
        send_alert_pca9555_fail(esp_err_to_name(ret), 
            "PCA9555 I2C initialization failed - Check wiring and address");
    }else {
        ESP_LOGI("FIRE_SYSTEM", "PCA9555 initialized successfully");
        
        ret = pca9555_configure_all_outputs(&pca_dev);
        if (ret != ESP_OK) {
            ESP_LOGE("FIRE_SYSTEM", "Failed to configure PCA9555 outputs: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI("FIRE_SYSTEM", "PCA9555 ports configured as outputs");
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

    ESP_LOGI("FIRE_SYSTEM", "Hardware initialized");
    ESP_LOGI("FIRE_SYSTEM", "System ARMED and ready");
    ESP_LOGI("FIRE_SYSTEM", "========================================");
}



void reset_system_to_defaults(void) {
    ESP_LOGI("FIRE_SYSTEM", "===== RESETTING SYSTEM TO DEFAULTS =====");
    
    // 1. Clear emergency stop if active
    if (emergencyStopActive) {
        ESP_LOGI("FIRE_SYSTEM", "Clearing emergency stop...");
        emergencyStopActive = false;
    }
    
    // 2. Force stop ALL pumps (override all protections)
    ESP_LOGI("FIRE_SYSTEM", "Stopping all pumps...");
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
            
            ESP_LOGI("FIRE_SYSTEM", "Pump %d (%s) reset to OFF", i+1, pumps[i].name);
        }
    }
    
    // 3. Reset profile to default (WILDLAND_STANDARD)
    ESP_LOGI("FIRE_SYSTEM", "Resetting profile to WILDLAND_STANDARD...");
    currentProfile = WILDLAND_STANDARD;
    
    // 4. Clear water lockout (allow system to re-evaluate)
    if (waterLockout) {
        ESP_LOGI("FIRE_SYSTEM", "Clearing water lockout...");
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
    
    ESP_LOGI("FIRE_SYSTEM", "===== SYSTEM RESET COMPLETE =====");
    ESP_LOGI("FIRE_SYSTEM", "- All pumps: OFF");
    ESP_LOGI("FIRE_SYSTEM", "- Profile: WILDLAND_STANDARD");
    ESP_LOGI("FIRE_SYSTEM", "- Emergency Stop: Cleared");
    ESP_LOGI("FIRE_SYSTEM", "- Water Lockout: Cleared");
    ESP_LOGI("FIRE_SYSTEM", "- System Armed: YES");
    ESP_LOGI("FIRE_SYSTEM", "==========================================");
}

// ========================================
// FIRE DETECTION FUNCTIONS
// ========================================
// Note: this used to also classify the overall fire event as
// SINGLE_SECTOR / MULTIPLE_SECTORS / FULL_SYSTEM. That classification has
// been removed - callers should use the detected-sector info below (which
// sectors are on fire, how many, by name) directly instead.

void update_fire_detection_info(void) {
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Get current IR sensor values
    const char* sectorNames[4] = {"N", "S", "E", "W"};
    // Reset fire info
    int activeCount = 0;
    currentFireInfo.detectedSectorNames[0] = '\0';
    
    // Check each sector - uses global FIRE_THRESHOLD from fire_system.h
    for (int i = 0; i < 4; i++) {
        // Use the decoded 0–20 mA state: only FIRE confirms a sector as active.
        // This also excludes sensors that are in BIT or BIT-fault states.
        currentFireInfo.detectedSectors[i] = (pumps[i].flameSignalState == FLAME_SIGNAL_FIRE)
                                           && !pumps[i].inAuxBit
                                           && !pumps[i].sensorFault;
        if (currentFireInfo.detectedSectors[i]) {
            activeCount++;
            
            // Build sector names string
            if (strlen(currentFireInfo.detectedSectorNames) > 0) {
                strncat(currentFireInfo.detectedSectorNames, ",", 
                       sizeof(currentFireInfo.detectedSectorNames) - strlen(currentFireInfo.detectedSectorNames) - 1);
            }
            strncat(currentFireInfo.detectedSectorNames, sectorNames[i],
                   sizeof(currentFireInfo.detectedSectorNames) - strlen(currentFireInfo.detectedSectorNames) - 1);
        }
    }
    
    currentFireInfo.detectedSectorCount = activeCount;
    currentFireInfo.lastUpdateTime = now;
}


int get_active_fire_sector_count(void) {
    update_fire_detection_info();
    return currentFireInfo.detectedSectorCount;
}


const char* get_active_sectors_string(void) {
    update_fire_detection_info();
    return currentFireInfo.detectedSectorNames;
}


bool is_sector_on_fire(int sectorIndex) {
    if (sectorIndex < 0 || sectorIndex >= 4) return false;
    update_fire_detection_info();
    return currentFireInfo.detectedSectors[sectorIndex];
}


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