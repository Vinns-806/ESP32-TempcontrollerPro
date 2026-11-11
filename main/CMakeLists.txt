idf_component_register(SRCS "hello_world_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver freertos esp_system esp_timer esp_driver_i2c)