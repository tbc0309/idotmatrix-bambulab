set(PIXEL "ported/features/pixel_screen")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../web/online-update.js" "${CMAKE_CURRENT_LIST_DIR}/../resources/pixel-layout.json" "${CMAKE_CURRENT_LIST_DIR}/../scripts/generate_pixel_layout.py" "${CMAKE_CURRENT_LIST_DIR}/../boards/catalog.json")
set(WEB_ASSETS "${CMAKE_BINARY_DIR}/web-assets")
set(ASSETS index_html app_js style_css)
list(TRANSFORM ASSETS PREPEND "${WEB_ASSETS}/")
list(TRANSFORM ASSETS APPEND ".gz")
if(NOT MATRIX_BOARD STREQUAL "clockwise-esp32-hub75")
    set(BLE_SOURCES "${PIXEL}/connection.cpp" "${PIXEL}/link_protocol.cpp" "ported/platform/idf/ble_runtime.cpp")
    set(BLE_COMPONENT bt)
endif()
execute_process(COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../scripts/embed.py" "${WEB_ASSETS}" "${MATRIX_BOARD}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../web/index.html" "${CMAKE_CURRENT_LIST_DIR}/../web/app.js" "${CMAKE_CURRENT_LIST_DIR}/../web/printer-manager.js" "${CMAKE_CURRENT_LIST_DIR}/../web/style.css" "${CMAKE_CURRENT_LIST_DIR}/../web/pixel-art.js" "${CMAKE_CURRENT_LIST_DIR}/../web/i18n.js" "${CMAKE_CURRENT_LIST_DIR}/../web/forms.js" "${CMAKE_CURRENT_LIST_DIR}/../web/timezone-picker.js" "${CMAKE_CURRENT_LIST_DIR}/../resources/timezones/windows-zones.json" "${CMAKE_CURRENT_LIST_DIR}/../resources/timezones/windows-source.json" "${CMAKE_CURRENT_LIST_DIR}/../resources/locales.json" "${CMAKE_CURRENT_LIST_DIR}/../scripts/generate_locales.py" "${CMAKE_CURRENT_LIST_DIR}/../scripts/embed.py")
idf_component_register(SRCS
 "${MATRIX_BOARD_DIR}/board.cpp"
 "app.cpp" "vendor/captive_dns/dns_server.c" "config.cpp" "network.cpp" "printer.cpp" "telemetry.cpp" "web.cpp" "cloud.cpp" "cloud_sessions.cpp" "lan_validation.cpp" "serial.cpp" "boot_button.cpp" "update.cpp" "firmware_writer.cpp"
 "display/render32/renderer.cpp" "${PIXEL}/core.cpp" "${PIXEL}/display_policy.cpp" ${BLE_SOURCES}
 "ported/xtouch/ams_units.c" "ported/xtouch/vault.cpp"
 INCLUDE_DIRS "." "ported" "vendor/ArduinoJson" "../boards/common" "${CMAKE_BINARY_DIR}/boards"
 REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client app_update
 nvs_flash ${BLE_COMPONENT} mqtt mbedtls esp_timer esp_driver_gpio esp_driver_ledc esp_driver_uart esp_hw_support
 PRIV_REQUIRES esp_driver_i2c esp_driver_rmt
 EMBED_FILES ${ASSETS})
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u idotmatrix_app_board")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/ported/platform/idf/json.h>" -Wno-error=use-after-free -Wno-error=misleading-indentation)

if(MATRIX_BOARD STREQUAL "esp32c3-oled-042")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MATRIX_BOARD_OLED)
endif()

if(MATRIX_BOARD STREQUAL "esp32c3-rgb24")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MATRIX_BOARD_RGB_RING)
endif()

if(MATRIX_BOARD STREQUAL "clockwise-esp32-hub75")
    option(CLOCKWISE_SCALE_LAYOUT "Use the 32x32 printer layout at 2x scale" ON)
    if(CLOCKWISE_SCALE_LAYOUT)
        set_source_files_properties("${MATRIX_BOARD_DIR}/board.cpp"
            PROPERTIES COMPILE_DEFINITIONS CLOCKWISE_SCALE_LAYOUT)
    else()
        target_sources(${COMPONENT_LIB} PRIVATE "${MATRIX_BOARD_DIR}/layouts/native64/native_renderer.cpp")
    endif()
    set(HUB75_DRIVER "${MATRIX_BOARD_DIR}/vendor/src")
    target_sources(${COMPONENT_LIB} PRIVATE
        "${HUB75_DRIVER}/../qrcode/qrcodegen.c"
        "${HUB75_DRIVER}/ESP32-HUB75-MatrixPanel-I2S-DMA.cpp"
        "${HUB75_DRIVER}/ESP32-HUB75-MatrixPanel-leddrivers.cpp"
        "${HUB75_DRIVER}/platforms/esp32/esp32_i2s_parallel_dma.cpp")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${HUB75_DRIVER}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MATRIX_BOARD_HUB75 NO_GFX)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_hw_support)
endif()
