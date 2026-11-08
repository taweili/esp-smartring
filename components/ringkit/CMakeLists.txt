# ringkit — generic Smart Ring Plus skeleton.
#
# Wraps the whole viewesmart/smartring_plus BSP so applications never touch
# bsp_* directly. smartring_plus is a PUBLIC requirement: it already exports
# lvgl, esp_lcd_touch, esp_driver_i2c, esp_driver_i2s, espressif__esp_codec_dev
# and sdmmc, all of which appear in ringkit's public headers and therefore
# propagate to dependents.
#
# Dependency split: a component stays in REQUIRES only when a ringkit public
# header exposes one of its types. Everything else is PRIV_REQUIRES so
# dependents do not inherit include paths they never use. The four voice/BT
# modules are additionally gated by CONFIG_RINGKIT_* (see Kconfig); the
# shared leaves (ring_ca/json/http/...) stay compiled so any combination of
# the gates links.
set(RINGKIT_SRCS
    "src/ring_board.c"
    "src/ring_display.c"
    "src/ring_touch.c"
    "src/ring_battery.c"
    "src/ring_power.c"
    "src/ring_audio.c"
    "src/ring_imu.c"
    "src/ring_sd.c"
    "src/ring_wifi.c"
    "src/ring_ui.c"
    "src/ring_rand.c"
    "src/ring_secret.c"
    "src/ring_ca.c"
    "src/ring_time.c"
    "src/ring_nvs.c"
    "src/ring_json.c"
    "src/ring_byte_ring.c"
    "src/ring_resample.c"
    "src/ring_resample_coeffs.c"
    "src/ring_http.c"
)

if(CONFIG_RINGKIT_NET)
    list(APPEND RINGKIT_SRCS
        "src/ring_net.c"
        "src/ring_net_signaling.c"
        "src/ring_net_codec.c"
        "src/ring_net_protocol.c")
endif()

if(CONFIG_RINGKIT_WS)
    list(APPEND RINGKIT_SRCS
        "src/ring_ws.c"
        "src/ring_ws_transport.c"
        "src/ring_ws_protocol.c")
endif()

if(CONFIG_RINGKIT_CHAT)
    list(APPEND RINGKIT_SRCS
        "src/ring_chat.c"
        "src/ring_stt.c"
        "src/ring_llm.c"
        "src/ring_tts.c")
endif()

if(CONFIG_RINGKIT_BT)
    list(APPEND RINGKIT_SRCS
        "src/ring_bt.c"
        "src/ring_bt_gatt.c"
        "src/ring_bt_gap.c")
endif()

idf_component_register(
    SRCS ${RINGKIT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES smartring_plus lvgl esp_http_client
    PRIV_REQUIRES esp_lvgl_adapter esp_wifi esp_netif esp_event nvs_flash bt
                  esp_peer esp-tls lwip mbedtls esp_audio_codec esp_websocket_client
    EMBED_TXTFILES "assets/qianwen_ca.pem"
)
