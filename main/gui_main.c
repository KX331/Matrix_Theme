#include "lvgl.h"
#include "esp_log.h"
#include "bsp_display.h"
#include "gui_main.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"   // esp_timer_get_time(): 复位即归零，仅用于短时计时，不可作跨重启时间基准
// RTC 计数器读取：ESP-IDF 5.5 起旧 rtc.h 已弃用，统一入口为 esp_rtc_time.h 的
// uint64_t esp_rtc_get_time_us(void)，返回 RTC 计数器当前微秒值，跨软复位/深睡持续累加，
// 仅上电复位（掉电）才清零。这正是"跨重启推算时间"所需的那把尺子。
#include "esp_rtc_time.h"
#include "esp_attr.h"    // RTC_NOINIT_ATTR：把变量放在 RTC 保留内存，掉电才丢
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

// ★ 功能菜单汉字字体（24px）声明
//   本工程自带三套 pixel_hacker 位图字体，经离线解析各文件的 cmaps[] 表逐字核对：
//     · lv_font_pixel_hacker_16 / _20：cmap 仅覆盖 ASCII(32~126) 与 160~255 拉丁区，
//       【不含任何 CJK 汉字】，用它渲染菜单汉字只会得到空白/缺字；
//     · lv_font_pixel_hacker_24：覆盖 GB2312 常用汉字区间，已逐字确认包含菜单所需的
//       "个人名片相册天气预报手电筒取消" 全部 15 个汉字（等宽点阵，adv_w=384/16=24px）。
//   因此功能菜单必须使用 24px 字体。该字体定义在 lv_font_pixel_hacker_24.c
//   （已列入 main/CMakeLists.txt 的 SRCS），此处必须显式声明才能引用，否则编译报
//   "lv_font_pixel_hacker_16 undeclared" 一类错误。
LV_FONT_DECLARE(lv_font_pixel_hacker_24);
// 16px 字体保留声明备用（ASCII 范围）；右上角电量现已改用 24px，与功能菜单字号一致。
LV_FONT_DECLARE(lv_font_pixel_hacker_16);

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320

// 总雨滴数量：20 个
#define MAX_RAIN_DROPS 20

// 画布尺寸：120x160（内存占用约 38KB，安全不爆内存）
#define CANVAS_WIDTH  80
#define CANVAS_HEIGHT 107
#define CANVAS_BUFFER_SIZE (CANVAS_WIDTH * CANVAS_HEIGHT * 2)  // 80*107*2=17120B ≈17KB，原来38KB，省出21KB

// ---------------------------------------------------------
// ★ 关机/重启后时间保持（重写版）
//
// 【上一版为什么失效】
//   旧实现把「当前 unix 时间 + esp_timer_get_time() 的微秒数」当成"基准对"存进 NVS，
//   下次开机用「基准 unix + (本次 esp_timer 值 - 基准 esp_timer 值)/1e6」推算时间。
//   这个算法成立的前提是 esp_timer_get_time() 跨重启可比 —— 但前提不成立：
//     - ESP32-C3 的 esp_timer 走 SYSTIMER（见 sdkconfig: CONFIG_ESP_TIMER_IMPL_SYSTIMER=y），
//       官方文档明确：它返回的是"自 esp_timer 初始化以来的时间"，而 esp_timer 在 app_main
//       之前初始化，因此【每次复位后都从 0 重新开始计数】；
//     - 于是本次开机读出的 esp_timer 值（比如 0.2 秒）远小于上次保存时的值（比如 300 秒），
//       相减得到负数，推算出的 recovered 时间戳直接落到 1970 年附近，
//       被 `recovered < 1700000000` 这道闸门拦下 —— 恢复彻底失败。
//   结论：必须换一个【跨复位持续累加】的时钟源，而不是 esp_timer。
//
// 【本版做法】
//   1) 时钟源改用 RTC 计数器 esp_rtc_get_time_us()：
//      官方文档说明 RTC timer "可跨任意复位保持（仅上电复位会清零 RTC 计数器）"，
//      深睡/软复位/看门狗复位后其计数值【继续累加而不归零】。
//      用它作"基准对"里那个可跨重启比较的微秒数，推算才成立。
//   2) 额外记录"上电复位次数 boot_cnt"（每次真上电 RTC 域清空时自增）：
//      用来识别"这次是不是掉过电"。掉电后 RTC 计数器归零，跨重启的推算关系断裂，
//      此时不再盲目把 base_unix 直接当当前时间（那会明显偏慢，正是用户看到的
//      "刚启动时间与显示时间不符"），而是以 base_unix 起步并标注"时钟可能滞后"，
//      交由 SNTP / 自动补同步任务尽快校正。
//   3) 保存时机不变：SNTP 同步成功、以及 GUI 每 60 秒心跳时各写一次（限频保护 Flash）。
// ---------------------------------------------------------
#define TIME_NVS_NAMESPACE "matrix_time"
#define TIME_NVS_KEY_BASE_USEC  "base_us"    // int64: RTC 计数器基准（微秒，跨复位可比）
#define TIME_NVS_KEY_BASE_UNIX  "base_unix"  // int64: 对应的 unix 秒
#define TIME_NVS_KEY_BOOT_CNT   "boot_cnt"   // int32: 上电复位次数（掉电识别）

// 放在 RTC 保留内存里的上电计数：软复位/深睡唤醒后仍保留，真掉电才丢失。
// 它不是持久存储，只用于"本次是否掉过电"的瞬时判断。
RTC_NOINIT_ATTR static uint32_t s_rtc_boot_cnt_magic;
RTC_NOINIT_ATTR static uint32_t s_rtc_boot_cnt;
#define RTC_BOOT_MAGIC 0x4D54584Bu   /* 'MTXK'，用于识别 RTC 保留区是否被上电清空 */

// 记住最后一次写入 NVS 的时间（用 RTC 计数器计），避免同步期间频繁写 Flash
static int64_t s_last_save_us = 0;
static bool    s_time_restored = false;   // 本次开机是否已成功恢复出可用时间

