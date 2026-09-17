set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

idf_component_register(
    SRCS
        "main.c"
        "bsp_display.c"
        "cover.c"
        "wifi_sta.c"
        "${REPO_ROOT}/src/UI/screen.c"
        "${REPO_ROOT}/src/UI/fonts/lv_font_pt_14.c"
        "${REPO_ROOT}/src/UI/fonts/lv_font_pt_20.c"
        "${REPO_ROOT}/src/UI/fonts/lv_font_jp_14.c"
        "${REPO_ROOT}/src/UI/fonts/lv_font_jp_20.c"
        "${REPO_ROOT}/src/spotify/spotify.c"
    INCLUDE_DIRS
        "."
        "${REPO_ROOT}/src"
    REQUIRES
        esp_lcd
        esp_wifi
        esp_http_client
        esp-tls
        nvs_flash
        driver
        esp_timer
        esp_driver_ledc
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE SCREEN_CLIP_CIRCLE=0 LV_LVGL_H_INCLUDE_SIMPLE)
