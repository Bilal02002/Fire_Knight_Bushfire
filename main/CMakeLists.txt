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
        
    INCLUDE_DIRS 
    PRIV_INCLUDE_DIRS 
        # Add any private include directories here if needed
        
    REQUIRES        
    PRIV_REQUIRES
)