// 取"跨复位可持续比较"的微秒计数。RTC 计数器即为此用途；esp_timer 只在 RTC 不可用时兜底。
static inline int64_t clock_us(void) {
    return (int64_t)esp_rtc_get_time_us();
}

// 读取/推进上电计数。返回本次是否为"上电复位"（RTC 保留区被清空 => 掉过电）
static bool rtc_boot_count_step(void) {
    if (s_rtc_boot_cnt_magic != RTC_BOOT_MAGIC) {
        // 魔数不匹配：RTC 保留区被清空，说明这次是真正的上电（掉电后重新上电）
        s_rtc_boot_cnt_magic = RTC_BOOT_MAGIC;
        s_rtc_boot_cnt = 1;
        return true;
    }
    // 魔数仍在：软复位 / 看门狗复位 / 深睡唤醒，RTC 计数器继续累加
    s_rtc_boot_cnt++;
    return false;
}

// 保存当前时间基准到 NVS（成功同步/定期保存时调用）
static void persist_time_base(void) {
    int64_t now_us = clock_us();
    time_t now_unix = time(NULL);
    if (now_unix < 1700000000) return; // 时间还没同步好（2023-11 以前），不保存垃圾

    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, TIME_NVS_KEY_BASE_USEC,  now_us);
    nvs_set_i64(h, TIME_NVS_KEY_BASE_UNIX,  (int64_t)now_unix);
    nvs_set_i32(h, TIME_NVS_KEY_BOOT_CNT,   (int32_t)s_rtc_boot_cnt);
    nvs_commit(h);
    nvs_close(h);
    s_last_save_us = now_us;
}

// 从 NVS 恢复时间基准，开机后在 SNTP 启动前调用，使 time() 立即有意义
void restore_time_from_nvs(void) {
    bool power_on_reset = rtc_boot_count_step();

    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW("MATRIX", "无时间基准记录，等待 NTP 同步");
        return;
    }
    int64_t base_us = 0, base_unix = 0;
    int32_t saved_boot = 0;
    bool ok1 = (nvs_get_i64(h, TIME_NVS_KEY_BASE_USEC,  &base_us) == ESP_OK);
    bool ok2 = (nvs_get_i64(h, TIME_NVS_KEY_BASE_UNIX,  &base_unix) == ESP_OK);
    (void)nvs_get_i32(h, TIME_NVS_KEY_BOOT_CNT, &saved_boot);
    nvs_close(h);
    if (!ok1 || !ok2 || base_unix < 1700000000) {
        ESP_LOGW("MATRIX", "时间基准无效，等待 NTP 同步");
        return;
    }

    int64_t now_us = clock_us();
    time_t recovered;
    int64_t elapsed_sec;

    if (power_on_reset) {
        // 掉过电：RTC 计数器已从 0 重新开始，不能再用它与 base_us 相减推算时长。
        // 直接以"上次保存的时间"起步，并明确告知用户这段时间是断的。用户在界面上
        // 会看到时间从上次关机时刻接着走，随后由自动补同步任务尽快校正到真实时间。
        elapsed_sec = 0;
        recovered = (time_t)base_unix;
        ESP_LOGW("MATRIX", "检测到上电复位（曾断电），时间以基准 %s 起步，等待 NTP 校正",
                 ctime(&recovered));
    } else {
        // 软复位/看门狗复位/深睡唤醒：RTC 计数器持续累加，可精确推算关机期间经过的真实时长
        elapsed_sec = (now_us - base_us) / 1000000;
        if (elapsed_sec < 0) {
            // RTC 计数值反而变小（异常：RTC 域被复位但魔数侥幸保留），按掉电处理
            elapsed_sec = 0;
            ESP_LOGW("MATRIX", "RTC 计数异常回退，按掉电处理，等待 NTP 校正");
        }
        recovered = (time_t)(base_unix + elapsed_sec);
        ESP_LOGI("MATRIX", "时间已从NVS恢复: %s (距上次保存约 %lld 秒)",
                 ctime(&recovered), (long long)elapsed_sec);
    }

    if (recovered < 1700000000) {
        ESP_LOGW("MATRIX", "恢复出的时间无效，等待 NTP 同步");
        return;
    }

    struct timeval tv = { .tv_sec = recovered, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_last_save_us = now_us;
    s_time_restored = true;
}

// 供外部（网页"时间"卡片 / 串口）查询：本次开机是否已恢复出可用时间
bool time_is_restored(void) { return s_time_restored; }

// NTP 同步成功回调 / GUI 每分钟心跳里调用：保证时间被持久化
void time_sync_notify_persist(void) {
    // 距离上次保存超过 60 秒才写一次，避免频繁擦写 NVS（Flash 擦写寿命约 10w 次）
    int64_t now_us = clock_us();
    if (s_last_save_us == 0 || (now_us - s_last_save_us) > 60 * 1000000LL) {
        persist_time_base();
    }
}

static uint8_t *canvas_buf = NULL;
static lv_obj_t *matrix_canvas = NULL;
static lv_obj_t *time_label = NULL;
static lv_obj_t *date_label = NULL;
static lv_obj_t *week_label = NULL;
static lv_timer_t *ui_timer = NULL;
// ★ 主屏幕右上角电池电量 label（需求：电量【跟随功能菜单的出现而显示】，
//   位置固定在主屏幕右上角而不是菜单面板内，字号与功能菜单条目一致）。
//   建在 lv_layer_top() 上，随菜单打开而创建、随菜单关闭而销毁：
//   菜单不出现时，屏幕右上角不出现任何电量文字。
static lv_obj_t *battery_label = NULL;

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

    char time_str[40];
    char date_str[40];
    char week_str[40];

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

// 前向声明：主屏幕右上角电量刷新（定义在文件后半部）
static void battery_label_refresh_locked(void);

