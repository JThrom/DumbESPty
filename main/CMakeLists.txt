set(srcs
    "console_base.cpp"
    "waveshare_display.cpp"
    "ch422g_init.cpp"
    "coex_manager.cpp"
    "ble_hid_host.cpp"
    "shell.cpp"
    "ui_status_menu.cpp"
    "wifi_mgr.cpp"
    "tailscale_mgr.cpp"
    "secret_vault.cpp"
    "hostname_mgr.cpp"
    "ssh_client.cpp"
    "terminal.cpp"
    "fonts/cozette_bdf_13.c"
    "fonts/lv_font_term_mono_10.c"
    "fonts/lv_font_nerd_symbols_10.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "include"
    REQUIRES lvgl nvs_flash esp_lcd bt esp_psram esp_hid espressif__esp_lcd_touch esp_wifi esp_netif esp_event esp_coex esp_app_format skuodi__libssh2_esp mbedtls microlink
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)

if(TARGET __idf_wireguard_lwip)
    target_compile_options(__idf_wireguard_lwip PRIVATE -Wno-error=unterminated-string-initialization -Wno-error=stringop-overread)
endif()

if(TARGET __idf_microlink)
    target_compile_options(__idf_microlink PRIVATE -Wno-error=stringop-truncation -Wno-error=unused-but-set-variable)
endif()
