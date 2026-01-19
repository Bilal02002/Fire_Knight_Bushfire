# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS 
        "main.c"
        "clsPCA9555.c"
        "fire_system.c"
        "wifi_config.c"
        "spiffs_handler.c"
        "time_manager.c"
        "gsm_manager.c"
        #"device_shadow.c"
        "clsPCA9555.h"
        "fire_system.h"
        "wifi_config.h"
        "spiffs_handler.h"
        "time_manager.h"
        "gsm_manager.h"
        "gsm_config.h"
       # "device_shadow.h"
        
    INCLUDE_DIRS 
    PRIV_INCLUDE_DIRS 
        # Add any private include directories here if needed
        
    REQUIRES        
    PRIV_REQUIRES
)