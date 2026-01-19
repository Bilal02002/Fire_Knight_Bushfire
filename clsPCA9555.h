/*#ifndef _CLS_PCA9555_H_
#define _CLS_PCA9555_H_

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c.h"
#include "esp_err.h"
#include "driver/gpio.h"

// PCA9555 I2C Address (default with all address pins grounded)
#define PCA9555_I2C_ADDRESS_BASE 0x20

// PCA9555 Registers
#define PCA9555_REG_INPUT_0     0x00
#define PCA9555_REG_INPUT_1     0x01
#define PCA9555_REG_OUTPUT_0    0x02
#define PCA9555_REG_OUTPUT_1    0x03
#define PCA9555_REG_POLARITY_0  0x04
#define PCA9555_REG_POLARITY_1  0x05
#define PCA9555_REG_CONFIG_0    0x06
#define PCA9555_REG_CONFIG_1    0x07

// I2C Configuration
#define PCA9555_I2C_MASTER_FREQ_HZ       100000
#define PCA9555_I2C_MASTER_TIMEOUT_MS    1000

// PCA9555 Device Structure - SIMPLIFIED FOR LEGACY DRIVER
typedef struct {
    uint8_t address;
    i2c_port_t i2c_port;
    bool initialized;
} pca9555_t;

// Function prototypes
esp_err_t pca9555_init(pca9555_t *dev, uint8_t address, i2c_port_t i2c_port, 
                      gpio_num_t sda_gpio, gpio_num_t scl_gpio);
esp_err_t pca9555_deinit(pca9555_t *dev);

// Register operations
esp_err_t pca9555_write_register(pca9555_t *dev, uint8_t reg, uint8_t value);
esp_err_t pca9555_read_register(pca9555_t *dev, uint8_t reg, uint8_t *value);

// Configuration
esp_err_t pca9555_configure_port0_output(pca9555_t *dev);
esp_err_t pca9555_configure_port1_output(pca9555_t *dev);
esp_err_t pca9555_configure_all_outputs(pca9555_t *dev);

// Port control
esp_err_t pca9555_set_port0_output(pca9555_t *dev, uint8_t value);
esp_err_t pca9555_set_port1_output(pca9555_t *dev, uint8_t value);
esp_err_t pca9555_read_port0_output(pca9555_t *dev, uint8_t *value);
esp_err_t pca9555_read_port1_output(pca9555_t *dev, uint8_t *value);

// Pin control
esp_err_t pca9555_set_port0_pin_high(pca9555_t *dev, uint8_t pin);
esp_err_t pca9555_set_port0_pin_low(pca9555_t *dev, uint8_t pin);
esp_err_t pca9555_set_pin_state(pca9555_t *dev, uint8_t port, uint8_t pin, bool state);

// Debug functions
void pca9555_scan_devices(i2c_port_t i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);
esp_err_t pca9555_debug_test(pca9555_t *dev);
bool pca9555_is_initialized(pca9555_t *dev);

#endif // _CLS_PCA9555_H_*/


//--------------------TEST



#ifndef _CLS_PCA9555_H_
#define _CLS_PCA9555_H_

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c.h"
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// PCA9555 I2C Address (default with all address pins grounded)
#define PCA9555_I2C_ADDRESS_BASE 0x20

// PCA9555 Registers
#define PCA9555_REG_INPUT_0     0x00
#define PCA9555_REG_INPUT_1     0x01
#define PCA9555_REG_OUTPUT_0    0x02
#define PCA9555_REG_OUTPUT_1    0x03
#define PCA9555_REG_POLARITY_0  0x04
#define PCA9555_REG_POLARITY_1  0x05
#define PCA9555_REG_CONFIG_0    0x06
#define PCA9555_REG_CONFIG_1    0x07

// I2C Configuration
#define PCA9555_I2C_MASTER_FREQ_HZ       100000
#define PCA9555_I2C_MASTER_TIMEOUT_MS    1000

// PCA9555 Device Structure
typedef struct {
    uint8_t address;
    i2c_port_t i2c_port;
    bool initialized;
} pca9555_t;

// ==========================================================================
// BASIC DEVICE OPERATIONS
// ==========================================================================
esp_err_t pca9555_init(pca9555_t *dev, uint8_t address, i2c_port_t i2c_port, 
                      gpio_num_t sda_gpio, gpio_num_t scl_gpio);
esp_err_t pca9555_deinit(pca9555_t *dev);
bool pca9555_is_initialized(pca9555_t *dev);

// ==========================================================================
// REGISTER OPERATIONS
// ==========================================================================
esp_err_t pca9555_write_register(pca9555_t *dev, uint8_t reg, uint8_t value);
esp_err_t pca9555_read_register(pca9555_t *dev, uint8_t reg, uint8_t *value);

// ==========================================================================
// CONFIGURATION FUNCTIONS
// ==========================================================================
esp_err_t pca9555_configure_port0_output(pca9555_t *dev);
esp_err_t pca9555_configure_port1_output(pca9555_t *dev);
esp_err_t pca9555_configure_all_outputs(pca9555_t *dev);

// ==========================================================================
// PORT CONTROL FUNCTIONS
// ==========================================================================
esp_err_t pca9555_set_port0_output(pca9555_t *dev, uint8_t value);
esp_err_t pca9555_set_port1_output(pca9555_t *dev, uint8_t value);
esp_err_t pca9555_read_port0_output(pca9555_t *dev, uint8_t *value);
esp_err_t pca9555_read_port1_output(pca9555_t *dev, uint8_t *value);

// ==========================================================================
// PIN CONTROL FUNCTIONS
// ==========================================================================
esp_err_t pca9555_set_port0_pin_high(pca9555_t *dev, uint8_t pin);
esp_err_t pca9555_set_port0_pin_low(pca9555_t *dev, uint8_t pin);
esp_err_t pca9555_set_pin_state(pca9555_t *dev, uint8_t port, uint8_t pin, bool state);

// ==========================================================================
// CUSTOM PIN CONTROL FUNCTIONS
// ==========================================================================
esp_err_t pca9555_set_pin_high(pca9555_t *dev, uint8_t port, uint8_t pin);
esp_err_t pca9555_set_pin_low(pca9555_t *dev, uint8_t port, uint8_t pin);
esp_err_t pca9555_toggle_pin(pca9555_t *dev, uint8_t port, uint8_t pin);
esp_err_t pca9555_read_pin_state(pca9555_t *dev, uint8_t port, uint8_t pin, bool *state);
esp_err_t pca9555_read_all_pins(pca9555_t *dev, uint8_t *port0_state, uint8_t *port1_state);

// ==========================================================================
// DEBUG & UTILITY FUNCTIONS
// ==========================================================================
void pca9555_scan_devices(i2c_port_t i2c_port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);
esp_err_t pca9555_debug_test(pca9555_t *dev);

#ifdef __cplusplus
}
#endif

#endif // _CLS_PCA9555_H_