set(HUB75 "${CMAKE_CURRENT_LIST_DIR}/../../clockwise-esp32-hub75/vendor/src")
idf_component_register(SRCS "app.cpp" "ble.c"
    "${HUB75}/ESP32-HUB75-MatrixPanel-I2S-DMA.cpp"
    "${HUB75}/ESP32-HUB75-MatrixPanel-leddrivers.cpp"
    "${HUB75}/platforms/esp32/esp32_i2s_parallel_dma.cpp"
    PRIV_INCLUDE_DIRS "${HUB75}"
    PRIV_REQUIRES bt nvs_flash esp_timer esp_driver_gpio esp_hw_support)
target_compile_definitions(${COMPONENT_LIB} PRIVATE NO_GFX)
