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
const char* get_nfc_content(void);   // 自定义 NFC 标签内容（个人信息页设置）
int get_sleep_key(void);
const char* get_sleep_key_name(int id);  // 0→"UP键" 1→"DOWN键" 2→"OK键"（越界按 UP键）

// ---- 熄屏按键「运行时」读写接口（唯一入口，gui_main.c 自持，供按键任务读取）----
// 网页保存设置后调用 gui_key_set_sleep_key() 立即生效；按键任务每次判定都调用
// gui_key_get_sleep_key() 现取，因此改完设置无需重启/重新连接即可生效。
void gui_key_set_sleep_key(int id);  // 设置熄屏按键，id 越界自动回落 0（UP键）
int  gui_key_get_sleep_key(void);    // 读取当前熄屏按键：0=UP键 1=DOWN键 2=OK键

// ---- 功能菜单按键「运行时」读写接口（唯一入口，gui_main.c 自持，供按键任务读取）----
// 短按「功能菜单按键」一次 → 屏幕最上层弹出功能菜单；与熄屏按键互斥（网页保存时做冲突检查）。
// 默认 OK键。改完设置由网页保存流程调用 gui_key_set_menu_key() 立即生效。
void gui_key_set_menu_key(int id);   // 设置功能菜单按键，id 越界自动回落 2（OK键）
int  gui_key_get_menu_key(void);     // 读取当前功能菜单按键：0=UP键 1=DOWN键 2=OK键

// 供 Web 层查询（用于设置页回显）
int get_menu_key(void);
const char* get_menu_key_name(int id);  // 0→"UP键" 1→"DOWN键" 2→"OK键"（越界按 OK键）

// ---- 自动同步频率（每小时整点 / 每天零时）----
#define NTP_SYNC_HOURLY    0   // 每小时整点自动同步一次
#define NTP_SYNC_DAILY     1   // 每天 00:00 自动同步一次
int  get_ntp_sync_mode(void);
const char* get_ntp_sync_mode_name(int mode);   // 0→"每小时" 1→"每天零时"（越界按"每小时"）

// ---- 整点报时 ----
bool get_hour_chime_enabled(void);   // 是否开启整点报时（默认关闭）
int  get_chime_silent_start(void);   // 静默起始小时 0~23（默认 22）
int  get_chime_silent_end(void);     // 静默结束小时 0~23（默认 7，含 7 点整）
// 当前小时 hour(0..23) 是否落在静默时段内（供整点报时判定是否跳过）
bool is_hour_in_silent(int hour);

// ---- 电池（电源）查询 ----
// soc ：剩余电量 0~100（-1=读失败/未初始化）
// mv  ：电池电压 mV（-1=读失败）
// charging ：是否在充电/USB 供电（基于 CW2017 电压启发式，≥4300mV 视为 USB 供电）
int  get_battery_soc(void);
int  get_battery_mv(void);
bool get_battery_charging(void);

// ---- 整点报时内部任务（由按键任务或 main 启动一次）----
// 不直接暴露接口，由 web_config_init() 内部调用启动任务。

// ---- 功能菜单（真机屏幕）----
// 短按功能菜单按键一次 → 在最上层新开一个图层，屏幕中央显示文字选项框：
// 个人名片 / 相册 / 天气预报 / 手电筒 / 取消。
// 菜单内 UP键/DOWN键 移动选择（选中项高亮，未选中项压暗），OK键 确认；
// 【关闭菜单的唯一方式】：把选中项移到「取消」后按 OK键 确认（不再用"连按两次菜单键"）。
// 菜单已打开时，菜单键的短按不再承担"开菜单"动作，而同键若是 OK 则等同"确认"，
// 因此默认配置（熄屏键=UP、菜单键=OK）下：短按 OK 开菜单，UP/DOWN 移动，移到
// 「取消」再短按 OK 即关闭。
bool gui_menu_is_open(void);        // 菜单图层当前是否已显示
void gui_menu_open(void);           // 显示菜单图层（重复调用无副作用）
void gui_menu_close(void);          // 隐藏菜单图层（选「取消」或确认后自动调用）
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

// 读取 GPIO0（ADC1_CH0）的电压，单位 mV，带校准与滤波。
// 电池电压检测与三键（UP/DOWN/OK）电阻梯识别共用此函数。
// 返回 >=0 为实测毫伏；返回 -1 表示 ADC 未就绪或读取失败（调用方须判负，
// 不可当成 0mV——0mV 是 UP 键的有效按下电压）。
int adc_read_mv(void);

// 屏幕背光控制
//   长按「熄屏按键」（网页设置，gui_key_get_sleep_key()）→ 熄屏
//   熄屏状态下短按任意键 → 亮屏
//   亮屏状态下短按非熄屏键 → 切换亮/灭
void screen_backlight_set(bool on);
bool screen_backlight_is_on(void);
void screen_backlight_toggle(void);

// 时间接口
bool get_matrix_time_str(char *buf, size_t len);
bool get_matrix_date_str(char *buf, size_t len);
const char* get_matrix_weekday_str(void);

// 时间持久化：开机从 NVS 恢复上次保存的时间基准，让关机/重启后时间继续步进；
// NTP 同步成功或每 60 秒心跳时调用 time_sync_notify_persist() 把基准写回 NVS。
// 注意：基准里的"微秒时钟"用的是 RTC 计数器 esp_rtc_get_time_us()（跨软复位/深睡持续
// 累加），而非复位即归零的 esp_timer_get_time()——后者是上一版恢复失效的根因。
void restore_time_from_nvs(void);
void time_sync_notify_persist(void);
bool time_is_restored(void);   // 本次开机是否已从 NVS 恢复出可用时间（false=要等 NTP）
bool time_is_synced(void);     // 当前系统时间是否已被 NTP 校正到真实时间

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
