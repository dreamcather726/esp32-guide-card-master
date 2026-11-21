set(BOARD_DIR "boards/jiuchuan-s3")
set(SOURCES
    "main.cc" "application.cc" "device_state_machine.cc" "settings.cc"
    "system_info.cc" "mcp_server.cc" "ota.cc" "assets.cc"
    "audio/audio_codec.cc" "audio/audio_service.cc" "audio/codecs/es8311_audio_codec.cc"
    "audio/processors/audio_debugger.cc"
    "audio/wake_words/afe_wake_word.cc" "audio/wake_words/custom_wake_word.cc"
    "led/circular_strip.cc" "display/display.cc" "display/lcd_display.cc"
    "display/lvgl_display/lvgl_display.cc" "display/lvgl_display/lvgl_theme.cc"
    "display/lvgl_display/lvgl_font.cc" "display/lvgl_display/lvgl_image.cc"
    "display/lvgl_display/emoji_collection.cc"
    "display/lvgl_display/gif/lvgl_gif.cc" "display/lvgl_display/gif/gifdec.c"
    "display/lvgl_display/jpg/jpeg_to_image.c"
    "protocols/protocol.cc" "protocols/mqtt_protocol.cc" "protocols/websocket_protocol.cc"
    "boards/common/board.cc" "boards/common/rndis_board.cc" "boards/common/button.cc"
    "boards/common/backlight.cc" "boards/common/power_save_timer.cc"
    "${BOARD_DIR}/jiuchuan_dev_board.cc" "${BOARD_DIR}/dingwei_image.c" "${BOARD_DIR}/tubiao_image.c"
    "${BOARD_DIR}/location_mqtt_client.cc" "${BOARD_DIR}/position_subscriber.cc"
    "${BOARD_DIR}/sd_content_manager.cc"
)
if(CONFIG_USE_AUDIO_PROCESSOR)
    list(APPEND SOURCES "audio/processors/afe_audio_processor.cc")
else()
    list(APPEND SOURCES "audio/processors/no_audio_processor.cc")
endif()

set(LANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/zh-CN")
file(GLOB LANG_SOUNDS "${LANG_DIR}/*.ogg")
file(GLOB EN_US_SOUNDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/en-US/*.ogg")
foreach(FALLBACK IN LISTS EN_US_SOUNDS)
    get_filename_component(SOUND_NAME "${FALLBACK}" NAME)
    if(NOT EXISTS "${LANG_DIR}/${SOUND_NAME}")
        list(APPEND LANG_SOUNDS "${FALLBACK}")
    endif()
endforeach()
file(GLOB COMMON_SOUNDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg")

idf_component_register(SRCS ${SOURCES}
    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
    INCLUDE_DIRS "." "display" "display/lvgl_display" "display/lvgl_display/jpg"
                 "audio" "protocols" "boards/common"
    PRIV_REQUIRES app_update efuse esp_event esp_netif esp_pm esp_psram esp_timer
                  driver esp_lcd fatfs sdmmc json mqtt nvs_flash usb spi_flash
    WHOLE_ARCHIVE
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BOARD_TYPE="jiuchuan-s3" BOARD_NAME="jiuchuan-s3"
    BUILTIN_TEXT_FONT=font_puhui_basic_20_4 BUILTIN_ICON_FONT=font_awesome_20_4
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=sdmmc_read_sectors" "-Wl,--wrap=sdmmc_write_sectors"
)

idf_build_get_property(PYTHON PYTHON)
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")
add_custom_command(OUTPUT "${LANG_HEADER}"
    COMMAND "${PYTHON}" "${PROJECT_DIR}/scripts/gen_lang.py" --language zh-CN --output "${LANG_HEADER}"
    DEPENDS "${PROJECT_DIR}/scripts/gen_lang.py" "${LANG_DIR}/language.json"
    COMMENT "Generating Chinese language configuration"
    VERBATIM
)
add_custom_target(lang_header DEPENDS "${LANG_HEADER}")
add_dependencies(${COMPONENT_LIB} lang_header)

idf_component_get_property(ESP_SR_PATH espressif__esp-sr COMPONENT_DIR)
idf_component_get_property(FONTS_PATH 78__xiaozhi-fonts COMPONENT_DIR)
set(ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
add_custom_command(OUTPUT "${ASSETS_BIN}"
    COMMAND "${PYTHON}" "${PROJECT_DIR}/scripts/build_default_assets.py"
            --sdkconfig "${SDKCONFIG}" --output "${ASSETS_BIN}"
            --builtin_text_font font_puhui_basic_20_4 --emoji_collection twemoji_64
            --esp_sr_model_path "${ESP_SR_PATH}/model" --xiaozhi_fonts_path "${FONTS_PATH}"
    DEPENDS "${SDKCONFIG}" "${PROJECT_DIR}/scripts/build_default_assets.py"
    COMMENT "Building wake-word and display assets"
    VERBATIM
)
add_custom_target(generated_default_assets ALL DEPENDS "${ASSETS_BIN}")
add_dependencies(flash generated_default_assets)
esptool_py_flash_to_partition(flash "assets" "${ASSETS_BIN}")
