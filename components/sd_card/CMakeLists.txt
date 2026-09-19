idf_component_register(
    SRCS "sd_card.c" "filename_enc.c" "gbk_table.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES fatfs esp_driver_sdspi esp_driver_spi esp_driver_gpio sdmmc esp_timer
)
