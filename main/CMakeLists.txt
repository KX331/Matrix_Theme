# 🚀 终极修正：补全 nvs_flash、esp_wifi 等系统基础组件依赖映射
idf_component_register(SRCS "main.c"
                            "connections.c"
                            "gui_main.c"
                            "lv_font_pixel_hacker_16.c"
                            "lv_font_pixel_hacker_20.c"
                            "lv_font_pixel_hacker_24.c"
                            "lv_font_pixel_hacker_26.c"
                            "lv_font_pixel_hacker_28.c"
                            "lv_font_pixel_hacker_30.c"
                            "lv_font_pixel_hacker_32.c"
                            "lv_font_pixel_hacker_34.c"
                       INCLUDE_DIRS "."
                       REQUIRES bsp lvgl json nvs_flash esp_wifi esp_http_server) 
