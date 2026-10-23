#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// 引入官方原厂板级驱动支持头文件
#include "bsp_display.h"
#include "lvgl.h"
#include "gui_main.h"

static const char *TAG = "MAIN";

// 延迟启动 GUI 的任务：等 WiFi + Web 服务器真正启动、内存稳定后再 malloc 画布
static void gui_delayed_start_task(void *arg)
{
    (void)arg;

    // 先等至少 2.5 秒让 WiFi 关联、SPIFFS 挂载、Web 重试任务跑起来
    // 同时每隔 500ms 看一下空闲内存，到 28KB 以上就可以启动 GUI（画布已缩到 ~17KB）
    int waited_ms = 0;
    while (waited_ms < 10000) {
        size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "等待内存回收（已等 %d ms，空闲 %d KB）...", waited_ms, (int)(free_now / 1024));
        if (free_now >= 28 * 1024 && waited_ms >= 2500) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        waited_ms += 500;
    }

    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "开始初始化 GUI（空闲 %d KB）...", (int)(before / 1024));
    init_passport_main_gui();
    gui_key_task_start();
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "GUI 初始化完成，剩余空闲 %d KB（画布占用约 %d KB）",
             (int)(after / 1024), (int)((before - after) / 1024));

    vTaskDelete(NULL);
}

// 标准的系统主入口 app_main，彻底消灭链接器未定义报错
void app_main(void) {
    // 1. 初始化最基本的持久化闪存分区（web_config_init内部也会做，这里先做基础初始化）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 调用 FoloToy 官方原厂板级显示核心驱动（几乎不占堆内存）
    bsp_display_init();
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. 初始化显示背光
    bsp_display_backlight(100);

    // 4. 先只初始化 LVGL 本身（draw buffer 约 9.6KB，还不分配数字雨画布）
    bsp_lvgl_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    // 5. ★ 关键：先启动 WiFi + Web 服务器（含后台重试任务）+ 文件系统 + NTP。
    //   这样 httpd 启动时还没有那 38KB 的数字雨画布，内存不会被挤爆。
    //   启动后设备会开放热点 PassportXXXXXX（X为本机MAC后三位），连接后访问 http://192.168.4.1
    ESP_LOGI(TAG, "开机初始空闲内存: %d KB",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    web_config_init();
    ESP_LOGI(TAG, "Web/WiFi 启动完成，空闲内存 %d KB，稍后再初始化 GUI",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    // 6. GUI 画布（数字雨 ~17KB，已从原 38KB 缩小 80x107 配合 3x 放大铺满屏）延后启动：
    //    另起任务，等 WiFi 缓冲释放、SPIFFS 初始化完、内存回到 28KB 以上再分配。
    //    优先级设低（1 = tskIDLE_PRIORITY+1），避免抢占 Web/WiFi 任务。
    xTaskCreate(gui_delayed_start_task, "gui_dly", 4096, NULL, 1, NULL);

    ESP_LOGI(TAG, "系统正在启动，热点 PassportXXXXXX 开放后请访问 http://192.168.4.1");

    // 7. 主任务循环
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "系统运行中，空闲内存 %d KB",
                 (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
}