// 每 30ms 触发一次画面更新与重绘
static void ui_periodic_update(lv_timer_t *timer) {
    (void)timer;
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

    // 主屏幕右上角电量：只在功能菜单显示期间存在（label 由菜单打开时创建、
    // 关闭时销毁）。菜单未打开时本函数立即返回，屏幕右上角不显示电量。
    battery_label_refresh_locked();

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

    // --- 4) 右上角电池电量 Label（不在此处创建）---
    //   需求：电量【跟随功能菜单的出现而显示】，菜单不显示时右上角不出现电量文字。
    //   因此不再开机常驻创建，改为在 gui_menu_open_locked() 里创建、
    //   在 gui_menu_close_locked() 里销毁；位置固定在【主屏幕】右上角，
    //   字号与功能菜单条目一致（lv_font_pixel_hacker_24，等宽点阵，
    //   内容 "B: 87%"/"C: 99%"/"?: 45%" 全为 ASCII，不会出现缺字方块）。

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

// =========================================================
// ★ 功能菜单图层（真机屏幕最上层的文字功能选项框）
// =========================================================
// 需求要点（用户 2026-10 提出）：
//   3) 触发「功能菜单按键」后，在屏幕显示图层的最上方出现一个新的图层，
//      是一个文字功能选项框，不需要全屏，位于屏幕中心，只占功能菜单文字区域大小；
//   4) 菜单内容固定五项：个人名片 / 相册 / 天气预报 / 手电筒 / 取消；
//      前四项为待开发功能，"取消"关闭菜单图层；
//   5) 选中的菜单项需要高亮显示。
//
// 实现要点：
//   - 用"全屏半透明遮罩 + 居中小面板"两个对象构成菜单图层：
//       menu_mask ：铺满全屏的黑色 60% 不透明遮罩，把背后的代码雨/时间压暗，
//                   保证"菜单在最上方"的视觉分层清晰；
//       menu_panel：屏幕居中的小面板（深色底 + 亮绿边框），宽度固定 170px、
//                   高度由内容撑开，只包住 5 行文字，刻意【不铺满屏幕】。
//   - 两个对象都建在 lv_layer_top() 上：这是 LVGL 的最顶层图层，永远盖在所有普通
//     对象（画布、时间/日期/星期标签）之上，无需在 ui_periodic_update 里反复前置。
//   - 5 个条目是 5 个 label。高亮做法：选中项用亮绿 (0,255,80) + 满不透明 + 行首游标
//     "> "（纯 ASCII，任何字体都有字形；"▶" 不在自带字体 cmap 内，故不用），
//     未选中项压暗成暗绿 (0,110,0) + 70% 不透明，形成明显前后景对比。
//     不额外加边框/底色，避免小屏上文字糊成一片。
//   - 汉字字形：使用本工程自带的 lv_font_pixel_hacker_24。经离线逐字核对该字体 cmaps[]
//     与 glyph_dsc[]，"个人名片相册天气预报手电筒取消" 全部 15 字均有非空字形；
//     而 16px / 20px 字体只有 ASCII，不能用于显示汉字（这正是上一版编译报
//     "lv_font_pixel_hacker_16 undeclared" 之外、即便声明了也会显示不出汉字的根因）。
//   - 交互（统一在 key_poll_task 里处理，见下方按键任务）：
//       短按【一次】「功能菜单按键」→ 唤出功能菜单；
//       菜单打开时：UP/DOWN 移动选中项（循环），OK 确认；
//       确认「取消」→ 关闭菜单图层；确认前四项 → 关闭菜单并进入对应功能（当前为待开发）。
//       菜单已打开时再短按一次「功能菜单按键」→ 不做任何事（不再作为关闭方式）。
//   - 【关闭菜单的唯一方式】把选中项移到「取消」后按 OK 键确认（2026-10-24 修订）。
//     此前版本用"连按两次菜单键"收起菜单，用户反馈不符合直觉，已取消该路径；
//     菜单键短按改为"开菜单"的单一动作，关闭一律走「取消」。
//   - 菜单打开期间：除"长按熄屏键熄屏"外，原有短按行为全部让位给菜单操作，
//     避免"想选菜单却把屏幕关掉"的误操作。
// =========================================================
#define MENU_ITEM_COUNT 5
static const char *k_menu_items[MENU_ITEM_COUNT] = {
    "个人名片",   // 0 待开发
    "相册",       // 1 待开发
    "天气预报",   // 2 待开发
    "手电筒",     // 3 待开发
    "取消"        // 4 关闭菜单图层
};

static lv_obj_t *menu_mask  = NULL;                     // 顶层半透明遮罩
static lv_obj_t *menu_panel = NULL;                     // 居中选项面板
static lv_obj_t *menu_labels[MENU_ITEM_COUNT] = {0};    // 5 个文字条目
static int       menu_sel    = 0;                       // 当前高亮项下标
static bool      menu_opened = false;                   // 菜单图层是否已显示

// =========================================================
// ★ 线程安全：按键任务里所有 LVGL 调用必须持锁
// =========================================================
// BSP 注释原文："LVGL 非线程安全:在【非 LVGL 任务】里操作任何 lv_* 对象前后
// 必须加解锁。" key_poll_task 是独立 FreeRTOS 任务，而 LVGL 有自己的渲染/刷新任务，
// 如果在按键任务里直接 lv_obj_del / lv_obj_create / lv_label_set_text，可能正好撞上
// LVGL 渲染任务在 lv_refr.c 里遍历 obj->spec_attr->children[]，于是：
//   1) lv_obj_del(menu_panel) 在按键任务里释放父对象及其 spec_attr；
//   2) LVGL 渲染任务正在 obj->spec_attr->children[i] 取子对象；
//   3) 读到野指针 → Load access fault（串口 backtrace 就是 lv_refr.c:181）；
//   4) CPU 异常复位 → 重启期间 LCD 未刷新，屏幕显示全白几秒后才恢复。
// 这正是"选「取消」按 OK 后白屏几秒"的直接根因。
//
// 修复：按键任务里所有接触 lv_* 的入口，都用 bsp_lvgl_lock / bsp_lvgl_unlock 包住；
// 拿不到锁时直接放弃本次操作（下一次按键再试），绝不跨持有锁做 vTaskDelay。
// 高亮刷新、开菜单、关菜单、移动选中项、确认这几个内部函数不再自己加锁，
// 由【对外的】入口统一加锁，避免嵌套。
// =========================================================
#define MENU_LVGL_LOCK_TIMEOUT_MS  200     // 等锁超时：200ms 拿不到就放弃本次操作
static inline bool menu_lock(void)    { return bsp_lvgl_lock(MENU_LVGL_LOCK_TIMEOUT_MS); }
static inline void menu_unlock(void)  { bsp_lvgl_unlock(); }

// 选中项配色（亮绿、满不透明）与未选中项配色（暗绿、半透明）
#define MENU_SEL_COLOR    lv_color_make(0, 255, 80)
#define MENU_NOSEL_COLOR  lv_color_make(0, 110, 0)

// 立即刷新一次右上角电量文字（调用方必须已持 LVGL 锁）。
// 菜单打开、创建 label 后立刻调一次，避免菜单刚弹出时先显示占位文字、
// 再等下一秒的节流刷新才出真实数据。
static void battery_label_update_locked(void) {
    if (!battery_label) return;
    int soc = get_battery_soc();
    int mv  = get_battery_mv();
    bool batt_ready = (mv >= 3000);
    bool chg = batt_ready && get_battery_charging();
    char bat_buf[24];
    // "B:" = 电池供电；"C:" = USB 充电；"?" = 未就绪；
    // ASCII 字符在 lv_font_pixel_hacker_24 下都有字形（range 32~126），不会出方块
    if (!batt_ready) {
        snprintf(bat_buf, sizeof(bat_buf), "?: %d%%", soc);
    } else if (chg) {
        snprintf(bat_buf, sizeof(bat_buf), "C: %d%%", soc);
    } else {
        snprintf(bat_buf, sizeof(bat_buf), "B: %d%%", soc);
    }
    lv_label_set_text(battery_label, bat_buf);
}

// 主屏幕右上角电量：1Hz 节流刷新；必须在 LVGL 锁持有状态下调用
// （ui_periodic_update 是 lv_timer 回调，天然持锁；此处供其调用）。
// 菜单未打开时 battery_label 为 NULL，函数立即返回，屏幕右上角不显示任何电量文字。
static void battery_label_refresh_locked(void) {
    if (!battery_label) return;
    static uint32_t bat_tick = 0;
    bat_tick++;
    if (bat_tick < 33) return;      // 33 × 30ms ≈ 1s
    bat_tick = 0;
    battery_label_update_locked();
}

// ------- 以下为"持锁版"内部实现：调用方必须已拿到 LVGL 锁 -------
// 注释后缀带 _locked 的函数表示"调用方必须已通过 bsp_lvgl_lock 拿到锁"。
// 这些内部函数彼此之间可以互相调用而不再加锁，避免递归死锁。

static void menu_refresh_highlight_locked(void) {
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        if (!menu_labels[i]) continue;
        if (i == menu_sel) {
            char buf[32];
            snprintf(buf, sizeof(buf), "> %s", k_menu_items[i]);
            lv_label_set_text(menu_labels[i], buf);
            lv_obj_set_style_text_color(menu_labels[i], MENU_SEL_COLOR, 0);
            lv_obj_set_style_text_opa(menu_labels[i], LV_OPA_COVER, 0);
        } else {
            lv_label_set_text(menu_labels[i], k_menu_items[i]);
            lv_obj_set_style_text_color(menu_labels[i], MENU_NOSEL_COLOR, 0);
            lv_obj_set_style_text_opa(menu_labels[i], LV_OPA_70, 0);
        }
    }
}

