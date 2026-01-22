/*#include "clsPCA9555.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "PCA9555";

// I2C Configuration
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0
#define I2C_MASTER_TIMEOUT_MS       1000

//==========================================================================
// I2C SCANNER - USING LEGACY DRIVER
//==========================================================================
void pca9555_scan_devices(i2c_port_t i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio) {
    printf("\n[I2C SCANNER] Starting I2C bus scan...\n");
    printf("[I2C SCANNER] Port: %d, SDA: GPIO %d, SCL: GPIO %d\n", i2c_port, sda_gpio, scl_gpio);

    // Initialize I2C with the specified pins
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(i2c_port, &conf);
    if (ret != ESP_OK) {
        printf("[I2C SCANNER ERROR] I2C config failed: %s\n", esp_err_to_name(ret));
        return;
    }

    ret = i2c_driver_install(i2c_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
    if (ret != ESP_OK) {
        printf("[I2C SCANNER ERROR] I2C driver install failed: %s\n", esp_err_to_name(ret));
        return;
    }

    printf("[I2C SCANNER] Scanning addresses 0x08 to 0x77...\n");
    
    int found_count = 0;
    for (uint8_t address = 0x08; address < 0x78; address++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);

        ret = i2c_master_cmd_begin(i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            printf("[I2C SCANNER] Found device at address: 0x%02X\n", address);
            found_count++;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Cleanup
    i2c_driver_delete(i2c_port);

    if (found_count == 0) {
        printf("[I2C SCANNER] ✗ No I2C devices found!\n");
    } else {
        printf("[I2C SCANNER] Found %d device(s) total\n", found_count);
    }
    printf("[I2C SCANNER] Scan complete\n\n");
}

//==========================================================================
// INITIALIZE PCA9555 - CORRECTED VERSION
//==========================================================================
esp_err_t pca9555_init(pca9555_t *dev, uint8_t address, i2c_port_t i2c_port, 
                      gpio_num_t sda_gpio, gpio_num_t scl_gpio) {
    // CRITICAL: Check for NULL pointer first
    if (dev == NULL) {
        ESP_LOGE(TAG, "PCA9555 init failed: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    printf("[PCA9555] Initializing with legacy I2C driver...\n");
    printf("[PCA9555] Address: 0x%02X, I2C Port: %d\n", address, i2c_port);
    printf("[PCA9555] SDA: GPIO %d, SCL: GPIO %d\n", sda_gpio, scl_gpio);

    // Initialize device structure
    memset(dev, 0, sizeof(pca9555_t));
    dev->address = address;
    dev->i2c_port = i2c_port;
    dev->initialized = false;

    // First, run I2C scanner to check if devices are present
    printf("[PCA9555] Scanning for I2C devices before initialization...\n");
    pca9555_scan_devices(i2c_port, sda_gpio, scl_gpio);

    // Initialize I2C master
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(i2c_port, &conf);
    if (ret != ESP_OK) {
        printf("[PCA9555 ERROR] I2C config failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(i2c_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
    if (ret != ESP_OK) {
        printf("[PCA9555 ERROR] I2C driver install failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    printf("[PCA9555] I2C master initialized successfully\n");

    // Give the device a moment to initialize
    vTaskDelay(pdMS_TO_TICKS(100));

    // SET INITIALIZED TO TRUE HERE - before communication test
    dev->initialized = true;

    // Test communication by reading INPUT register
    printf("[PCA9555] Testing communication...\n");
    
    uint8_t test_value;
    ret = pca9555_read_register(dev, PCA9555_REG_INPUT_0, &test_value);
    
    if (ret == ESP_OK) {
        printf("[PCA9555] SUCCESS: Communication test passed. INPUT_0 = 0x%02X\n", test_value);
        
        // Configure the device as outputs
        printf("[PCA9555] Configuring device as outputs...\n");
        ret = pca9555_configure_all_outputs(dev);
        if (ret == ESP_OK) {
            printf("[PCA9555] Device configured successfully\n");
            
            // Set all outputs to LOW for safety
            pca9555_set_port0_output(dev, 0x00);
            pca9555_set_port1_output(dev, 0x00);
            printf("[PCA9555] All outputs set to LOW\n");
        } else {
            printf("[PCA9555 WARNING] Configuration failed: %s\n", esp_err_to_name(ret));
        }
        
        printf("[PCA9555] SUCCESS: Initialized at address 0x%02X\n", address);
        return ESP_OK;
    } else {
        printf("[PCA9555 ERROR] Communication test failed: %s\n", esp_err_to_name(ret));
        // Cleanup on failure
        dev->initialized = false;
        i2c_driver_delete(i2c_port);
        return ret;
    }
}

//==========================================================================
// WRITE REGISTER - CORRECTED VERSION
//==========================================================================
esp_err_t pca9555_write_register(pca9555_t *dev, uint8_t reg, uint8_t value) {
    // Check for NULL pointer first
    if (dev == NULL) {
        ESP_LOGE(TAG, "Write register: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!dev->initialized) {
        ESP_LOGE(TAG, "Write register: device not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C command link");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;
    
    i2c_master_start(cmd);
    ret = i2c_master_write_byte(cmd, (dev->address << 1) | I2C_MASTER_WRITE, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    ret = i2c_master_write_byte(cmd, reg, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    ret = i2c_master_write_byte(cmd, value, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    i2c_master_stop(cmd);
    
    ret = i2c_master_cmd_begin(dev->i2c_port, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write register 0x%02X = 0x%02X failed: %s", reg, value, esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Write register 0x%02X = 0x%02X - SUCCESS", reg, value);
    }
    
    return ret;
}

//==========================================================================
// READ REGISTER - CORRECTED VERSION
//==========================================================================
esp_err_t pca9555_read_register(pca9555_t *dev, uint8_t reg, uint8_t *value) {
    // CRITICAL: Check for NULL pointers FIRST
    if (dev == NULL || value == NULL) {
        ESP_LOGE(TAG, "Read register: NULL pointer (dev=%p, value=%p)", dev, value);
        return ESP_ERR_INVALID_ARG;
    }

    // Now it's safe to check dev->initialized
    if (!dev->initialized) {
        ESP_LOGE(TAG, "Read register: device not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // First write the register address
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C command link");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;
    
    i2c_master_start(cmd);
    ret = i2c_master_write_byte(cmd, (dev->address << 1) | I2C_MASTER_WRITE, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    ret = i2c_master_write_byte(cmd, reg, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    i2c_master_stop(cmd);
    
    ret = i2c_master_cmd_begin(dev->i2c_port, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read register 0x%02X - address write failed: %s", reg, esp_err_to_name(ret));
        return ret;
    }

    // Then read the value
    cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C command link for read");
        return ESP_ERR_NO_MEM;
    }
    
    i2c_master_start(cmd);
    ret = i2c_master_write_byte(cmd, (dev->address << 1) | I2C_MASTER_READ, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    ret = i2c_master_read_byte(cmd, value, I2C_MASTER_NACK);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    i2c_master_stop(cmd);
    
    ret = i2c_master_cmd_begin(dev->i2c_port, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read register 0x%02X failed: %s", reg, esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Read register 0x%02X = 0x%02X - SUCCESS", reg, *value);
    }
    
    return ret;
}

//==========================================================================
// CONFIGURE OUTPUTS
//==========================================================================
esp_err_t pca9555_configure_port0_output(pca9555_t *dev) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "configure_port0_output: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = pca9555_write_register(dev, PCA9555_REG_CONFIG_0, 0x00);
    if (ret == ESP_OK) {
        ret = pca9555_write_register(dev, PCA9555_REG_OUTPUT_0, 0x00);
    }
    return ret;
}

esp_err_t pca9555_configure_port1_output(pca9555_t *dev) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "configure_port1_output: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = pca9555_write_register(dev, PCA9555_REG_CONFIG_1, 0x00);
    if (ret == ESP_OK) {
        ret = pca9555_write_register(dev, PCA9555_REG_OUTPUT_1, 0x00);
    }
    return ret;
}

esp_err_t pca9555_configure_all_outputs(pca9555_t *dev) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "configure_all_outputs: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = pca9555_configure_port0_output(dev);
    if (ret == ESP_OK) {
        ret = pca9555_configure_port1_output(dev);
    }
    return ret;
}

//==========================================================================
// PIN CONTROL
//==========================================================================
esp_err_t pca9555_set_port0_pin_high(pca9555_t *dev, uint8_t pin) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_port0_pin_high: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (pin > 7 || !dev->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t current_value;
    esp_err_t ret = pca9555_read_port0_output(dev, &current_value);
    if (ret != ESP_OK) {
        return ret;
    }
    
    current_value |= (1 << pin);
    return pca9555_set_port0_output(dev, current_value);
}

esp_err_t pca9555_set_port0_pin_low(pca9555_t *dev, uint8_t pin) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_port0_pin_low: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (pin > 7 || !dev->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t current_value;
    esp_err_t ret = pca9555_read_port0_output(dev, &current_value);
    if (ret != ESP_OK) {
        return ret;
    }
    
    current_value &= ~(1 << pin);
    return pca9555_set_port0_output(dev, current_value);
}

esp_err_t pca9555_set_pin_state(pca9555_t *dev, uint8_t port, uint8_t pin, bool state) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_pin_state: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (port > 1 || pin > 7 || !dev->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t output_reg = (port == 0) ? PCA9555_REG_OUTPUT_0 : PCA9555_REG_OUTPUT_1;
    uint8_t current_state;
    
    esp_err_t ret = pca9555_read_register(dev, output_reg, &current_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_pin_state: failed to read current state");
        return ret;
    }
    
    if (state) {
        current_state |= (1 << pin);
    } else {
        current_state &= ~(1 << pin);
    }
    
    return pca9555_write_register(dev, output_reg, current_state);
}

//==========================================================================
// PORT CONTROL
//==========================================================================
esp_err_t pca9555_read_port0_output(pca9555_t *dev, uint8_t *value) {
    if (dev == NULL || value == NULL) {
        ESP_LOGE(TAG, "read_port0_output: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9555_read_register(dev, PCA9555_REG_OUTPUT_0, value);
}

esp_err_t pca9555_set_port0_output(pca9555_t *dev, uint8_t value) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_port0_output: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9555_write_register(dev, PCA9555_REG_OUTPUT_0, value);
}

esp_err_t pca9555_set_port1_output(pca9555_t *dev, uint8_t value) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_port1_output: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9555_write_register(dev, PCA9555_REG_OUTPUT_1, value);
}

esp_err_t pca9555_read_port1_output(pca9555_t *dev, uint8_t *value) {
    if (dev == NULL || value == NULL) {
        ESP_LOGE(TAG, "read_port1_output: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9555_read_register(dev, PCA9555_REG_OUTPUT_1, value);
}

//==========================================================================
// DEBUG TEST FUNCTION
//==========================================================================
esp_err_t pca9555_debug_test(pca9555_t *dev) {
    if (dev == NULL) {
        printf("[PCA9555 DEBUG] Device pointer is NULL\n");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        printf("[PCA9555 DEBUG] Device not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }
    
    printf("\n[PCA9555 DEBUG] Starting comprehensive test...\n");
    
    // Read all registers
    uint8_t registers[8];
    const char* reg_names[] = {
        "INPUT_0", "INPUT_1", "OUTPUT_0", "OUTPUT_1",
        "POLARITY_0", "POLARITY_1", "CONFIG_0", "CONFIG_1"
    };
    
    printf("[PCA9555 DEBUG] Reading all registers:\n");
    for (int i = 0; i < 8; i++) {
        esp_err_t ret = pca9555_read_register(dev, i, &registers[i]);
        if (ret == ESP_OK) {
            printf("[PCA9555 DEBUG] %s (0x%02X) = 0x%02X\n", reg_names[i], i, registers[i]);
        } else {
            printf("[PCA9555 DEBUG] %s (0x%02X) read failed: %s\n", reg_names[i], i, esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    printf("[PCA9555 DEBUG] Test complete\n\n");
    return ESP_OK;
}

//==========================================================================
// DEINITIALIZE
//==========================================================================
esp_err_t pca9555_deinit(pca9555_t *dev) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        printf("[PCA9555] Device not initialized, nothing to deinit\n");
        return ESP_OK;
    }
    
    // Set all outputs to LOW before deinitializing for safety
    pca9555_set_port0_output(dev, 0x00);
    pca9555_set_port1_output(dev, 0x00);
    
    // Delete I2C driver
    i2c_driver_delete(dev->i2c_port);
    
    dev->initialized = false;
    
    printf("[PCA9555] Device deinitialized successfully\n");
    return ESP_OK;
}

//==========================================================================
// CHECK INITIALIZATION STATUS
//==========================================================================
bool pca9555_is_initialized(pca9555_t *dev) {
    return (dev != NULL && dev->initialized);
}*/


