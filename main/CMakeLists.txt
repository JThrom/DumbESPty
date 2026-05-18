set(srcs
    "console_base.cpp"
    "waveshare_display.cpp"
    "ch422g_init.cpp"
    "coex_manager.cpp"
    "ble_hid_host.cpp"
    "shell.cpp"
    "wifi_mgr.cpp"
    "ssh_client.cpp"
    "terminal.cpp"
    "fonts/cozette_bdf_13.c"
    "fonts/lv_font_term_mono_10.c"
    "fonts/lv_font_nerd_symbols_10.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "include"
    REQUIRES lvgl nvs_flash esp_lcd bt esp_psram esp_hid espressif__esp_lcd_touch esp_wifi esp_netif esp_event esp_coex esp_app_format skuodi__libssh2_esp
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