// 显示菜单图层（持锁版，幂等）
static void gui_menu_open_locked(void) {
    if (menu_opened) { menu_refresh_highlight_locked(); return; }

    lv_obj_t *top = lv_layer_top();          // LVGL 最顶层，永远盖住画布与时间标签
    lv_obj_set_scrollbar_mode(top, LV_SCROLLBAR_MODE_OFF);

    // 1) 全屏遮罩：把背后的代码雨/时间压暗，突出菜单
    menu_mask = lv_obj_create(top);
    lv_obj_set_size(menu_mask, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(menu_mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(menu_mask, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(menu_mask, LV_OPA_60, 0);
    lv_obj_set_style_border_width(menu_mask, 0, 0);
    lv_obj_set_style_radius(menu_mask, 0, 0);
    lv_obj_set_style_pad_all(menu_mask, 0, 0);
    lv_obj_set_scrollbar_mode(menu_mask, LV_SCROLLBAR_MODE_OFF);

    // 2) 居中面板：宽度固定 170px（"天气预报"4 字 × 24px = 96px，再加 "> " 游标与左右
    //    内边距 8+8=16px 共约 124px，170px 留足余量且不至于铺满 240px 屏宽），
    //    高度 LV_SIZE_CONTENT 由内容撑开 —— 只包住文字区域，不铺满屏幕。
    menu_panel = lv_obj_create(top);
    lv_obj_set_size(menu_panel, 170, LV_SIZE_CONTENT);
    lv_obj_align(menu_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(menu_panel, lv_color_make(0, 12, 0), 0);
    lv_obj_set_style_bg_opa(menu_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(menu_panel, lv_color_make(0, 255, 80), 0);
    lv_obj_set_style_border_width(menu_panel, 2, 0);
    lv_obj_set_style_radius(menu_panel, 6, 0);
    lv_obj_set_style_pad_all(menu_panel, 8, 0);
    lv_obj_set_style_pad_row(menu_panel, 8, 0);       // 行间距（24px 字体行距相应放大）
    lv_obj_set_flex_flow(menu_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(menu_panel, LV_SCROLLBAR_MODE_OFF);

    // 2.5) 主屏幕右上角电量：随菜单的出现而显示。
    //   本对象不属于 menu_panel，而是直接建在 lv_layer_top() 上、排在遮罩与面板之后创建，
    //   因此天然位于菜单图层最上方，不会被遮罩压暗。
    //   位置固定在【主屏幕】右上角（不是菜单面板的右上角）；字号与菜单条目一致。
    //   菜单关闭时由 gui_menu_close_locked() 销毁 —— 菜单不显示时右上角无电量文字。
    if (!battery_label) {
        battery_label = lv_label_create(top);
        lv_obj_set_style_text_font(battery_label, &lv_font_pixel_hacker_24, 0);
        lv_obj_set_style_text_color(battery_label, lv_color_make(0, 255, 80), 0);
        lv_obj_set_style_text_opa(battery_label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(battery_label, 0, 0);
        lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -4, 2);
        battery_label_update_locked();     // 立刻出数，不等下一秒的节流刷新
    }

    // 3) 5 个文字条目
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        menu_labels[i] = lv_label_create(menu_panel);
        lv_label_set_text(menu_labels[i], k_menu_items[i]);
        lv_obj_set_style_text_font(menu_labels[i], &lv_font_pixel_hacker_24, 0);
        lv_obj_set_style_text_color(menu_labels[i], MENU_NOSEL_COLOR, 0);
    }

    menu_sel = 0;                  // 每次打开都从第一项"个人名片"开始
    menu_refresh_highlight_locked();
    menu_opened = true;
    ESP_LOGI("MENU", "功能菜单已显示（5 项，UP/DOWN 选择，OK 确认）");
}

// 隐藏菜单图层（持锁版，幂等；必须持锁才能调 lv_obj_del 避免与渲染任务并发）
static void gui_menu_close_locked(void) {
    if (!menu_opened && !menu_panel && !menu_mask) return;
    // 关键修复：删除顺序必须"先删父（panel），再删遮罩（mask）"。
    // lv_obj_del(panel) 会一并释放其 children（5 个 label），所以我们之后只需要
    // 把 menu_labels[] 清空、避免后续误用；顺序错了不会直接崩，但会让 LVGL
    // 的 children 数组在渲染期被 free，再次触发 load access fault。
    if (menu_panel) { lv_obj_del(menu_panel); menu_panel = NULL; }
    if (menu_mask)  { lv_obj_del(menu_mask);  menu_mask  = NULL; }
    // 右上角电量随菜单一起销毁：菜单不显示时，屏幕右上角不出现电量文字。
    // （注意：battery_label 建在 lv_layer_top() 上、不属于 menu_panel，
    //   所以必须在这里显式删除，不能依赖父对象 menu_panel 的回收。）
    if (battery_label) { lv_obj_del(battery_label); battery_label = NULL; }
    for (int i = 0; i < MENU_ITEM_COUNT; i++) menu_labels[i] = NULL;
    menu_opened = false;
    menu_sel = 0;
    ESP_LOGI("MENU", "功能菜单已关闭");
}

// ------- 以下是"对外 API 版"：每次进入都先拿锁，拿不到就放弃本次操作 -------
// 这些函数既可能被按键任务调用，也可能被熄屏长按等其他路径调用。

bool gui_menu_is_open(void) {
    // 纯布尔读，无需锁；volatile 即可保证可见性。
    return menu_opened;
}

void gui_menu_open(void) {
    if (menu_opened) return;                         // 幂等：已打开直接走（避免无谓等锁）
    if (!menu_lock()) {
        ESP_LOGW("MENU", "开菜单时 LVGL 锁被占用，本次操作放弃");
        return;
    }
    gui_menu_open_locked();
    menu_unlock();
}

void gui_menu_close(void) {
    if (!menu_opened && !menu_panel && !menu_mask) return;
    if (!menu_lock()) {
        ESP_LOGW("MENU", "关菜单时 LVGL 锁被占用，本次操作放弃");
        return;
    }
    gui_menu_close_locked();
    menu_unlock();
}

// 按键任务专用的"带锁交互入口"：一次持锁连续完成"移动 / 确认"，避免
// "UP 键按一下 → 拿锁 → 移动 → 释放 → 刷新 → 再按一下又拿锁"造成的撕裂。
// 这三个静态包装是给 key_poll_task 里直接调用的，对外不暴露。
static void menu_move_safe(int dir) {
    if (!gui_menu_is_open()) return;
    if (!menu_lock()) { ESP_LOGW("MENU", "移动选中项时 LVGL 锁被占用，本次操作放弃"); return; }
    menu_sel = (menu_sel + dir + MENU_ITEM_COUNT) % MENU_ITEM_COUNT;
    menu_refresh_highlight_locked();
    ESP_LOGI("MENU", "当前选中：%s", k_menu_items[menu_sel]);
    menu_unlock();
}

static int menu_confirm_safe(void) {
    if (!gui_menu_is_open()) return -1;
    if (!menu_lock()) { ESP_LOGW("MENU", "确认菜单时 LVGL 锁被占用，本次操作放弃"); return -1; }
    int idx = menu_sel;
    gui_menu_close_locked();
    menu_unlock();
    if (idx == MENU_ITEM_COUNT - 1) {
        ESP_LOGI("MENU", "已选择「取消」，关闭菜单");
        return idx;
    }
    ESP_LOGW("MENU", "进入功能「%s」：该功能待开发，暂无对应页面", k_menu_items[idx]);
    return idx;
}

// ---------------------------------------------------------
// 实体按键控制
// 电路：三键共用 GPIO0/ADC1_CH0，经不同电阻分压；未按下时接近 3.3V。
// 真机现场标定（2026-10-23，串口 "BTN: GPIO0=xxx mV" 实测得出）：
//   - 空闲（未按下）：≈ 2967 mV
//   - 长按 UP  键  ：≈    0 mV（按键直接接地）
//   - 长按 DOWN键  ：≈  290 ~ 300 mV
//   - 长按 OK  键  ：≈  400 ~ 600 mV
// 三个键按下后都落在互不重叠的低压段，于是：
//   - "是否按下" ：以 "电压 < KEY_PRESS_MV_MAX" 判定（空闲≈2967mV，留有很大余量）；
//   - "按的哪个键"：再按下述三段区间区分。
// 若后续更换硬件或分压电阻，只需重新测量并按实测值调整下面这几个常量。
// 操作逻辑（2026-10-24 修订）：
//   - 长按「熄屏按键」（网页设置页可配置：UP键 / DOWN键 / OK键，默认 UP键）→ 熄屏
//   - 熄屏状态下按任意键（不论长短）→ 亮屏
//   - 「功能菜单按键」（网页设置页可配置，默认 OK 键）短按【一次】→ 唤出功能菜单
//   - 功能菜单的【关闭方式】：在菜单里把选中项移到「取消」，再按 OK 键确认
//     （菜单打开时再短按一次菜单键不再有任何动作）
//   - 亮屏、菜单未打开、且不是菜单键的短按 → 不做任何事
//     （原"亮屏下短按非熄屏键 → 切换背光"已移除：现场日志显示该规则会让用户短按
//       DOWN/OK 键时屏幕被意外关掉，是"短按 DOWN/OK 键会熄屏"的直接根因）
// ---------------------------------------------------------
#define KEY_PRESS_MV_MAX  1200   // 按下判定阈值：电压 < 此值视为有键按下（空闲实测≈2967mV）
#define KEY_UP_MV_MIN        0   // UP   键：0   ~ 145  mV（实测 ≈0）
#define KEY_UP_MV_MAX      145
#define KEY_DOWN_MV_MIN    145   // DOWN 键：145 ~ 350  mV（实测 ≈290~300）
#define KEY_DOWN_MV_MAX    350
#define KEY_OK_MV_MIN      350   // OK   键：350 ~ 1200 mV（实测 ≈400~600）
#define KEY_OK_MV_MAX     1200

#define KEY_ID_UP   0
#define KEY_ID_DOWN 1
#define KEY_ID_OK   2

// ---------------------------------------------------------
// 熄屏按键「运行时」副本（本文件内自持，不依赖 Web 模块的 static 变量）
// ---------------------------------------------------------
// 设计原因：
//   熄屏按键是"随时可能被网页改掉、且按键任务每秒检查几十次"的运行参数。
//   原实现把 sleep_key 放在 gui_main_web.c 里，靠 gui_main_web.c 提供
//   get_sleep_key() 供本文件调用，形成 "gui_main.c ← gui_main_web.c" 的链接期
//   依赖：一旦该模块的 static 变量因任何原因没有同步更新，或两处取值来源不一致，
//   按键任务就会一直拿旧值（表现为"网页里改成 DOWN/OK 键保存了，真机仍按 UP 键"）。
//   这里改为本文件自持一份，并提供取值/存值的唯一入口：
//     - gui_key_get_sleep_key() 只给按键任务读，保证读取路径最短、不被缓存；
//     - gui_key_set_sleep_key() 由网页保存流程调用，越界自动回落 UP 键。
//   启动时由 Web 模块从 NVS 读回后调用 set 同步进来，重启后仍然生效。
// ---------------------------------------------------------
static volatile int s_sleep_key = KEY_ID_UP;   // 0=UP键 1=DOWN键 2=OK键，默认 UP键

static bool s_backlight_on = true;

void screen_backlight_set(bool on) {
    s_backlight_on = on;
    bsp_display_backlight(on ? 100 : 0);
    ESP_LOGI("BTN", "背光 %s", on ? "亮" : "熄");
}
bool screen_backlight_is_on(void) { return s_backlight_on; }
void screen_backlight_toggle(void) { screen_backlight_set(!s_backlight_on); }

// 由 ADC 电压判定当前按下的是哪个键，无按键返回 -1
static int key_id_from_mv(int mv) {
    if (mv >= KEY_UP_MV_MIN   && mv < KEY_UP_MV_MAX)   return KEY_ID_UP;
    if (mv >= KEY_DOWN_MV_MIN && mv < KEY_DOWN_MV_MAX) return KEY_ID_DOWN;
    if (mv >= KEY_OK_MV_MIN   && mv < KEY_OK_MV_MAX)   return KEY_ID_OK;
    return -1;
}

static const char* key_id_name(int id) {
    switch (id) {
        case KEY_ID_UP:   return "UP键";
        case KEY_ID_DOWN: return "DOWN键";
        case KEY_ID_OK:   return "OK键";
        default:          return "无";
    }
}

// ---------------------------------------------------------
// 熄屏按键「运行时」副本的读写入口（放在 key_id_name 之后，便于打日志）
// ---------------------------------------------------------
// 设计原因：
//   熄屏按键是"随时可能被网页改掉、且按键任务每秒检查几十次"的运行参数。
//   原实现把 sleep_key 放在 gui_main_web.c 里，靠 gui_main_web.c 提供
//   get_sleep_key() 供本文件调用，取值要跨模块绕一圈；一旦该模块的 static
//   变量没有同步更新，按键任务就会一直拿旧值（表现为"网页里改成 DOWN/OK 键
//   保存了，真机仍只认 UP 键"）。
//   这里改为本文件自持一份，并提供唯一读写入口：
//     - gui_key_get_sleep_key() 给按键任务读，每次判定都现取，改完立即生效；
//     - gui_key_set_sleep_key() 由网页保存流程 / 启动读 NVS 后调用，越界回落 UP 键。
// ---------------------------------------------------------
void gui_key_set_sleep_key(int id) {
    if (id < KEY_ID_UP || id > KEY_ID_OK) id = KEY_ID_UP;   // 越界一律回落默认"UP键"
    s_sleep_key = id;
    ESP_LOGI("BTN", "熄屏按键已更新为 %s", key_id_name(id));
}

int gui_key_get_sleep_key(void) {
    int id = s_sleep_key;                                   // 单次读取，避免中途被改写
    if (id < KEY_ID_UP || id > KEY_ID_OK) id = KEY_ID_UP;
    return id;
}

// ---------------------------------------------------------
// 功能菜单按键「运行时」副本（结构同熄屏按键，默认 OK键）
// ---------------------------------------------------------
static volatile int s_menu_key = KEY_ID_OK;   // 0=UP键 1=DOWN键 2=OK键，默认 OK键

void gui_key_set_menu_key(int id) {
    if (id < KEY_ID_UP || id > KEY_ID_OK) id = KEY_ID_OK;   // 越界一律回落默认"OK键"
    // 防御：若调用方仍把两键设成同一个，则让步给熄屏按键（改回 OK/UP），
    //       保证"一个按键只挂一个功能"。网页端已在保存时做冲突拦截并提示用户。
    if (id == gui_key_get_sleep_key()) {
        ESP_LOGW("BTN", "功能菜单按键与熄屏按键冲突（同为 %s），已自动改为 %s",
                 key_id_name(id), key_id_name(id == KEY_ID_OK ? KEY_ID_UP : KEY_ID_OK));
        id = (id == KEY_ID_OK) ? KEY_ID_UP : KEY_ID_OK;
    }
    s_menu_key = id;
    ESP_LOGI("BTN", "功能菜单按键已更新为 %s", key_id_name(id));
}

int gui_key_get_menu_key(void) {
    int id = s_menu_key;                                    // 单次读取，避免中途被改写
    if (id < KEY_ID_UP || id > KEY_ID_OK) id = KEY_ID_OK;
    return id;
}

static void key_poll_task(void *arg) {
    (void)arg;
    if (!get_battery_adc_handle()) { vTaskDelete(NULL); return; }

    const int SAMPLE_MS = 10;          // 采样周期
    const int CONFIRM_CNT = 2;         // 连续 N 次识别一致才确认按键（滤除按下瞬间的过渡电压）
    const int DEBOUNCE_MS = 30;        // 去抖：短于此时长视为抖动
    const int LONG_PRESS_MS = 1000;    // 长按判定阈值（仅"熄屏按键"还在用长按）
    const int MENU_TOGGLE_GUARD_MS = 400;  // 菜单键"开菜单"后的静默期：防止一次按住被反复触发
    const int IDLE_LOG_PERIOD = 10000; // 空闲时每 10 秒打印一次电压
    const int KEY_LOG_PERIOD = 300;    // 有按键按下时每 300ms 打印一次（便于现场标定）

    int  pressed_ms = 0;
    int  pressed_key = -1;             // 当前已确认按住的键（-1=无）
    bool pressed_was_lit = false;      // 本次按下【之前】背光是否亮着（区分"熄屏唤醒"与"长按熄屏"）
    bool menu_toggled = false;         // 本次按住期间是否已经"开过菜单"（防止重复触发）
    int  menu_guard_ms = 0;            // "开菜单"之后的静默计时（用于配合上面的防重触发）
    int  cand_key = -1;                // 待确认的键
    int  cand_cnt = 0;                 // 待确认键的连续命中次数
    int  log_timer = 0;

    while (1) {
        int mv = adc_read_mv();
        if (mv < 0) mv = KEY_PRESS_MV_MAX;   // ADC 读取异常时按"未按下"处理，避免误触发
        int key = key_id_from_mv(mv);
        bool pressed_now = (mv < KEY_PRESS_MV_MAX);

        // 连续 N 次识别到同一个键，才认为该键"稳定按下"。
        // 目的：按下瞬间电压从 ~2967mV 跌到目标值的过程中可能出现 1 帧的中间值
        //（例如跌向 OK 键途中经过 DOWN 键区间），单帧不确认即可免疫。
        if (key >= 0) {
            if (key == cand_key) { if (cand_cnt < CONFIRM_CNT) cand_cnt++; }
            else                 { cand_key = key; cand_cnt = 1; }
        } else {
            cand_key = -1;
            cand_cnt = 0;
        }
        int stable_key = (cand_cnt >= CONFIRM_CNT) ? cand_key : -1;

        // 日志策略：
        // - 空闲：每 10 秒打一行，确认 ADC 活着、基线正常
        // - 按下：每 300ms 打一行，方便从串口核对三键各自的实际 mV
        log_timer += SAMPLE_MS;
        if ((pressed_now && log_timer >= KEY_LOG_PERIOD) ||
            (!pressed_now && log_timer >= IDLE_LOG_PERIOD)) {
            log_timer = 0;
            ESP_LOGI("BTN", "GPIO0=%d mV，识别=%s（实测：空闲≈2967 / UP≈0 / DOWN≈300 / OK≈500 mV）",
                     mv, key_id_name(stable_key));
        } else if (pressed_now && key < 0) {
            log_timer = 0;
            ESP_LOGW("BTN", "GPIO0=%d mV 处于按下区但未命中任何键区间，请调整 KEY_*_MV_* 阈值", mv);
        }

        // ---- 菜单键"开菜单"静默期计时 ----
        // 语义：菜单键短按一次唤出菜单后，给它一个 400ms 的静默期，避免"一次按住"被
        //       反复当作"松手→短按"而把菜单开了又开（表现为闪烁）。
        // 细节：本文件此处的"松手"判定用的是【瞬时电压】而非"连续 N 帧确认"，所以一次
        //       正常松手只会在极少几帧里满足条件；但为稳妥，仍用"必须已经松手 + 静默期满"
        //       两个条件一起清掉标记，宁可多等 400ms 也不让菜单重复打开。
        if (menu_toggled && !pressed_now) {
            menu_guard_ms += SAMPLE_MS;
            if (menu_guard_ms >= MENU_TOGGLE_GUARD_MS) {
                menu_toggled = false;
                menu_guard_ms = 0;
            }
        }

        if (!pressed_now) {
            // ---- 松手（瞬时电压回到高位）：以瞬时值判定，保证响应速度 ----
            if (pressed_key >= 0) {
                bool is_long  = (pressed_ms >= LONG_PRESS_MS);
                bool is_short = (pressed_ms >= DEBOUNCE_MS && !is_long);
                int  menu_key = gui_key_get_menu_key();

                if (!pressed_was_lit) {
                    // 按下【之前】背光就是熄的 → 本次按压是"唤醒"，松手即亮屏（不论长短按）。
                    // 这里刻意用"按下之前"而不是"松手时"的背光状态，用于区分两种情况：
                    //   ① 熄屏状态下按键          → 要亮屏；
                    //   ② 长按熄屏键刚把屏幕关掉后松手 → 不能又立刻点亮（否则熄屏功能形同失效）。
                    ESP_LOGI("BTN", "%s 唤醒屏幕", key_id_name(pressed_key));
                    screen_backlight_set(true);
                } else if (is_short && pressed_key == menu_key && !gui_menu_is_open()) {
                    // ===== 功能菜单键：短按【一次】唤出功能菜单 =====
                    // 这里加了 !gui_menu_is_open() 的前提：菜单键在"菜单已打开"时不再做开菜单
                    // 的动作，而是继续往下走，由后面那条"OK 键 → menu_confirm()"分支接管，
                    // 这样"把选中项移到「取消」再按 OK 键"就能真正关闭菜单（即使菜单键就是 OK 键）。
                    if (!menu_toggled) {
                        ESP_LOGI("BTN", "短按 %s，唤出功能菜单", key_id_name(pressed_key));
                        gui_menu_open();
                        menu_toggled = true;   // 进入静默期，避免同一次按住被反复触发
                        menu_guard_ms = 0;
                    }
                } else if (is_short && gui_menu_is_open()) {
                    // ===== 菜单已打开：UP/DOWN 移动选中项，OK 确认（即时响应） =====
                    // 关闭菜单的唯一路径就在这里：选中最后一项「取消」后按 OK，menu_confirm()
                    // 内部判断到是「取消」即关闭图层；选前四项则进入对应功能（当前为待开发）。
                    // 【线程安全】这里统一走 _safe 包装，由它们在内部拿锁/放锁，避免按键
                    // 任务直接操作 LVGL 对象导致 use-after-free（现场已验证会 panic）。
                    if (pressed_key == KEY_ID_UP) {
                        menu_move_safe(-1);
                    } else if (pressed_key == KEY_ID_DOWN) {
                        menu_move_safe(+1);
                    } else if (pressed_key == KEY_ID_OK) {
                        menu_confirm_safe();
                    }
                } else if (is_short) {
                    // 亮屏 + 菜单未打开 + 非菜单键的短按：不做任何事。
                    // 【本次修复】原实现此处是"切换背光"，现场日志证实它会导致用户短按
                    // DOWN/OK 键时屏幕被意外关掉：
                    //     "短按 DOWN键 130ms，切换背光 / 背光 熄"
                    // 现在：熄屏只由"长按熄屏键"触发，唤醒只由"熄屏状态下按键"触发。
                }
                pressed_key = -1;
                pressed_ms = 0;
                pressed_was_lit = false;
            }
        } else if (stable_key >= 0) {
            if (pressed_key < 0) {
                // 首次稳定按下：锁定按键，开始计时，并记录"按下之前"的背光状态
                pressed_key = stable_key;
                pressed_ms = 0;
                pressed_was_lit = screen_backlight_is_on();
            } else if (pressed_key == stable_key) {
                // 持续按住：累计时长，检查是否达到"长按"动作
                pressed_ms += SAMPLE_MS;
                if (pressed_ms > 30000) pressed_ms = 30000;

                if (pressed_ms >= LONG_PRESS_MS && screen_backlight_is_on()) {
                    if (pressed_key == gui_key_get_sleep_key()) {
                        // 长按「熄屏按键」→ 熄屏
                        // （顺带收起可能打开着的菜单：黑屏后菜单图层没有意义，且会残留状态）
                        ESP_LOGI("BTN", "长按 %s 熄屏（当前熄屏键=%s）",
                                 key_id_name(pressed_key), key_id_name(gui_key_get_sleep_key()));
                        if (gui_menu_is_open()) gui_menu_close();
                        screen_backlight_set(false);
                        menu_toggled = false;
                        menu_guard_ms = 0;
                        pressed_ms = LONG_PRESS_MS;   // 避免松手前重复触发
                    } else if (pressed_key == gui_key_get_menu_key()) {
                        // 「功能菜单按键」的动作是"短按一次开菜单"，长按不再有任何动作。
                        // 这里只把静默标记清掉，避免长按结束、松手后又被当成一次"开菜单"。
                        menu_toggled = false;
                        menu_guard_ms = 0;
                        pressed_ms = LONG_PRESS_MS;   // 避免重复执行
                    }
                }
            } else {
                // 稳定识别结果与已按住的键不同：视为抖动，放弃本次
                pressed_key = -1;
                pressed_ms = 0;
            }
        } else if (pressed_key >= 0) {
            // 已按下但当前帧尚未确认（过渡帧）：保持状态并继续计时，避免长按被误打断
            pressed_ms += SAMPLE_MS;
            if (pressed_ms > 30000) pressed_ms = 30000;
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

// 启动按键轮询任务
// 栈从 2048 提升到 3072：菜单交互路径里会调用 LVGL（创建/删除对象、刷 label 文本），
// LVGL 内部与 snprintf 在 2048 字节栈上余量偏紧，抬高一档更稳妥。
void gui_key_task_start(void) {
    xTaskCreate(key_poll_task, "key_poll", 3072, NULL, 4, NULL);
    ESP_LOGI("BTN", "按键轮询任务已启动（含功能菜单交互）");
}
