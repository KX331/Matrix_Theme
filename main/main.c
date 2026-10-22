#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// 引入官方原厂板级驱动支持头文件
#include "bsp_display.h"
#include "lvgl.h"

// 声明在 gui_main.c 中编写的 UI 初始化入口
extern void init_passport_main_gui(void);

// 标准的系统主入口 app_main，彻底消灭链接器未定义报错
void app_main(void) {
    // 1. 初始化最基本的持久化闪存分区
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 调用 FoloToy 官方原厂板级显示核心驱动
    // 自动配置液晶屏控制器芯片的所有内部供电外设（LDO）以及通信总线复位
    bsp_display_init(); 
    vTaskDelay(pdMS_TO_TICKS(100)); // 稳压延时：给真机屏幕电容留出 100ms 硬件响应时间
    
    // 3. 调用官方标准的背光调节函数，将 PWM 亮度拉满（100% 通电）
    bsp_display_backlight(100); 

    // 4. 初始化官方图形适配图层（内部会自动创建维持走帧和重绘的后台高优先级 RTOS 任务）
    bsp_lvgl_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    // 5. 安全加载你的 UI 界面
   init_passport_main_gui();

    // 6. 主任务安全释放 CPU 控制权，允许后台专门的图形任务满速刷屏
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}