/////----------------TEST 



#include "clsPCA9555.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "PCA9555";

// I2C Configuration
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0
#define I2C_MASTER_TIMEOUT_MS       1000

//==========================================================================
// I2C SCANNER - USING LEGACY DRIVER
//==========================================================================
void pca9555_scan_devices(i2c_port_t i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio) {

    // Initialize I2C with the specified pins
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(i2c_port, &conf);
    if (ret != ESP_OK) {
        printf("[I2C SCANNER ERROR] I2C config failed: %s\n", esp_err_to_name(ret));
        return;
    }

    ret = i2c_driver_install(i2c_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
    if (ret != ESP_OK) {
        printf("[I2C SCANNER ERROR] I2C driver install failed: %s\n", esp_err_to_name(ret));
        return;
    }

    
    int found_count = 0;
    for (uint8_t address = 0x08; address < 0x78; address++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);

        ret = i2c_master_cmd_begin(i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            printf("[I2C SCANNER] Found device at address: 0x%02X\n", address);
            found_count++;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Cleanup
    i2c_driver_delete(i2c_port);

    if (found_count == 0) {
        printf("[I2C SCANNER] ✗ No I2C devices found!\n");
    } else {
        printf("[I2C SCANNER] Found %d device(s) total\n", found_count);
    }
    printf("[I2C SCANNER] Scan complete\n\n");
}

//==========================================================================
// INITIALIZE PCA9555
//==========================================================================
esp_err_t pca9555_init(pca9555_t *dev, uint8_t address, i2c_port_t i2c_port, 
                      gpio_num_t sda_gpio, gpio_num_t scl_gpio) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "PCA9555 init failed: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize device structure
    memset(dev, 0, sizeof(pca9555_t));
    dev->address = address;
    dev->i2c_port = i2c_port;
    dev->initialized = false;

    // First, run I2C scanner to check if devices are present
    printf("[PCA9555] Scanning for I2C devices before initialization...\n");
    pca9555_scan_devices(i2c_port, sda_gpio, scl_gpio);

    // Initialize I2C master
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(i2c_port, &conf);
    if (ret != ESP_OK) {
        printf("[PCA9555 ERROR] I2C config failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(i2c_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
    if (ret != ESP_OK) {
        printf("[PCA9555 ERROR] I2C driver install failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    // Give the device a moment to initialize
    vTaskDelay(pdMS_TO_TICKS(100));

    // Set initialized to true before communication test
    dev->initialized = true;

    // Test communication by reading INPUT register
    printf("[PCA9555] Testing communication...\n");
    
    uint8_t test_value;
    ret = pca9555_read_register(dev, PCA9555_REG_INPUT_0, &test_value);
    
    if (ret == ESP_OK) {
        
        // Configure the device as outputs
        ret = pca9555_configure_all_outputs(dev);
        if (ret == ESP_OK) {
            
            // Set all outputs to LOW for safety
            pca9555_set_port0_output(dev, 0x00);
            pca9555_set_port1_output(dev, 0x00);
        } else {
            printf("[PCA9555 WARNING] Configuration failed: %s\n", esp_err_to_name(ret));
        }
        printf("[PCA9555] Testing Successfully Completed\n");
        
        return ESP_OK;
    } else {
        printf("[PCA9555 ERROR] Communication test failed: %s\n", esp_err_to_name(ret));
        // Cleanup on failure
        dev->initialized = false;
        i2c_driver_delete(i2c_port);
        return ret;
    }
}

//==========================================================================
// WRITE REGISTER
//==========================================================================
esp_err_t pca9555_write_register(pca9555_t *dev, uint8_t reg, uint8_t value) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "Write register: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!dev->initialized) {
        ESP_LOGE(TAG, "Write register: device not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C command link");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;
    
    i2c_master_start(cmd);
    ret = i2c_master_write_byte(cmd, (dev->address << 1) | I2C_MASTER_WRITE, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    ret = i2c_master_write_byte(cmd, reg, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    ret = i2c_master_write_byte(cmd, value, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    i2c_master_stop(cmd);
    
    ret = i2c_master_cmd_begin(dev->i2c_port, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write register 0x%02X = 0x%02X failed: %s", reg, value, esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Write register 0x%02X = 0x%02X - SUCCESS", reg, value);
    }
    
    return ret;
}

//==========================================================================
// READ REGISTER
//==========================================================================
esp_err_t pca9555_read_register(pca9555_t *dev, uint8_t reg, uint8_t *value) {
    if (dev == NULL || value == NULL) {
        ESP_LOGE(TAG, "Read register: NULL pointer (dev=%p, value=%p)", dev, value);
        return ESP_ERR_INVALID_ARG;
    }

    if (!dev->initialized) {
        ESP_LOGE(TAG, "Read register: device not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // First write the register address
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C command link");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;
    
    i2c_master_start(cmd);
    ret = i2c_master_write_byte(cmd, (dev->address << 1) | I2C_MASTER_WRITE, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    ret = i2c_master_write_byte(cmd, reg, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    i2c_master_stop(cmd);
    
    ret = i2c_master_cmd_begin(dev->i2c_port, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read register 0x%02X - address write failed: %s", reg, esp_err_to_name(ret));
        return ret;
    }

    // Then read the value
    cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C command link for read");
        return ESP_ERR_NO_MEM;
    }
    
    i2c_master_start(cmd);
    ret = i2c_master_write_byte(cmd, (dev->address << 1) | I2C_MASTER_READ, true);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    ret = i2c_master_read_byte(cmd, value, I2C_MASTER_NACK);
    if (ret != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return ret;
    }
    
    i2c_master_stop(cmd);
    
    ret = i2c_master_cmd_begin(dev->i2c_port, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read register 0x%02X failed: %s", reg, esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Read register 0x%02X = 0x%02X - SUCCESS", reg, *value);
    }
    
    return ret;
}

//==========================================================================
// CONFIGURATION FUNCTIONS
//==========================================================================
esp_err_t pca9555_configure_port0_output(pca9555_t *dev) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "configure_port0_output: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = pca9555_write_register(dev, PCA9555_REG_CONFIG_0, 0x00);
    if (ret == ESP_OK) {
        ret = pca9555_write_register(dev, PCA9555_REG_OUTPUT_0, 0x00);
    }
    return ret;
}

esp_err_t pca9555_configure_port1_output(pca9555_t *dev) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "configure_port1_output: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = pca9555_write_register(dev, PCA9555_REG_CONFIG_1, 0x00);
    if (ret == ESP_OK) {
        ret = pca9555_write_register(dev, PCA9555_REG_OUTPUT_1, 0x00);
    }
    return ret;
}

esp_err_t pca9555_configure_all_outputs(pca9555_t *dev) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "configure_all_outputs: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = pca9555_configure_port0_output(dev);
    if (ret == ESP_OK) {
        ret = pca9555_configure_port1_output(dev);
    }
    return ret;
}

//==========================================================================
// PORT CONTROL FUNCTIONS
//==========================================================================
esp_err_t pca9555_set_port0_output(pca9555_t *dev, uint8_t value) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_port0_output: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9555_write_register(dev, PCA9555_REG_OUTPUT_0, value);
}

esp_err_t pca9555_set_port1_output(pca9555_t *dev, uint8_t value) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_port1_output: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9555_write_register(dev, PCA9555_REG_OUTPUT_1, value);
}

esp_err_t pca9555_read_port0_output(pca9555_t *dev, uint8_t *value) {
    if (dev == NULL || value == NULL) {
        ESP_LOGE(TAG, "read_port0_output: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9555_read_register(dev, PCA9555_REG_OUTPUT_0, value);
}

esp_err_t pca9555_read_port1_output(pca9555_t *dev, uint8_t *value) {
    if (dev == NULL || value == NULL) {
        ESP_LOGE(TAG, "read_port1_output: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9555_read_register(dev, PCA9555_REG_OUTPUT_1, value);
}

//==========================================================================
// PIN CONTROL FUNCTIONS
//==========================================================================
esp_err_t pca9555_set_port0_pin_high(pca9555_t *dev, uint8_t pin) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_port0_pin_high: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (pin > 7 || !dev->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t current_value;
    esp_err_t ret = pca9555_read_port0_output(dev, &current_value);
    if (ret != ESP_OK) {
        return ret;
    }
    
    current_value |= (1 << pin);
    return pca9555_set_port0_output(dev, current_value);
}

esp_err_t pca9555_set_port0_pin_low(pca9555_t *dev, uint8_t pin) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_port0_pin_low: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (pin > 7 || !dev->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t current_value;
    esp_err_t ret = pca9555_read_port0_output(dev, &current_value);
    if (ret != ESP_OK) {
        return ret;
    }
    
    current_value &= ~(1 << pin);
    return pca9555_set_port0_output(dev, current_value);
}

esp_err_t pca9555_set_pin_state(pca9555_t *dev, uint8_t port, uint8_t pin, bool state) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_pin_state: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (port > 1 || pin > 7 || !dev->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t output_reg = (port == 0) ? PCA9555_REG_OUTPUT_0 : PCA9555_REG_OUTPUT_1;
    uint8_t current_state;
    
    esp_err_t ret = pca9555_read_register(dev, output_reg, &current_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_pin_state: failed to read current state");
        return ret;
    }
    
    if (state) {
        current_state |= (1 << pin);
    } else {
        current_state &= ~(1 << pin);
    }
    
    return pca9555_write_register(dev, output_reg, current_state);
}

//==========================================================================
// CUSTOM PIN CONTROL FUNCTIONS - DEFINITIONS
//==========================================================================
esp_err_t pca9555_set_pin_high(pca9555_t *dev, uint8_t port, uint8_t pin) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_pin_high: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (port > 1 || pin > 7 || !dev->initialized) {
        ESP_LOGE(TAG, "set_pin_high: invalid parameters (port=%d, pin=%d)", port, pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Setting PORT%d PIN%d HIGH", port, pin);
    return pca9555_set_pin_state(dev, port, pin, true);
}

esp_err_t pca9555_set_pin_low(pca9555_t *dev, uint8_t port, uint8_t pin) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "set_pin_low: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (port > 1 || pin > 7 || !dev->initialized) {
        ESP_LOGE(TAG, "set_pin_low: invalid parameters (port=%d, pin=%d)", port, pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Setting PORT%d PIN%d LOW", port, pin);
    return pca9555_set_pin_state(dev, port, pin, false);
}

esp_err_t pca9555_toggle_pin(pca9555_t *dev, uint8_t port, uint8_t pin) {
    if (dev == NULL) {
        ESP_LOGE(TAG, "toggle_pin: dev is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (port > 1 || pin > 7 || !dev->initialized) {
        ESP_LOGE(TAG, "toggle_pin: invalid parameters (port=%d, pin=%d)", port, pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t output_reg = (port == 0) ? PCA9555_REG_OUTPUT_0 : PCA9555_REG_OUTPUT_1;
    uint8_t current_state;
    
    // Read current state
    esp_err_t ret = pca9555_read_register(dev, output_reg, &current_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "toggle_pin: failed to read current state");
        return ret;
    }
    
    // Toggle the specific pin
    bool current_pin_state = (current_state >> pin) & 0x01;
    bool new_pin_state = !current_pin_state;
    
    ESP_LOGI(TAG, "Toggling PORT%d PIN%d from %s to %s", 
             port, pin, 
             current_pin_state ? "HIGH" : "LOW",
             new_pin_state ? "HIGH" : "LOW");
    
    return pca9555_set_pin_state(dev, port, pin, new_pin_state);
}

esp_err_t pca9555_read_pin_state(pca9555_t *dev, uint8_t port, uint8_t pin, bool *state) {
    if (dev == NULL || state == NULL) {
        ESP_LOGE(TAG, "read_pin_state: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (port > 1 || pin > 7 || !dev->initialized) {
        ESP_LOGE(TAG, "read_pin_state: invalid parameters (port=%d, pin=%d)", port, pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t input_reg = (port == 0) ? PCA9555_REG_INPUT_0 : PCA9555_REG_INPUT_1;
    uint8_t port_state;
    
    esp_err_t ret = pca9555_read_register(dev, input_reg, &port_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read_pin_state: failed to read input register");
        return ret;
    }
    
    *state = (port_state >> pin) & 0x01;
    ESP_LOGD(TAG, "PORT%d PIN%d state: %s", port, pin, *state ? "HIGH" : "LOW");
    
    return ESP_OK;
}

esp_err_t pca9555_read_all_pins(pca9555_t *dev, uint8_t *port0_state, uint8_t *port1_state) {
    if (dev == NULL || port0_state == NULL || port1_state == NULL) {
        ESP_LOGE(TAG, "read_all_pins: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        ESP_LOGE(TAG, "read_all_pins: device not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = pca9555_read_register(dev, PCA9555_REG_INPUT_0, port0_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read_all_pins: failed to read PORT0");
        return ret;
    }
    
    ret = pca9555_read_register(dev, PCA9555_REG_INPUT_1, port1_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read_all_pins: failed to read PORT1");
        return ret;
    }
    
    ESP_LOGD(TAG, "All pins state - PORT0: 0x%02X, PORT1: 0x%02X", *port0_state, *port1_state);
    return ESP_OK;
}

//==========================================================================
// DEBUG TEST FUNCTION
//==========================================================================
esp_err_t pca9555_debug_test(pca9555_t *dev) {
    if (dev == NULL) {
        printf("[PCA9555 DEBUG] Device pointer is NULL\n");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        printf("[PCA9555 DEBUG] Device not initialized\n");
        return ESP_ERR_INVALID_STATE;
    }
    
    printf("\n[PCA9555 DEBUG] Starting comprehensive test...\n");
    
    // Read all registers
    uint8_t registers[8];
    const char* reg_names[] = {
        "INPUT_0", "INPUT_1", "OUTPUT_0", "OUTPUT_1",
        "POLARITY_0", "POLARITY_1", "CONFIG_0", "CONFIG_1"
    };
    
    printf("[PCA9555 DEBUG] Reading all registers:\n");
    for (int i = 0; i < 8; i++) {
        esp_err_t ret = pca9555_read_register(dev, i, &registers[i]);
        if (ret == ESP_OK) {
            printf("[PCA9555 DEBUG] %s (0x%02X) = 0x%02X\n", reg_names[i], i, registers[i]);
        } else {
            printf("[PCA9555 DEBUG] %s (0x%02X) read failed: %s\n", reg_names[i], i, esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    printf("[PCA9555 DEBUG] Test complete\n\n");
    return ESP_OK;
}

//==========================================================================
// DEINITIALIZE
//==========================================================================
esp_err_t pca9555_deinit(pca9555_t *dev) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dev->initialized) {
        printf("[PCA9555] Device not initialized, nothing to deinit\n");
        return ESP_OK;
    }
    
    // Set all outputs to LOW before deinitializing for safety
    pca9555_set_port0_output(dev, 0x00);
    pca9555_set_port1_output(dev, 0x00);
    
    // Delete I2C driver
    i2c_driver_delete(dev->i2c_port);
    
    dev->initialized = false;
    
    printf("[PCA9555] Device deinitialized successfully\n");
    return ESP_OK;
}

//==========================================================================
// CHECK INITIALIZATION STATUS
//==========================================================================
bool pca9555_is_initialized(pca9555_t *dev) {
    return (dev != NULL && dev->initialized);
}