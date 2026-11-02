idf_component_register(
    SRCS
        "main.c"
        "can_link.c"
        "isotp.c"
        "decode.c"
        "obd_poll.c"
        "runtime.c"
        "odo.c"
        "uplink.c"
        "status_led.c"
        "app_power.c"
        "version.c"
    INCLUDE_DIRS "."
    REQUIRES
        shared
        driver          # 旧版 TWAI 驱动（driver/twai.h），5.4+ 由 meta 组件转发
        esp_timer
        nvs_flash
        esp_wifi        # 含 ESP-NOW
)
