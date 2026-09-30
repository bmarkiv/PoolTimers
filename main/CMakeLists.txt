idf_component_register(
    SRCS "../main.cpp"
    INCLUDE_DIRS ".."
    REQUIRES ESP32WifiManager cjson nvs_flash esp_driver_gpio esp_timer
)