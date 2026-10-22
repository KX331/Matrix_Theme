#include "lvgl.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#define SCREEN_WIDTH  240  
#define SCREEN_HEIGHT 320

// 总雨滴数量：20 个
#define MAX_RAIN_DROPS 20  

// 画布尺寸：120x160（内存占用约 38KB，安全不爆内存）
#define CANVAS_WIDTH  120  
#define CANVAS_HEIGHT 160
#define CANVAS_BUFFER_SIZE (CANVAS_WIDTH * CANVAS_HEIGHT * 2)

static uint8_t *canvas_buf = NULL;
static lv_obj_t *matrix_canvas = NULL;
static lv_obj_t *time_label = NULL;
static lv_obj_t *date_label = NULL;
static lv_obj_t *week_label = NULL; // 🚀 新增：星期显示 Label
static lv_timer_t *ui_timer = NULL;

// 雨滴结构体
typedef struct {
    int16_t x;          // 列X坐标
    int16_t y;          // 下落头部y坐标
    uint8_t speed;      // 下落速度
    uint16_t length;    // 尾巴长度
    uint8_t depth;      // 深度层级：0 - 远景(多且快), 1 - 中景(不变), 2 - 近景(少且慢)
} MatrixDrop;

static MatrixDrop drops[MAX_RAIN_DROPS];

// 重置雨滴层级与速度属性
static void reset_drop(int i) {
    if (i < 10) {
        drops[i].depth = 0; // 远景（背景）
        drops[i].speed = (rand() % 3) + 7;   // 速度快：7 ~ 9 px/帧
        drops[i].length = (rand() % 10) + 10;
    } else if (i < 16) {
        drops[i].depth = 1; // 中景
        drops[i].speed = (rand() % 3) + 4;   // 速度中：4 ~ 6 px/帧
        drops[i].length = (rand() % 15) + 20;
    } else {
        drops[i].depth = 2; // 近景（前景）
        drops[i].speed = (rand() % 2) + 1;   // 速度慢：1 ~ 2 px/帧
        drops[i].length = (rand() % 20) + 30;
    }

    drops[i].y = -(rand() % CANVAS_HEIGHT);
    drops[i].x = (rand() % (CANVAS_WIDTH - 6)) + 3;
}

