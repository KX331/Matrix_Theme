#include "lvgl.h"
#include "esp_log.h"
#include "bsp_display.h"
#include "gui_main.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"   // esp_timer_get_time() 微秒级硬件时钟
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
#define CANVAS_WIDTH  80
#define CANVAS_HEIGHT 107
#define CANVAS_BUFFER_SIZE (CANVAS_WIDTH * CANVAS_HEIGHT * 2)  // 80*107*2=17120B ≈17KB，原来38KB，省出21KB

// ---------------------------------------------------------
// ★ 关机/重启后时间保持：利用 ESP32-C3 的 RTC 定时器在睡眠/重启间保持
// 原理：
//   1) 系统时钟由 LwIP/SNTP 同步后，把当前 unix 时间戳 + esp_timer 微秒数
//      （即"基准对")写入 NVS；
//   2) 下次开机（只要 RTC 不断电，包括普通热重启、深度睡眠唤醒）：
//      - 先从 NVS 读回基准对；
//      - 用"基准 unix 时间 + (当前 esp_timer 微秒 - 基准微秒) / 1e6"推算当前时间；
//      - 调用 settimeofday() 直接把系统时间设回去，这样 time() 一开机就有正确值；
//   3) NTP 同步成功后会更新 set 时间，届时再把新基准写回 NVS；
//   4) 完全断电（拔电池/掉电）后 RTC 会停，此时仍会用最后一次保存的时间起步，
//      但会因掉电时长未知而有偏差——这是无外部 RTC 硬件的 ESP32 能做到的最好程度。
// ---------------------------------------------------------
#define TIME_NVS_NAMESPACE "matrix_time"
#define TIME_NVS_KEY_BASE_USEC  "base_us"    // int64: esp_timer 基准微秒
#define TIME_NVS_KEY_BASE_UNIX  "base_unix"  // int64: 对应的 unix 秒

// 记住最后一次写入 NVS 的时间，避免同步期间频繁写 Flash（1分钟写一次即可）
static int64_t s_last_save_time_us = 0;

// 保存当前时间基准到 NVS（成功同步/定期保存时调用）
static void persist_time_base(void) {
    int64_t now_us = esp_timer_get_time();
    time_t now_unix = time(NULL);
    if (now_unix < 1700000000) return; // 时间还没同步好（2023-11 以前），不保存垃圾

    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, TIME_NVS_KEY_BASE_USEC,  now_us);
    nvs_set_i64(h, TIME_NVS_KEY_BASE_UNIX,  (int64_t)now_unix);
    nvs_commit(h);
    nvs_close(h);
    s_last_save_time_us = now_us;
}

// 从 NVS 恢复时间基准，开机后在 SNTP 启动前调用，使 time() 立即有意义
void restore_time_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    int64_t base_us = 0, base_unix = 0;
    bool ok1 = (nvs_get_i64(h, TIME_NVS_KEY_BASE_USEC,  &base_us) == ESP_OK);
    bool ok2 = (nvs_get_i64(h, TIME_NVS_KEY_BASE_UNIX,  &base_unix) == ESP_OK);
    nvs_close(h);
    if (!ok1 || !ok2 || base_unix < 1700000000) return;

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_sec = (now_us - base_us) / 1000000;
    time_t recovered = (time_t)(base_unix + elapsed_sec);
    if (recovered < 1700000000) return;

    struct timeval tv = { .tv_sec = recovered, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_last_save_time_us = now_us;
    ESP_LOGI("MATRIX", "时间已从NVS恢复: %s (距上次保存约 %lld 秒)",
             ctime(&recovered), (long long)elapsed_sec);
}

// NTP 同步成功回调 / GUI 每分钟心跳里调用：保证时间被持久化
void time_sync_notify_persist(void) {
    // 距离上次保存超过 60 秒才写一次，避免频繁擦写 NVS（Flash 擦写寿命约 10w 次）
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_save_time_us > 60 * 1000000LL) {
        persist_time_base();
    }
}

