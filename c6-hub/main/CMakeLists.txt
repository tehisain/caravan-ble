idf_component_register(
    SRCS "main.c" "wifi_manager.c" "ota_handler.c"
         "powerqueen_bms.c" "victron_iadv.c" "power_monitor.c"
         "power_state.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bt
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_https_ota
        app_update
        spi_flash
        esp_app_format
        esp_partition
        esp_timer
        mbedtls
)
