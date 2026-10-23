#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化Web管理系统、WiFi AP/STA、文件系统、电池ADC、NTP
 */
void web_config_init(void);

// ---- 供GUI层读取配置的接口 ----
const char* get_dev_name(void);
bool is_wifi_connected(void);
int get_battery_percent(void);
int get_saver_timeout_sec(void);
int get_album_interval_sec(void);
int get_screensaver_theme(void);
bool has_custom_avatar(void);
bool has_custom_boot_music(void);
const char* get_storage_mount_point(void);
int get_storage_used_percent(int *out_total_kb);

// 电池/按键 ADC 句柄（已由 web_config_init 建好，供 GUI 侧按键轮询复用）
void* get_battery_adc_handle(void);

// 屏幕背光控制（短按 OK 键切换亮/灭）
void screen_backlight_set(bool on);
bool screen_backlight_is_on(void);
void screen_backlight_toggle(void);

// 时间接口
bool get_matrix_time_str(char *buf, size_t len);
bool get_matrix_date_str(char *buf, size_t len);
const char* get_matrix_weekday_str(void);

// 时间持久化：开机从 NVS 恢复上次保存的时间基准，让关机/重启后时间继续步进；
// NTP 同步成功或每 60 秒心跳时调用 time_sync_notify_persist() 把基准写回 NVS。
void restore_time_from_nvs(void);
void time_sync_notify_persist(void);

/**
 * @brief 初始化黑客帝国主题 GUI
 */
void init_passport_main_gui(void);
void gui_main_init(void);
void gui_update_time_date(const char * time_str, const char * date_str, const char * week_str);
void gui_key_task_start(void);

#ifdef __cplusplus
}
#endif