static uint8_t *canvas_buf = NULL;
static lv_obj_t *matrix_canvas = NULL;
static lv_obj_t *time_label = NULL;
static lv_obj_t *date_label = NULL;
static lv_obj_t *week_label = NULL;
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
        drops[i].depth = 2; // 近景（前景）—— 有快有慢，增加层次感
        uint8_t r = (uint8_t)(rand() % 10);
        if (r < 4) {
            drops[i].speed = (rand() % 2) + 1;     // 慢速 1~2 px/帧
        } else if (r < 7) {
            drops[i].speed = (rand() % 3) + 3;     // 中速 3~5 px/帧
        } else {
            drops[i].speed = (rand() % 4) + 6;     // 快速 6~9 px/帧
        }
        if (drops[i].speed >= 6) {
            drops[i].length = (rand() % 8) + 12;
        } else if (drops[i].speed >= 3) {
            drops[i].length = (rand() % 12) + 18;
        } else {
            drops[i].length = (rand() % 20) + 28;
        }
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

    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

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

            for (int j = 0; j < drops[i].length; j++) {
                int16_t curr_y = drops[i].y - j;

                if (drops[i].x >= 0 && drops[i].x < CANVAS_WIDTH && curr_y >= 0 && curr_y < CANVAS_HEIGHT) {

                    uint8_t alpha = 255 - (j * (240 / drops[i].length));
                    lv_color_t rain_color;

                    if (layer == 2) {
                        if (j == 0) {
                            rain_color = lv_color_make(180, 255, 180);
                        } else {
                            rain_color = lv_color_make(0, alpha, (alpha / 3));
                        }
                    } else if (layer == 1) {
                        rain_color = lv_color_make(0, (alpha * 3) / 4, 0);
                    } else {
                        rain_color = lv_color_make(0, alpha / 3, 0);
                    }

                    lv_canvas_set_px(matrix_canvas, drops[i].x, curr_y, rain_color, LV_OPA_COVER);

                    if (layer == 2 && (drops[i].x + 1) < CANVAS_WIDTH) {
                        lv_canvas_set_px(matrix_canvas, drops[i].x + 1, curr_y, rain_color, LV_OPA_COVER);
                    }
                }
            }

            drops[i].y += drops[i].speed;

            if (drops[i].y - drops[i].length > CANVAS_HEIGHT) {
                reset_drop(i);
            }
        }
    }

    lv_canvas_set_buffer(matrix_canvas, canvas_buf, CANVAS_WIDTH, CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(matrix_canvas);

    update_time_display();

    // 每约 1 分钟（2000 帧 × 30ms）保存一次时间基准到 NVS
    {
        static uint32_t tick = 0;
        tick++;
        if (tick >= 2000) {
            tick = 0;
            time_sync_notify_persist();
        }
    }
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
    lv_obj_set_style_transform_zoom(matrix_canvas, 768, 0); // 放大 3 倍铺满 240x320 屏幕（80*3=240, 107*3=321）

    // 4. 创建上层的日期、时间与星期文本 Label（叠加在背景画布之上）
    // ★ 去掉所有 drop_shadow 和 shadow 外框效果，保持字体干净锐利；
    //   之前的 shadow_width=8 + shadow_spread=2 会在文字周围渲染一圈绿色光晕外框，
    //   字体粗细由 montserrat 字体本身决定，不再通过阴影描边模拟。

    // --- 1) 时间 Label (32号，纯荧光绿，无外框) ---
    time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(time_label, lv_color_make(0, 255, 64), 0);
    lv_obj_set_style_text_opa(time_label, LV_OPA_COVER, 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -30);

    // --- 2) 日期 Label (20号，纯暗绿，无外框) ---
    date_label = lv_label_create(scr);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(date_label, lv_color_make(0, 220, 60), 0);
    lv_obj_set_style_text_opa(date_label, LV_OPA_COVER, 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 15);

    // --- 3) 星期几 Label (20号，纯暗绿，无外框) ---
    week_label = lv_label_create(scr);
    lv_obj_set_style_text_font(week_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(week_label, lv_color_make(0, 220, 60), 0);
    lv_obj_set_style_text_opa(week_label, LV_OPA_COVER, 0);
    lv_obj_align(week_label, LV_ALIGN_CENTER, 0, 45);

    // 5. 初始化所有雨滴
    for (int i = 0; i < MAX_RAIN_DROPS; i++) {
        reset_drop(i);
    }

    // 6. 启动 30ms 走帧与刷新定时器
    ui_timer = lv_timer_create(ui_periodic_update, 30, NULL);

    // 第一次立即更新时间显示（避免启动时显示空白）
    update_time_display();

    ESP_LOGI("MATRIX", "Matrix Clock GUI initialized (no-outline font, NVS time keep)");
}

// ---------------------------------------------------------
// 实体按键控制：短按 OK 键（电源键）切换屏幕亮/灭
// 电路说明：三键共用 GPIO0/ADC1_CH0 分压
// ---------------------------------------------------------
static bool s_backlight_on = true;

void screen_backlight_set(bool on) {
    s_backlight_on = on;
    bsp_display_backlight(on ? 100 : 0);
    ESP_LOGI("BTN", "背光 %s", on ? "亮" : "熄");
}
bool screen_backlight_is_on(void) { return s_backlight_on; }
void screen_backlight_toggle(void) { screen_backlight_set(!s_backlight_on); }

static void key_poll_task(void *arg) {
    (void)arg;
    adc_oneshot_unit_handle_t adc = (adc_oneshot_unit_handle_t)get_battery_adc_handle();
    if (!adc) { vTaskDelete(NULL); return; }

    const int SAMPLE_MS = 10;
    const int DEBOUNCE_MS = 30;
    const int SHORT_MAX_MS = 800;
    int pressed_ms = 0;
    bool pressed = false;
    int last_mv = 3300;

    while (1) {
        int raw = 0;
        if (adc_oneshot_read(adc, ADC_CHANNEL_0, &raw) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
            continue;
        }
        int mv = raw * 3100 / 4095;
        last_mv = mv;
        bool ok_pressed = (mv >= 400 && mv <= 2000);

        if (ok_pressed && !pressed) {
            pressed = true;
            pressed_ms = 0;
        } else if (ok_pressed && pressed) {
            pressed_ms += SAMPLE_MS;
            if (pressed_ms > 10000) pressed_ms = 10000;
        } else if (!ok_pressed && pressed) {
            if (pressed_ms >= DEBOUNCE_MS && pressed_ms <= SHORT_MAX_MS) {
                ESP_LOGI("BTN", "OK 键短按 %dms，切换背光", pressed_ms);
                screen_backlight_toggle();
            }
            pressed = false;
            pressed_ms = 0;
        }

        (void)last_mv;
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

// 启动按键轮询任务
void gui_key_task_start(void) {
    xTaskCreate(key_poll_task, "key_poll", 2048, NULL, 4, NULL);
    ESP_LOGI("BTN", "按键轮询任务已启动");
}