// 更新系统当前日期、时间与星期文字
static void update_time_display(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char time_str[32];
    char date_str[32];
    char week_str[32];

// 格式化时间与日期
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

    // 🚀 定义英文大写星期简写映射表 (tm_wday: 0=周日, 1=周一, ..., 6=周六)
    const char *week_days_en[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    if (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) {
        snprintf(week_str, sizeof(week_str), "%s", week_days_en[timeinfo.tm_wday]);
    } else {
        snprintf(week_str, sizeof(week_str), "UNK");
    }

    if (time_label) {
        lv_label_set_text(time_label, time_str);
    }
    if (date_label) {
        lv_label_set_text(date_label, date_str);
    }
    if (week_label) {
        lv_label_set_text(week_label, week_str);
    }
}

// 每 30ms 触发一次画面更新与重绘
static void ui_periodic_update(lv_timer_t *timer) {
    if (!canvas_buf || !matrix_canvas) return;

    // 1. 抹黑画布内存 (纯黑背景)
    memset(canvas_buf, 0, CANVAS_BUFFER_SIZE);

    // 2. 依次按深度绘制雨滴（远景 -> 中景 -> 近景）
    for (int layer = 0; layer < 3; layer++) {
        for (int i = 0; i < MAX_RAIN_DROPS; i++) {
            if (drops[i].depth != layer) continue;

            // 绘制当前雨滴残影
            for (int j = 0; j < drops[i].length; j++) {
                int16_t curr_y = drops[i].y - j;

                if (drops[i].x >= 0 && drops[i].x < CANVAS_WIDTH && curr_y >= 0 && curr_y < CANVAS_HEIGHT) {
                    
                    uint8_t alpha = 255 - (j * (240 / drops[i].length));
                    lv_color_t rain_color;

                    if (layer == 2) {
                        // 近景：大颗粒亮绿/白绿
                        if (j == 0) {
                            rain_color = lv_color_make(180, 255, 180); 
                        } else {
                            rain_color = lv_color_make(0, alpha, (alpha / 3)); 
                        }
                    } else if (layer == 1) {
                        // 中景：标准经典骇客绿
                        rain_color = lv_color_make(0, (alpha * 3) / 4, 0);
                    } else {
                        // 远景：暗绿背景衬托
                        rain_color = lv_color_make(0, alpha / 3, 0);
                    }

                    // 绘制主点
                    lv_canvas_set_px(matrix_canvas, drops[i].x, curr_y, rain_color, LV_OPA_COVER);

                    // 近景额外加宽 1 像素
                    if (layer == 2 && (drops[i].x + 1) < CANVAS_WIDTH) {
                        lv_canvas_set_px(matrix_canvas, drops[i].x + 1, curr_y, rain_color, LV_OPA_COVER);
                    }
                }
            }

            // 推动雨滴向底部滑落
            drops[i].y += drops[i].speed;

            // 滚出底部后重置
            if (drops[i].y - drops[i].length > CANVAS_HEIGHT) {
                reset_drop(i);
            }
        }
    }

    // 刷新画布
    lv_canvas_set_buffer(matrix_canvas, canvas_buf, CANVAS_WIDTH, CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(matrix_canvas);

    // 动态更新实时日期、时间与星期
    update_time_display();
}

// 主 UI 初始化入口
void init_passport_main_gui(void) {
    lv_obj_t *scr = lv_scr_act();

    // 1. 抹黑全屏背景
    lv_obj_set_style_bg_color(scr, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 2. 申请约 38KB 的画布内存
    canvas_buf = malloc(CANVAS_BUFFER_SIZE);
    if (!canvas_buf) {
        ESP_LOGE("MATRIX", "Canvas malloc failed!");
        return;
    }

    // 3. 创建底层的代码雨画布对象
    matrix_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(matrix_canvas, canvas_buf, CANVAS_WIDTH, CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(matrix_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_transform_zoom(matrix_canvas, 512, 0); // 放大 2 倍铺满 240x320 屏幕

    // 4. 创建上层的日期、时间与星期文本 Label（叠加在背景画布之上）

    // --- 1) 时间 Label (加大到 32号字) ---
    time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_32, 0);       // 🚀 加大字体至 32号
    lv_obj_set_style_text_color(time_label, lv_color_make(0, 255, 64), 0); // 发光的骇客荧光绿
    lv_obj_set_style_text_opa(time_label, LV_OPA_90, 0);                     // 透明度
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -30);                      // 屏幕中央偏上

    // --- 2) 日期 Label (加大到 20号字) ---
    date_label = lv_label_create(scr);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_20, 0);       // 🚀 加大字体至 20号
    lv_obj_set_style_text_color(date_label, lv_color_make(0, 200, 50), 0);  // 稍暗绿色的日期
    lv_obj_set_style_text_opa(date_label, LV_OPA_90, 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 15);                       // 屏幕中央偏下

    // --- 3) 星期几 Label (新增，20号字) ---
    week_label = lv_label_create(scr);
    lv_obj_set_style_text_font(week_label, &lv_font_montserrat_20, 0);       // 🚀 新增星期 Label，20号字
    lv_obj_set_style_text_color(week_label, lv_color_make(0, 200, 50), 0);  // 骇客绿
    lv_obj_set_style_text_opa(week_label, LV_OPA_90, 0);
    lv_obj_align(week_label, LV_ALIGN_CENTER, 0, 45);                       // 位于日期正下方

    // 5. 初始化所有雨滴
    for (int i = 0; i < MAX_RAIN_DROPS; i++) {
        reset_drop(i);
    }

    // 6. 启动 30ms 走帧与刷新定时器
    ui_timer = lv_timer_create(ui_periodic_update, 30, NULL);
    
    ESP_LOGI("MATRIX", "Matrix Clock GUI with Depth & Weekday initialized successfully!");
}