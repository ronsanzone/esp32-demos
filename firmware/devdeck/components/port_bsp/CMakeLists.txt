idf_component_register(
    SRCS 
    "i2c_bsp.cpp" 
    "display_bsp.cpp" 
    PRIV_REQUIRES 
    esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd
    pmicpower
    espressif__esp_lcd_sh8601
    waveshare__esp_lcd_touch_cst9217
    esp_timer
    INCLUDE_DIRS 
    "./")
