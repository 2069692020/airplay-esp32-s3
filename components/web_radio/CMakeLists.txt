idf_component_register(
    SRCS "web_radio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client
    PRIV_REQUIRES esp_audio_codec esp_timer mbedtls
)
