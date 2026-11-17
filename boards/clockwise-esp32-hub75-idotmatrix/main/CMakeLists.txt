if(NOT "$ENV{CW_NATIVE_LVGL}" STREQUAL "")
    set(LVGL_LOCAL_REQUIRES lvgl littlefs)
endif()
if("$ENV{CW_NATIVE_WIFI}" STREQUAL "ON")
    set(DNS "${CMAKE_CURRENT_LIST_DIR}/../../../main/vendor/captive_dns")
    set(PORTAL_SOURCES "portal.c" "${DNS}/dns_server.c")
    set(WIFI_REQUIRES esp_wifi esp_netif esp_event esp_http_server lwip)
endif()
set(HUB75 "${CMAKE_CURRENT_LIST_DIR}/../../clockwise-esp32-hub75/vendor/src")
idf_component_register(SRCS "app.cpp" "ble.c" "effects.cpp" "media.cpp" "text_media.cpp" "clock_view.cpp" "rhythm.cpp" "events.cpp" ${PORTAL_SOURCES}
    "${HUB75}/ESP32-HUB75-MatrixPanel-I2S-DMA.cpp"
    "${HUB75}/ESP32-HUB75-MatrixPanel-leddrivers.cpp"
    "${HUB75}/platforms/esp32/esp32_i2s_parallel_dma.cpp"
    PRIV_INCLUDE_DIRS "${HUB75}" ${DNS}
    PRIV_REQUIRES bt nvs_flash esp_timer esp_driver_gpio esp_hw_support esp_partition ${WIFI_REQUIRES} ${LVGL_LOCAL_REQUIRES})
target_compile_definitions(${COMPONENT_LIB} PRIVATE NO_GFX)

if("$ENV{CW_NATIVE_WIFI}" STREQUAL "ON")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CLOCKWISE_WITH_WIFI=1)
    set(PORTAL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/portal_page.h")
    add_custom_command(
        OUTPUT "${PORTAL_HEADER}"
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/embed_portal.py"
                "${CMAKE_CURRENT_LIST_DIR}/portal_page.html" "${PORTAL_HEADER}"
        DEPENDS embed_portal.py portal_page.html
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE "${PORTAL_HEADER}")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
