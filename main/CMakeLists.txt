set(srcs
    "console_base.cpp"
    "ch422g_init.cpp"
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

if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND srcs
        "waveshare_display_p4.cpp"
        "coex_manager_stub.cpp"
        "ble_hid_host.cpp"
        "usb_hid_host.cpp"
    )
    set(extra_requires bt esp_hid usb espressif__usb_host_hid esp_driver_ledc)
else()
    list(APPEND srcs
        "waveshare_display.cpp"
        "coex_manager.cpp"
        "ble_hid_host.cpp"
        "usb_hid_host.cpp"
    )
    set(extra_requires bt esp_hid)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "include"
    REQUIRES lvgl nvs_flash esp_lcd esp_psram espressif__esp_lcd_touch esp_wifi esp_netif esp_event esp_coex esp_app_format skuodi__libssh2_esp mbedtls microlink ${extra_requires}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)

if(TARGET __idf_wireguard_lwip)
    target_compile_options(__idf_wireguard_lwip PRIVATE -Wno-error=unterminated-string-initialization -Wno-error=stringop-overread)
endif()

if(TARGET __idf_microlink)
    target_compile_options(__idf_microlink PRIVATE -Wno-error=stringop-truncation -Wno-error=unused-but-set-variable)
endif()
