# every board's driver is in here, each file compiles to nothing unless board.h picks it
idf_component_register(SRCS "main.c" "net.c" "store.c" "console.c" "button.c" "touch.c"
                            "lcd_spi.c" "lcd_rgb.c" "lcd_dsi.c" "lcd_ili9486.c" "epd.c" "i2c_bus.c" "tiles.c" "led.c"
                       INCLUDE_DIRS "."
                       REQUIRES core esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_rmt
                                esp_driver_uart esp_wifi esp_event esp_netif nvs_flash esp_timer
                                esp_http_client mbedtls esp_partition)
