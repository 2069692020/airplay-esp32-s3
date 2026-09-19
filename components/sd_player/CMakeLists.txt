idf_component_register(
    SRCS "sd_player.c" "tags.c"
    INCLUDE_DIRS "include"
    REQUIRES sd_card
    PRIV_REQUIRES esp_audio_codec esp_timer
)
