# Matrix_Theme Web管理系统CMake配置
idf_component_register(SRCS "main.c"
                            "gui_main.c"
                            "gui_main_web.c"
                            "danmaku.c"
                            "lv_font_pixel_hacker_16.c"
                            "lv_font_pixel_hacker_20.c"
                            "lv_font_pixel_hacker_24.c"
                       INCLUDE_DIRS "."
                       REQUIRES bsp lvgl nvs_flash esp_wifi esp_http_server spiffs esp_adc lwip esp-tls esp_timer)
