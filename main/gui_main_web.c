#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_spiffs.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us()：ADC 连续采样间隔
#include "lvgl.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"   // esp_timer_get_time()
#include "esp_heap_caps.h"  // heap_caps_get_free_size()，用于启动前检查可用内存
#include <stdio.h>       // snprintf / sscanf
#include <unistd.h>      // truncate()
#include "danmaku.h"     // 抖音弹幕：WebSocket 接收模块
#include "gui_main.h"    // restore_time_from_nvs() / time_sync_notify_persist()
#include "bsp_battery.h" // CW2017 电量计（硬件 I2C，比 GPIO0 分压估算准确得多）
#include "bsp_audio.h"   // ES8311 音频 codec：整点报时合成提示音用

#define TAG "MATRIX_WEB"

// NVS命名空间与键
#define NVS_NAMESPACE "matrix_cfg"
#define NVS_KEY_NAME  "custom_name"
#define NVS_KEY_NTP   "ntp_server"
#define NVS_KEY_SAVER "screensaver_theme"
#define NVS_KEY_SAVER_TIMEOUT "saver_timeout_sec"
#define NVS_KEY_ALBUM_INTERVAL "album_interval_sec"
#define NVS_KEY_BOOT_MUSIC_VOL "boot_music_vol"
#define NVS_KEY_AVATAR_SET "avatar_set"
#define NVS_KEY_BOOT_MUSIC_SET "boot_music_set"
#define NVS_KEY_NFC_CONTENT "nfc_content"     // 自定义 NFC 标签内容（个人信息页）
#define NVS_KEY_SLEEP_KEY   "sleep_key"       // 触发熄屏的按键：0=UP键 1=DOWN键 2=OK键
#define NVS_KEY_MENU_KEY    "menu_key"        // 短按唤出功能菜单的按键：0=UP键 1=DOWN键 2=OK键
#define NVS_KEY_NTP_SYNC    "ntp_sync_mode"   // 自动同步频率：0=每小时 1=每天零时
#define NVS_KEY_CHIME_EN    "chime_en"        // 整点报时开关：1=开启 0=关闭
#define NVS_KEY_CHIME_SIL_S "chime_sil_s"     // 静默时段起始小时（0~23）
#define NVS_KEY_CHIME_SIL_E "chime_sil_e"     // 静默时段结束小时（0~23）
#define WIFI_LIST_NAMESPACE "wifi_list"

#define NFC_CONTENT_MAX     64                // NFC 标签内容最大字节数
#define SLEEP_KEY_UP        0                 // UP键
#define SLEEP_KEY_DOWN      1                 // DOWN键
#define SLEEP_KEY_OK        2                 // OK键

// 自动同步频率取值（与 gui_main.h 里的 NTP_SYNC_* 宏一一对应）
#define SYNC_MODE_HOURLY  0
#define SYNC_MODE_DAILY   1

// 整点报时默认静默时段：每晚 22 点 ~ 次日 7 点整
#define CHIME_SILENT_DEFAULT_START 22
#define CHIME_SILENT_DEFAULT_END   7

#define STORAGE_MOUNT_POINT "/storage"
#define MAX_ALBUM_IMAGES 30
#define ALBUM_MAX_W 240
#define ALBUM_MAX_H 320
#define AVATAR_MAX_W 90
#define AVATAR_MAX_H 90
#define MAX_BOOT_MUSIC_SIZE (256 * 1024) // 开机音乐最大256KB
#define HTTP_RECV_BUF_SIZE 2048
// 单个标签页 HTML 片段缓冲区。按各页"静态文本 + 最坏参数"逐字节核算：
// 最坏的是"相册"页：约 1450B 静态文本 + 相册计数 %d 各 11B = 1483B。
// 其余各页：设置页约 2900B（WiFi 列表单独占 wifi_list_html[2048]，不含在此）、
// 个人信息页约 1550B（NFC 转义值 64×6=384B 计入）、弹幕页约 1500B。
// 6144 对所有页均有充足余量；若后续继续在单页内加大段静态文本，需按上式重算。
#define HTML_CHUNK_SIZE 12288  /* v25: 从 8192 提到 12K——设置页新增 NTP 频率/整点报时/静默时段控件+wifi 列表展开后，单块 HTML 会超过 8K，snprintf 静默截断会导致后续标签 DOM 丢失、点击标签无反应 */

static char current_custom_name[32] = "Matrix Clock";
static char ntp_server_addr[64] = "ntp.ntsc.ac.cn";
static char nfc_content[NFC_CONTENT_MAX] = "";
static int sleep_key = SLEEP_KEY_UP;   // 触发熄屏的按键，默认"UP键"
static int menu_key  = SLEEP_KEY_OK;   // 短按唤出功能菜单的按键，默认"OK键"
static char saved_ssid_list[5][32] = {0}; // 最多保存5个WiFi
static int saved_wifi_count = 0;
static int current_wifi_idx = -1;
static bool wifi_connected_status = false;
static int screensaver_theme = 0; // 0=无, 1=黑客帝国数字雨
static int saver_timeout_sec = 30;
static int album_interval_sec = 10;
static bool avatar_exists = false;
static bool boot_music_exists = false;
static int battery_percent = 0;          // 由 bsp_battery_soc() 更新（不再用 GPIO0 分压估算）
static int battery_mv = 0;               // 由 bsp_battery_mv() 更新，mV（-1=未就绪）
static bool battery_charging = false;    // 电压启发式判断：≥4300mV 视为 USB 供电/充满
static bool bsp_batt_ok = false;         // CW2017 是否初始化成功
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;   // ADC 校准句柄（curve_fitting，不可用则未校准换算）
static bool adc_cali_ok = false;                   // 校准是否可用
static bool s_fs_ready = false;   // SPIFFS 是否挂载成功（供状态页显示）

// ★ 自动同步频率 + 整点报时相关配置（NVS 持久化）
//   默认值：每小时整点同步、整点报时关闭、静默 22:00~7:00
static int  ntp_sync_mode = SYNC_MODE_HOURLY;
static bool chime_enabled = false;
static int  chime_silent_start = CHIME_SILENT_DEFAULT_START;
static int  chime_silent_end   = CHIME_SILENT_DEFAULT_END;

// WiFi 切换参数：起独立任务延迟切网，避免在 httpd 任务里阻塞/切断响应
typedef struct {
    char ssid[32];
    char pass[64];
} wifi_switch_param_t;

// 文件系统统计
static void fs_init(void);
static void fs_deinit(void);
static void initialize_sntp(void);
static void sync_time_manual(void);
static void init_battery_adc(void);
static int read_battery_percent(void);
static int save_wifi_to_list(const char* ssid, const char* pass);
static int find_wifi_in_list(const char* ssid);

// ---------------------------------------------------------
// 文件系统初始化（移到独立任务中执行，避免首次格式化120秒阻塞主任务触发看门狗）
// ---------------------------------------------------------
static void fs_init_task(void *arg) {
    (void)arg;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 8,
        // 首次上电（或分区刚被擦除）时分区内没有 SPIFFS 魔数，挂载会返回
        // -10025(SPIFFS_ERR_NOT_A_FS) 并自动格式化；该过程视分区大小需
        // 10~60 秒（实测 5MB 分区约 120 秒），期间串口无输出，属正常现象，不是故障。
        .format_if_mount_failed = true
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    int64_t cost_ms = (esp_timer_get_time() - t0) / 1000;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 不可用: %s (本次耗时 %lld ms)", esp_err_to_name(ret), (long long)cost_ms);
        s_fs_ready = false;
        vTaskDelete(NULL);
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS 挂载成功: 分区 %dKB (SPIFFS 实际可用约 %dKB), 已用 %dKB, 本次耗时 %lld ms",
             (int)(total/1024), (int)((total*3/4)/1024), (int)(used/1024), (long long)cost_ms);
    s_fs_ready = true;

    // 创建必要子目录
    mkdir(STORAGE_MOUNT_POINT "/avatar", 0775);
    mkdir(STORAGE_MOUNT_POINT "/album", 0775);
    mkdir(STORAGE_MOUNT_POINT "/music", 0775);

    vTaskDelete(NULL);
}

static void fs_init(void) {
    // 优先级设为1（低于主任务和IDLE任务），4KB栈足够
    xTaskCreate(fs_init_task, "fs_init", 4096, NULL, 1, NULL);
}

// 供状态页/其它模块查询文件系统是否可用
bool is_fs_ready(void) { return s_fs_ready; }

// 查询 SPIFFS 存储用量：返回 0~100 的已用百分比，并填好已用/总大小(KB)
int get_storage_used_kb(int *out_total_kb) {
    if (!s_fs_ready) {
        if (out_total_kb) *out_total_kb = 0;
        return 0;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK) {
        if (out_total_kb) *out_total_kb = 0;
        return 0;
    }
    if (out_total_kb) *out_total_kb = (int)(total / 1024);
    int pct = 0;
    if (total > 0) pct = (int)((used * 100) / total);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static void __attribute__((unused)) fs_deinit(void) {
    esp_vfs_spiffs_unregister("storage");
}

// ---------------------------------------------------------
// ADC 采样 (ESP32-C3 ADC1_CH0 = GPIO0)
// 注意：GPIO0 在 FoloToy AI Passport 上被复用了两个用途——
//   1) 电池电压检测（板上分压电路）
//   2) 三个实体键 UP/DOWN/OK 的电阻梯（三键共用 GPIO0，靠分压值区分）
//   因此这里必须提供“带校准的毫伏采样”，供电池估算与按键识别共用，
//   否则未校准的 raw*3300/4095 与键位电压区间会对不上，导致按键无反应。
// ---------------------------------------------------------
static void init_battery_adc(void) {
    adc_oneshot_unit_init_cfg_t cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&cfg, &adc_handle));
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,            // 量程约 0~3.3V
        .bitwidth = ADC_BITWIDTH_DEFAULT,    // 12bit
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_0, &chan_cfg));

    // 建校准曲线：ESP-IDF v5.5.x 仅支持 curve_fitting（官方示例仅初始化 unit_id/atten/bitwidth 三字段）；
    // 失败走未校准兜底换算（adc_cali_raw_to_voltage 不可用时，adc_read_mv 内部会用 raw*3100/4095 兜底）。
    adc_cali_curve_fitting_config_t cf_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cf_cfg, &adc_cali_handle) == ESP_OK) {
        adc_cali_ok = true;
        ESP_LOGI("ADC", "ADC 曲线拟合校准已启用");
    } else {
        adc_cali_ok = false;
        ESP_LOGW("ADC", "ADC 校准创建失败，将使用未校准换算（精度较低）");
    }
}

// 读取 GPIO0 的电压（mV）。多次采样取平均并做中值滤波，抑制 WiFi 射频噪声导致的跳变。
// 这是电池估算与按键识别的唯一数据源，保证两者用同一把"尺子"。
// 返回值：>=0 为实测毫伏；-1 表示 ADC 句柄无效或读取失败（调用方需自行处理，
//         不可把 -1 当成 0mV——0mV 恰好是 UP 键的有效按下电压）。
int adc_read_mv(void) {
    if (!adc_handle) return -1;

    const int N = 9;            // 采样次数（奇数便于取中值）
    int raw_buf[N];
    for (int i = 0; i < N; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc_handle, ADC_CHANNEL_0, &raw) != ESP_OK) return -1;
        raw_buf[i] = raw;
        esp_rom_delay_us(200);
    }
    // 冒泡排序取中值（抗脉冲干扰）
    for (int i = 0; i < N - 1; i++)
        for (int j = 0; j < N - 1 - i; j++)
            if (raw_buf[j] > raw_buf[j + 1]) { int t = raw_buf[j]; raw_buf[j] = raw_buf[j + 1]; raw_buf[j + 1] = t; }
    int raw = raw_buf[N / 2];

    int mv = 0;
    if (adc_cali_ok && adc_cali_raw_to_voltage(adc_cali_handle, raw, &mv) == ESP_OK) {
        return mv;
    }
    // 未校准兜底换算：C3 在 12dB 衰减下满量程约 3100mV
    return raw * 3100 / 4095;
}

static int read_battery_percent(void) {
    // ★ 改用板载 CW2017 电量计（I2C 0x63），bsp_battery 组件自带：
    //   硬件本身就是 Li-Poly profile 直接出 SOC%，比原 GPIO0 分压估算（GPIO0
    //   还被三键分压复用，误差本就大）准得多；并且 GPIO0 按键分压梯在按键按下时
    //   会把节点电压拉到 0~600mV，原"读 GPIO0 × 2"的方案按键按下时会把"电量"
    //   瞬间读到 0%，这也是状态页电量乱跳的根因之一。
    //
    // 充电状态启发式：板子 bsp_pins.h 未提供单独的 CHG 检测引脚，CW2017 也不提供
    // "是否在充"的寄存器位；观察到 USB 供电时电池电压被 VBUS 抬到 4.30V 以上（单节
    // 锂电自身不可能到这个电压），以此作为"USB 供电/充电中"的判据。
    if (!bsp_batt_ok) {
        // 幂等初始化：首次调用或 I2C 暂未就绪时补一次；失败则保持旧值并返回 -1 告知调用方。
        if (bsp_battery_init() == ESP_OK) {
            bsp_batt_ok = true;
            ESP_LOGI(TAG, "CW2017 电量计已就绪，电源状态将显示真实 SOC");
        } else {
            // ADC 兜底：bsp_battery 还没就绪/板子无电量计（极早期原型）时退回原来的 ADC 估算，
            // 保证 UI 永远有数；只是这个数值会受按键分压干扰，仅作兜底。
            if (!adc_handle) return battery_percent;
            int gpio_mv = adc_read_mv();
            if (gpio_mv < 0) return battery_percent;
            int batt_mv = gpio_mv * 2;
            int pct = (batt_mv - 3000) * 100 / 1200;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            battery_percent = pct;
            battery_mv = batt_mv;
            battery_charging = (batt_mv >= 4300);
            return pct;
        }
    }

    int soc = bsp_battery_soc();
    int mv  = bsp_battery_mv();
    if (soc >= 0) {
        battery_percent = soc;
    } else if (battery_percent < 0) {
        battery_percent = 0;
    }
    if (mv >= 0) battery_mv = mv;
    battery_charging = (battery_mv >= 4300) || (battery_percent >= 99 && battery_mv >= 4180);
    return battery_percent;
}

// ---------------------------------------------------------
// WiFi列表管理（多网络保存）
// ---------------------------------------------------------
static void load_wifi_list_from_nvs(void) {
    nvs_handle_t handle;
    saved_wifi_count = 0;
    if (nvs_open(WIFI_LIST_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i32(handle, "count", (int32_t*)&saved_wifi_count);
        char key_name[16];
        for (int i = 0; i < saved_wifi_count && i < 5; i++) {
            snprintf(key_name, sizeof(key_name), "ssid%d", i);
            size_t len = sizeof(saved_ssid_list[i]);
            nvs_get_str(handle, key_name, saved_ssid_list[i], &len);
        }
        nvs_get_i32(handle, "current", (int32_t*)&current_wifi_idx);
        nvs_close(handle);
    }
}

static int find_wifi_in_list(const char* ssid) {
    for (int i = 0; i < saved_wifi_count; i++) {
        if (strcmp(saved_ssid_list[i], ssid) == 0) return i;
    }
    return -1;
}

static int save_wifi_to_list(const char* ssid, const char* pass) {
    nvs_handle_t handle;
    if (strlen(ssid) == 0) return -1;
    int existing = find_wifi_in_list(ssid);
    if (existing >= 0) {
        // 已存在，只更新密码
        if (nvs_open(WIFI_LIST_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            char key_pass[16];
            snprintf(key_pass, sizeof(key_pass), "pass%d", existing);
            if (pass && strlen(pass) > 0) nvs_set_str(handle, key_pass, pass);
            nvs_set_i32(handle, "current", existing);
            current_wifi_idx = existing;
            nvs_commit(handle);
            nvs_close(handle);
        }
        return existing;
    }
    // 新WiFi
    if (saved_wifi_count >= 5) {
        ESP_LOGW(TAG, "WiFi list full, overwriting oldest entry");
        saved_wifi_count = 4; // 覆盖最后一个
    }
    int idx = saved_wifi_count++;
    // strlcpy 保证补上字符串结束符；原 strncpy 在 SSID 恰好占满时不会终止，后续 %s 会越界读
    strlcpy(saved_ssid_list[idx], ssid, sizeof(saved_ssid_list[idx]));
    if (nvs_open(WIFI_LIST_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        char key_name[16], key_pass[16];
        snprintf(key_name, sizeof(key_name), "ssid%d", idx);
        snprintf(key_pass, sizeof(key_pass), "pass%d", idx);
        nvs_set_str(handle, key_name, ssid);
        if (pass && strlen(pass) > 0) nvs_set_str(handle, key_pass, pass);
        nvs_set_i32(handle, "count", saved_wifi_count);
        nvs_set_i32(handle, "current", idx);
        current_wifi_idx = idx;
        nvs_commit(handle);
        nvs_close(handle);
    }
    return idx;
}

static bool delete_wifi_from_list(int idx) {
    if (idx < 0 || idx >= saved_wifi_count) return false;
    nvs_handle_t handle;
    if (nvs_open(WIFI_LIST_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    // 向前移动覆盖
    for (int i = idx; i < saved_wifi_count - 1; i++) {
        char k1[16], k2[16];
        snprintf(k1, sizeof(k1), "ssid%d", i);
        snprintf(k2, sizeof(k2), "ssid%d", i+1);
        char buf[32];
        size_t len = sizeof(buf);
        if (nvs_get_str(handle, k2, buf, &len) == ESP_OK) nvs_set_str(handle, k1, buf);
        snprintf(k1, sizeof(k1), "pass%d", i);
        snprintf(k2, sizeof(k2), "pass%d", i+1);
        len = sizeof(buf);
        if (nvs_get_str(handle, k2, buf, &len) == ESP_OK) nvs_set_str(handle, k1, buf);
        strcpy(saved_ssid_list[i], saved_ssid_list[i+1]);
    }
    saved_wifi_count--;
    if (current_wifi_idx >= saved_wifi_count) current_wifi_idx = saved_wifi_count - 1;
    nvs_set_i32(handle, "count", saved_wifi_count);
    nvs_set_i32(handle, "current", current_wifi_idx);
    nvs_commit(handle);
    nvs_close(handle);
    return true;
}

static void load_password_by_idx(int idx, char* out_pass, size_t out_len) {
    out_pass[0] = 0;
    if (idx < 0 || idx >= saved_wifi_count) return;
    nvs_handle_t handle;
    if (nvs_open(WIFI_LIST_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        char key_pass[16];
        snprintf(key_pass, sizeof(key_pass), "pass%d", idx);
        size_t len = out_len;
        nvs_get_str(handle, key_pass, out_pass, &len);
        nvs_close(handle);
    }
}

// ---------------------------------------------------------
// NVS配置加载/保存
// ---------------------------------------------------------
static void load_config_from_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(current_custom_name);
        if (nvs_get_str(handle, NVS_KEY_NAME, current_custom_name, &size) != ESP_OK) {
            strcpy(current_custom_name, "Matrix Clock");
        }
        size = sizeof(ntp_server_addr);
        if (nvs_get_str(handle, NVS_KEY_NTP, ntp_server_addr, &size) != ESP_OK) {
            strcpy(ntp_server_addr, "ntp.ntsc.ac.cn");
        }
        int32_t tmp;
        if (nvs_get_i32(handle, NVS_KEY_SAVER, &tmp) == ESP_OK) screensaver_theme = (int)tmp;
        if (nvs_get_i32(handle, NVS_KEY_SAVER_TIMEOUT, &tmp) == ESP_OK) saver_timeout_sec = (int)tmp;
        if (nvs_get_i32(handle, NVS_KEY_ALBUM_INTERVAL, &tmp) == ESP_OK) album_interval_sec = (int)tmp;
        if (nvs_get_i32(handle, NVS_KEY_SLEEP_KEY, &tmp) == ESP_OK) sleep_key = (int)tmp;
        if (sleep_key < SLEEP_KEY_UP || sleep_key > SLEEP_KEY_OK) sleep_key = SLEEP_KEY_UP;
        if (nvs_get_i32(handle, NVS_KEY_MENU_KEY, &tmp) == ESP_OK) menu_key = (int)tmp;
        if (menu_key < SLEEP_KEY_UP || menu_key > SLEEP_KEY_OK) menu_key = SLEEP_KEY_OK;
        // 两个按键互斥：若历史数据里存成了同一个键（例如从旧固件升级、或写入过程中断电），
        // 保持"熄屏按键"不动，把"功能菜单按键"挪到"非熄屏键的首个键"上，避免两功能绑死同键。
        if (menu_key == sleep_key) menu_key = (sleep_key == SLEEP_KEY_UP) ? SLEEP_KEY_DOWN : SLEEP_KEY_UP;
        // 同步给按键任务自持的运行时值（gui_main.c），保证重启后取值来源一致、立即可用
        gui_key_set_sleep_key(sleep_key);
        gui_key_set_menu_key(menu_key);
        size_t nfc_len = sizeof(nfc_content);
        if (nvs_get_str(handle, NVS_KEY_NFC_CONTENT, nfc_content, &nfc_len) != ESP_OK) nfc_content[0] = 0;
        int8_t flag;
        if (nvs_get_i8(handle, NVS_KEY_AVATAR_SET, &flag) == ESP_OK) avatar_exists = (flag != 0);
        if (nvs_get_i8(handle, NVS_KEY_BOOT_MUSIC_SET, &flag) == ESP_OK) boot_music_exists = (flag != 0);
        // 自动同步频率（默认"每小时"）
        int32_t mi;
        if (nvs_get_i32(handle, NVS_KEY_NTP_SYNC, &mi) == ESP_OK) ntp_sync_mode = (int)mi;
        if (ntp_sync_mode != SYNC_MODE_HOURLY && ntp_sync_mode != SYNC_MODE_DAILY)
            ntp_sync_mode = SYNC_MODE_HOURLY;
        // 整点报时开关（默认关闭）
        int8_t ch;
        if (nvs_get_i8(handle, NVS_KEY_CHIME_EN, &ch) == ESP_OK) chime_enabled = (ch != 0);
        else chime_enabled = false;
        // 静默时段（默认 22~7）
        int32_t s0 = chime_silent_start, e0 = chime_silent_end;
        if (nvs_get_i32(handle, NVS_KEY_CHIME_SIL_S, &s0) == ESP_OK) chime_silent_start = (int)s0;
        if (nvs_get_i32(handle, NVS_KEY_CHIME_SIL_E, &e0) == ESP_OK) chime_silent_end   = (int)e0;
        if (chime_silent_start < 0 || chime_silent_start > 23) chime_silent_start = CHIME_SILENT_DEFAULT_START;
        if (chime_silent_end   < 0 || chime_silent_end   > 23) chime_silent_end   = CHIME_SILENT_DEFAULT_END;
        nvs_close(handle);
    }
    load_wifi_list_from_nvs();
}

static void save_config_to_nvs(const char* name, const char* ntp, int theme, int timeout, int album_int) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (name && strlen(name) > 0) {
            nvs_set_str(handle, NVS_KEY_NAME, name);
            // strlcpy 保证以字符串结束符终止（名称占满 31 字节时 strncpy 不补终止符）
            strlcpy(current_custom_name, name, sizeof(current_custom_name));
        }
        if (ntp && strlen(ntp) > 0) {
            nvs_set_str(handle, NVS_KEY_NTP, ntp);
            strlcpy(ntp_server_addr, ntp, sizeof(ntp_server_addr));
        }
        nvs_set_i32(handle, NVS_KEY_SAVER, theme);
        nvs_set_i32(handle, NVS_KEY_SAVER_TIMEOUT, timeout);
        nvs_set_i32(handle, NVS_KEY_ALBUM_INTERVAL, album_int);
        nvs_set_i32(handle, NVS_KEY_SLEEP_KEY, sleep_key);   // 熄屏按键跟随"保存所有设置"一起持久化
        screensaver_theme = theme;
        saver_timeout_sec = timeout;
        album_interval_sec = album_int;
        nvs_commit(handle);
        nvs_close(handle);
    }
}

// 单独保存 NFC 标签内容（个人信息页的"保存 NFC 内容"按钮）
static void save_nfc_content_to_nvs(const char* content) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_NFC_CONTENT, content ? content : "");
        nvs_commit(handle);
        nvs_close(handle);
    }
    strlcpy(nfc_content, content ? content : "", sizeof(nfc_content));
}

// ---------------------------------------------------------
// 时间同步
// ---------------------------------------------------------
// SNTP 同步成功回调：把时间基准写 NVS，下次开机就能直接恢复
static void sntp_sync_cb(struct timeval *tv) {
    (void)tv;
    time_t now = time(NULL);
    ESP_LOGI(TAG, "NTP 时间已同步: %s", ctime(&now));
    time_sync_notify_persist();  // 立刻把基准写 NVS
}

static void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP: %s", ntp_server_addr);
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntp_server_addr);
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
}

static void sync_time_manual(void) {
    initialize_sntp();
    ESP_LOGI(TAG, "Manual time sync requested");
}

// 供状态页判断"当前时间是否已由 NTP 校正到真实时间"：
// 只要系统年份已达到当前年份量级，就认为已同步（否则是 1970 占位或仅 NVS 恢复的历史值）。
// 与 ntp_auto_sync_task 里的 looks_synced 判定保持一致的口径。
bool time_is_synced(void) {
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    return (timeinfo.tm_year >= (2026 - 1900));
}

// 判断 hour 是否落在静默区间。两种情况：
// ---------------------------------------------------------
// 整点报时：用 bsp_audio 合成几声响铃（无需音频文件，纯代码生成，节省 SPIFFS）
// 新规则（5 短 + 1 长、按秒触发）：
//   - XX:59:55 / 56 / 57 / 58 / 59  每秒 1 声短滴（1000 Hz，约 100 ms）
//   - XX+1:00:00                   1 声长嘀（2000 Hz，约 1 s）
// 每声独立任务播放，不阻塞 NTP 轮询 / 按键 / httpd。
// ---------------------------------------------------------
#define CHIME_SAMPLE_RATE 16000
#define CHIME_CHUNK       512
#define CHIME_SHORT_FREQ  1000
#define CHIME_SHORT_MS    100
#define CHIME_SHORT_AMP   8000
#define CHIME_LONG_FREQ   2000
#define CHIME_LONG_MS     1000
#define CHIME_LONG_AMP    9000

// 播一个单音（短滴或长嘀），独立短任务，按秒触发避免跨秒漂移
static void chime_beep_task(void *arg) {
    int freq   = (int)((intptr_t)arg >> 16) & 0xFFFF;
    int ms     = (int)((intptr_t)arg) & 0xFFFF;
    // 音量从 arg 携带，需要 3 参数 → 改用独立封装

    vTaskDelay(pdMS_TO_TICKS(30));
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "整点报时：codec 初始化失败，跳过本声");
        vTaskDelete(NULL); return;
    }
    if (bsp_audio_set_format(CHIME_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "整点报时：设置采样格式失败，跳过本声");
        vTaskDelete(NULL); return;
    }
    bsp_audio_set_volume(70);

    int16_t *buf = malloc(CHIME_CHUNK * sizeof(int16_t));
    if (!buf) { vTaskDelete(NULL); return; }

    int total  = CHIME_SAMPLE_RATE * ms / 1000;
    int phase  = 0;
    int period = (freq > 0) ? (CHIME_SAMPLE_RATE / freq) : 0;
    int amp    = (freq == CHIME_LONG_FREQ) ? CHIME_LONG_AMP : CHIME_SHORT_AMP;
    while (total > 0) {
        int n = total < CHIME_CHUNK ? total : CHIME_CHUNK;
        for (int i = 0; i < n; i++) {
            if (period == 0) { buf[i] = 0; continue; }
            // 带淡入淡出包络，避免爆音
            int p = phase % period;
            int tri = (p < period/2) ? p : (period - p);
            int raw = (tri * 4 / period - 1) * amp;
            // 简单包络：前 10ms 淡入、后 10ms 淡出
            int elapsed_ms = (phase * 1000) / CHIME_SAMPLE_RATE;
            int remain_ms  = ms - elapsed_ms;
            if (elapsed_ms < 10)        raw = raw * elapsed_ms / 10;
            else if (remain_ms < 10)    raw = raw * remain_ms / 10;
            buf[i] = (int16_t)raw;
            phase++;
        }
        bsp_audio_write(buf, (size_t)n * sizeof(int16_t));
        total -= n;
    }
    // 长音结尾停 100ms 静音，防止尾音拖到下一循环
    if (freq == CHIME_LONG_FREQ) {
        memset(buf, 0, CHIME_CHUNK * sizeof(int16_t));
        int silence_samples = CHIME_SAMPLE_RATE * 100 / 1000;
        while (silence_samples > 0) {
            int n = silence_samples < CHIME_CHUNK ? silence_samples : CHIME_CHUNK;
            bsp_audio_write(buf, (size_t)n * sizeof(int16_t));
            silence_samples -= n;
        }
    }
    free(buf);
    vTaskDelete(NULL);
}

// 触发一声短滴或长嘀；以独立任务播放，不阻塞主循环
static void chime_trigger(int freq, int ms) {
    intptr_t arg = ((intptr_t)(freq & 0xFFFF) << 16) | (ms & 0xFFFF);
    xTaskCreate(chime_beep_task, "chimeb", 3072, (void*)arg, 2, NULL);
}

// 判断给定整点小时是否在静默时段（前后跨午夜都支持）
static bool chime_silent_for_hour(int hour) {
    if (chime_silent_start == chime_silent_end) return false;
    if (chime_silent_start < chime_silent_end) {
        return (hour >= chime_silent_start && hour < chime_silent_end);
    } else {
        // 跨午夜：如 22~7 表示 22、23、0、1、2、3、4、5、6 都静默
        return (hour >= chime_silent_start || hour < chime_silent_end);
    }
}

// ---------------------------------------------------------
// 整点报时：经典"5 短 1 长"
//   短声：整点前 5 秒开始，每秒一声 —— 59 分 55 秒、56、57、58、59 秒；
//   长声：整点 00 分 00 秒。
// 判据统一用 delta = 距下一个整点的秒数：
//   delta = 5/4/3/2/1 → 第 1~5 声短滴；delta = 0 → 整点长嘀。
// 这样不依赖"恰好落在 s==55 那一秒"（主循环被 NTP/HTTP 拖慢跨过 1 秒
// 时，旧写法会整轮漏掉），delta 直接给出当前应播第几声，不会丢整轮。
// ---------------------------------------------------------
static int  chime_target_hour = -1;   // 本次报时对准的整点小时
static int  chime_step        = -1;   // -1 未开始 / 0~4 已播第 1~5 短声 / 5 长声已播
static bool chime_active      = false; // 本次窗口是否真正发声（落在静默时段则为 false）

// 整点报时状态机推进：每 1 秒由主循环调用
static void chime_tick(const struct tm *ti) {
    if (!chime_enabled || ti->tm_year < (2024 - 1900)) {
        chime_target_hour = -1; chime_step = -1; chime_active = false;
        return;
    }

    const int h = ti->tm_hour;
    const int m = ti->tm_min;
    const int s = ti->tm_sec;

    // delta = 距下一个整点的秒数；仅报时窗口内有效，其余为 -1
    int delta = -1;
    if (m == 59)                   delta = 60 - s;   // 59:55→5 ... 59:59→1
    else if (m == 0 && s == 0)     delta = 0;        // 00:00→0

    // ---- 入场：进入 59 分的最后 5 秒，为"下一个整点"开一轮 ----
    if (m == 59 && s >= 55) {
        int next_hour = (h + 1) % 24;
        if (chime_target_hour != next_hour) {
            chime_target_hour = next_hour;
            chime_step        = -1;
            chime_active      = !chime_silent_for_hour(next_hour);
            if (chime_active) {
                ESP_LOGI(TAG, "整点报时：目标 %02d:00，自 %02d:59:55 起每秒一声短滴", next_hour, h);
            } else {
                ESP_LOGI(TAG, "整点报时：%02d:00 位于静默时段（%d 时~%d 时），本轮跳过",
                         next_hour, chime_silent_start, chime_silent_end);
            }
        }
    }

    // ---- 短声：delta 5→1 依次对应第 1~5 声 ----
    if (delta >= 1 && delta <= 5 && chime_active && chime_target_hour != -1) {
        int want_step = 5 - delta;   // delta5→0(第1声) ... delta1→4(第5声)
        if (chime_step < want_step) {
            chime_step = want_step;
            ESP_LOGI(TAG, "整点报时：第 %d 声短滴（%02d:59:%02d）", want_step + 1, h, s);
            chime_trigger(CHIME_SHORT_FREQ, CHIME_SHORT_MS);
        }
    }

    // ---- 长声：整点 00:00:00 准时一声 ----
    if (delta == 0) {
        if (chime_target_hour != h) {   // 兜底：59 分窗口整段被跳过时也能补上长声
            chime_target_hour = h;
            chime_step        = -1;
            chime_active      = !chime_silent_for_hour(h);
        }
        if (chime_active && chime_step < 5) {
            chime_step = 5;
            ESP_LOGI(TAG, "整点报时：%02d:00:00 整点长嘀", h);
            chime_trigger(CHIME_LONG_FREQ, CHIME_LONG_MS);
        }
    }

    // ---- 离开报时窗口后复位，允许下一轮 ----
    if (delta < 0 && chime_target_hour != -1) {
        chime_target_hour = -1;
        chime_step        = -1;
        chime_active      = false;
    }
}

static void ntp_auto_sync_task(void *pvParameters) {
    (void)pvParameters;
    int sync_retry = 0;
    int last_sync_hour = -1;   // ★ 防重：记录"上一次触发自动同步"是哪个小时/哪天的key，避免整点窗口内每秒都触发
    chime_target_hour = -1; chime_step = -1; chime_active = false;
    while (1) {
        time_t now = 0;
        struct tm timeinfo = { 0 };
        time(&now);
        localtime_r(&now, &timeinfo);

        // 判断"系统时间是否已被 NTP 校正过"：
        //   年份 >= 2026 视为真实的网络时间。若只停留在"从 NVS 恢复的历史时间"
        //   （time_is_restored() 为 true 但年份仍偏旧/时间已在过去），同样需要补同步，
        //   把滞后或掉电期间损失的时间校正回来。
        bool looks_synced = (timeinfo.tm_year >= (2026 - 1900));

        // ★ 兜底自动同步：STA 已连上、但时间尚未被 NTP 校正过，就持续补同步。
        if (wifi_connected_status && !looks_synced) {
            last_sync_hour = -1;
            if (sync_retry == 0) {
                ESP_LOGI(TAG, "WiFi 已连接但时间未校正（当前 %04d-%02d-%02d %02d:%02d:%02d），触发 NTP 同步",
                         timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                         timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            }
            initialize_sntp();
            sync_retry++;
            if (sync_retry > 6) sync_retry = 10;
            if (sync_retry <= 6) vTaskDelay(pdMS_TO_TICKS(30000));
            else                vTaskDelay(pdMS_TO_TICKS(300000));
            continue;
        }

        sync_retry = 0;

        // ★ 按用户选择的频率触发自动同步：
        //   每小时（HOURLY）：每到整点 min==0 && sec==0~5 之间**只触发一次**
        //                    （用 last_sync_hour 记"这一小时已经触发过"，秒数窗口跨过去也不会重入）；
        //   每天零时（DAILY）：tm_hour==0 && min==0 触发一次，当天不再触发。
        if (wifi_connected_status && looks_synced) {
            bool trigger = false;
            int cur_hour_key = (ntp_sync_mode == SYNC_MODE_DAILY)
                               ? timeinfo.tm_mday * 24     // 按天做 key，每天只触发一次
                               : timeinfo.tm_hour;         // 按小时做 key，每小时只触发一次
            bool on_the_hour = (timeinfo.tm_min == 0 && timeinfo.tm_sec < 5);

            if (ntp_sync_mode == SYNC_MODE_HOURLY) {
                if (on_the_hour && last_sync_hour != timeinfo.tm_hour) {
                    trigger = true;
                    last_sync_hour = timeinfo.tm_hour;
                }
            } else if (ntp_sync_mode == SYNC_MODE_DAILY) {
                if (on_the_hour && timeinfo.tm_hour == 0 && last_sync_hour != cur_hour_key) {
                    trigger = true;
                    last_sync_hour = cur_hour_key;
                } else if (timeinfo.tm_hour != 0) {
                    last_sync_hour = -1;   // 离开 0 点后重置，次日 0 点再触发
                }
            }
            if (trigger) {
                ESP_LOGI(TAG, "自动同步时间（%s 整点 %02d:00）",
                         (ntp_sync_mode == SYNC_MODE_DAILY) ? "每天零时" : "每小时",
                         timeinfo.tm_hour);
                sync_time_manual();
            }
        }

        // ★ 整点报时：每 1 秒推进一次状态机（独立于"是否已连 WiFi"判断，
        //   让从 NVS 恢复时间、无外网的场景下也能按时报时；chime_tick 内部
        //   已经做了"年份 >= 2024" 等真实性校验，不会在 1970 乱报）。
        chime_tick(&timeinfo);

        read_battery_percent();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------
// 获取相册文件列表
// ---------------------------------------------------------
// 相册文件判定：兼容旧版上传的 .bin 与新版压缩后的 .jpg
static bool is_album_file(const char* name) {
    return strstr(name, ".jpg") != NULL || strstr(name, ".bin") != NULL;
}

static int get_album_file_count(void) {
    DIR* d = opendir(STORAGE_MOUNT_POINT "/album");
    if (!d) return 0;
    int cnt = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_REG && is_album_file(e->d_name)) cnt++;
    }
    closedir(d);
    return cnt;
}

static void get_album_filenames(char names[30][64], int* out_cnt) {
    *out_cnt = 0;
    DIR* d = opendir(STORAGE_MOUNT_POINT "/album");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && *out_cnt < 30) {
        if (e->d_type == DT_REG && is_album_file(e->d_name)) {
            strncpy(names[*out_cnt], e->d_name, 63);
            names[*out_cnt][63] = 0;
            (*out_cnt)++;
        }
    }
    closedir(d);
}

// ---------------------------------------------------------
// 多部分表单解析辅助
// ---------------------------------------------------------
static const char* find_boundary(const char* ctype, char* boundary_out, int maxlen) {
    const char* b = strstr(ctype, "boundary=");
    if (!b) return NULL;
    b += 9;
    if (*b == '"') { b++; }
    int i;
    for (i = 0; i < maxlen-1 && b[i] && b[i] != '"' && b[i] != ';' && b[i] != '\r' && b[i] != '\n'; i++) {
        boundary_out[i] = b[i];
    }
    boundary_out[i] = 0;
    return boundary_out;
}

// ---------------------------------------------------------
// HTTP: 主页面（4标签页）
// ---------------------------------------------------------
// 把字符串转义成可安全放入 HTML 属性的形式
static void html_attr_escape(const char* in, char* out, size_t out_len) {
    size_t o = 0;
    if (!in) in = "";
    for (const char* p = in; *p && o + 6 < out_len; p++) {
        switch (*p) {
            case '&':  memcpy(out + o, "&amp;", 5);  o += 5; break;
            case '<':  memcpy(out + o, "&lt;", 4);   o += 4; break;
            case '>':  memcpy(out + o, "&gt;", 4);   o += 4; break;
            case '"':  memcpy(out + o, "&quot;", 6); o += 6; break;
            case '\'': memcpy(out + o, "&#39;", 5);  o += 5; break;
            default:   out[o++] = *p; break;
        }
    }
    out[o] = 0;
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    // 显式声明短连接 + UTF-8，发送完毕后 httpd 立即关闭 TCP，避免 keep-alive 半开连接占槽位
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    const char *conn_str = wifi_connected_status 
        ? "<span style='color:#0f0;font-weight:bold'>● 已连接</span>" 
        : "<span style='color:red;font-weight:bold'>● 未连接</span>";

    read_battery_percent();

    // 电源状态：battery_buf 显示 "XX% (XXXXmV)"；充电状态根据电压启发式判断
    // （USB 供电/充满 / 电池供电 / 未检测到电量计）。
    char battery_buf[64];
    char chg_buf[96];
    if (!bsp_batt_ok) {
        snprintf(chg_buf, sizeof(chg_buf),
                 "<span class='status-badge warn'>电量计未就绪（ADC 估算）</span>");
    } else if (battery_charging) {
        snprintf(chg_buf, sizeof(chg_buf),
                 "<span class='status-badge ok'>USB 供电 / 充电中</span>");
    } else {
        snprintf(chg_buf, sizeof(chg_buf),
                 "<span class='status-badge ok'>电池供电</span>");
    }
    snprintf(battery_buf, sizeof(battery_buf), "%d%% (%d mV)",
             battery_percent, battery_mv > 0 ? battery_mv : 0);

    // 整点报时状态字符串
    char chime_stat_buf[96];
    if (!chime_enabled) {
        snprintf(chime_stat_buf, sizeof(chime_stat_buf),
                 "<span class='status-badge err'>已关闭</span>");
    } else {
        // 判断"当前小时"是否在静默时段：59 分时要看下一个小时（因为此时正为下一个整点做倒计时），
        // 否则看当前小时。这样能准确反映用户当前会听到的状态。
        time_t _now2; struct tm _ti2;
        time(&_now2); localtime_r(&_now2, &_ti2);
        int cur_h = _ti2.tm_hour;
        if (_ti2.tm_min == 59) cur_h = (cur_h + 1) % 24;
        if (chime_silent_for_hour(cur_h)) {
            snprintf(chime_stat_buf, sizeof(chime_stat_buf),
                     "<span class='status-badge warn'>已开启（当前静默中）</span>");
        } else {
            snprintf(chime_stat_buf, sizeof(chime_stat_buf),
                     "<span class='status-badge ok'>已开启</span>");
        }
    }

    const char *sync_mode_str = (ntp_sync_mode == SYNC_MODE_DAILY) ? "每天零时" : "每小时整点";

    char time_buf[40], date_buf[40];
    const char* weekday_buf = "星期--";
    time_t now; struct tm timeinfo;
    time(&now); localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2024 - 1900)) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday);
        const char* weekdays[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
        weekday_buf = weekdays[timeinfo.tm_wday];
    } else {
        strcpy(time_buf, "未同步");
        strcpy(date_buf, "----/--/--");
    }

    // HTML头部 + 标签页CSS/JS
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<title>Matrix Passport 管理中心</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:'Consolas','Courier New',monospace;background:#0a0a0a;color:#0f0;padding:15px;max-width:500px;margin:0 auto;font-size:16px;line-height:1.6}"
        "h1{text-align:center;font-size:24px;margin-bottom:10px;text-shadow:0 0 8px #0f0}"
        ".tabs{display:flex;border-bottom:2px solid #0f0;margin-bottom:15px}"
        ".tab{flex:1;padding:10px 2px;text-align:center;cursor:pointer;border:1px solid #0f0;border-bottom:none;"
        "background:#111;color:#0a0;font-size:13px;font-weight:bold;transition:all .2s}"
        ".tab.active{background:#0f0;color:#000;box-shadow:0 0 10px #0f0}"
        ".tab:hover:not(.active){background:#003300;color:#0f0}"
        ".tab-content{display:none;padding:15px;border:1px solid #0f0;border-radius:0 0 6px 6px;background:#0d0d0d}"
        ".tab-content.active{display:block}"
        ".card{background:#111;border:1px solid #060;border-radius:6px;padding:14px;margin-bottom:12px}"
        ".card-title{font-weight:bold;border-bottom:1px solid #060;padding-bottom:8px;margin-bottom:12px;color:#0f0;font-size:17px}"
        ".row{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px dashed #030}"
        ".row:last-child{border-bottom:none}"
        ".label{color:#0a0;font-size:15px}"
        ".value{color:#0f0;font-weight:bold;font-size:15px}"
        "input,select,button{width:100%;padding:10px;margin:8px 0;border-radius:4px;border:1px solid #0f0;background:#000;color:#0f0;font-family:inherit;font-size:16px}"
        "button{background:#0f0;color:#000;font-weight:bold;cursor:pointer;transition:all .15s;font-size:16px}"
        "button:hover{background:#0c0;box-shadow:0 0 8px #0f0}"
        "button.danger{background:#500;border-color:#f00;color:#f00}"
        "button.danger:hover{background:#800;box-shadow:0 0 8px #f00}"
        "button.mini{width:auto;padding:6px 14px;font-size:14px;display:inline-block}"
        ".form-group{margin-bottom:12px}"
        "label{display:block;color:#0a0;font-size:15px;margin-bottom:4px}"
        ".hint{font-size:15px;color:#060;margin-top:3px;line-height:1.6}"
        ".switch-wrap{position:relative;display:inline-block;width:54px;height:30px;flex-shrink:0}"
        ".switch-wrap input{opacity:0;width:0;height:0}"
        ".switch-btn{position:absolute;cursor:pointer;inset:0;background:#300;border-radius:30px;"
        "border:1px solid #600;transition:.3s}"
        ".switch-btn:before{content:'';position:absolute;height:22px;width:22px;left:3px;bottom:3px;"
        "background:#800;border-radius:50%;transition:.3s}"
        ".switch-wrap input:checked+.switch-btn{background:#060;border-color:#0f0;box-shadow:0 0 8px #0f0}"
        ".switch-wrap input:checked+.switch-btn:before{transform:translateX(24px);background:#0f0}"
        ".wifi-item{display:flex;justify-content:space-between;align-items:center;padding:10px;border:1px solid #060;border-radius:4px;margin-bottom:8px;background:#080808}"
        ".wifi-name{color:#0f0;font-weight:bold;font-size:16px}"
        ".album-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:10px}"
        ".album-item{border:1px solid #060;border-radius:4px;padding:6px;text-align:center;font-size:14px;background:#080808}"
        ".album-item .del{color:#f00;cursor:pointer;font-size:14px;display:block;margin-top:3px}"
        ".album-name{color:#0a0;font-size:13px;margin-top:3px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
        ".album-btns{display:flex;gap:4px;margin-top:4px}"
        ".mini-btn{flex:1;display:block;padding:6px 0;font-size:14px;background:#0f0;color:#000;text-decoration:none;"
        "border-radius:3px;font-weight:bold}"
        ".mini-btn.danger{background:#500;color:#f00;border:1px solid #f00}"
        ".mini-btn.danger:hover{background:#800}"
        ".progress-bar{width:100%;height:24px;background:#222;border:1px solid #0f0;border-radius:10px;overflow:hidden;margin:10px 0}"
        ".progress-fill{height:100%;background:#0f0;transition:width .3s}"
        ".status-badge{display:inline-block;padding:4px 12px;border-radius:10px;font-size:15px}"
        ".ok{background:#060;color:#0f0}"
        ".warn{background:#660;color:#ff0}"
        ".err{background:#600;color:#f00}"
        ".dkm{padding:8px 10px;margin-bottom:6px;border-left:3px solid #0f0;background:#080808;color:#0f0;font-size:15px;word-break:break-all;line-height:1.6}"
        "hr{border:none;border-top:1px solid #060;margin:12px 0}"
        "</style>"
        "<script>"
        "var DK_ON="
        // 服务端注入弹幕总开关状态（true/false），前端据此决定是否轮询弹幕接口
    );
    httpd_resp_sendstr_chunk(req, danmaku_get_enabled() ? "true" : "false");
    httpd_resp_sendstr_chunk(req,
        ";"
        "function switchTab(id){document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));"
        "document.querySelectorAll('.tab-content').forEach(c=>c.classList.remove('active'));"
        "document.getElementById(id).classList.add('active');"
        "document.querySelector('[data-tab=\"'+id+'\"]').classList.add('active');"
        "try{sessionStorage.setItem('mxtab',id);}catch(e){}"
        "if(id==='tab-danmaku')pollDanmaku();}"
        "function confirmDel(msg){return confirm(msg);}"
        // ★ 按键冲突前端拦截：熄屏按键 与 功能菜单按键 不允许选同一个键。
        //   服务端在 save_settings_handler 里也做了同样的兜底检查（双保险）。
        //   直接在 onsubmit 里返回 false 即可阻止提交，用户不必等服务端往返。
        "function checkKeyConflict(f){"
        "var s=f.querySelector('[name=sleep_key]').value;"
        "var m=f.querySelector('[name=menu_key]').value;"
        "if(s===m){"
        "var names={'0':'UP键','1':'DOWN键','2':'OK键'};"
        "alert('按键设置冲突：\\n\\n「熄屏按键」和「功能菜单按键」都选择了 '+names[s]+'。\\n\\n两个功能不能绑在同一个按键上，请为其中一个改选别的按键。');"
        "return false;}"
        "return true;}"
        "function onChimeToggle(cb){"
        "var g=document.getElementById('chime_sil_group');if(!g)return;"
        "g.style.opacity=cb.checked?'1':'0.45';"
        "g.style.pointerEvents=cb.checked?'auto':'none';}"
        // 页面加载后让静默段根据开关状态呈现（用 DOMContentLoaded）
        "document.addEventListener('DOMContentLoaded',function(){"
        "var cb=document.querySelector('input[name=chime_en]');if(cb)onChimeToggle(cb);});"
        "function checkUploadSize(input, maxKB){if(input.files[0] && input.files[0].size > maxKB*1024){alert('文件大小超过限制('+maxKB+'KB)!');input.value='';return false;}return true;}"
        // 相册上传：先在浏览器端把图片等比缩放到不超过 240x320 并压成 JPEG，再上传，
        // 这样单张仅 15~25KB，30 张合计约 600KB，不会超出 SPIFFS 实际可用空间。
        "function uploadAlbum(ev){"
        "if(ev)ev.preventDefault();"
        "var f=document.getElementById('albumFile').files[0];"
        "var st=document.getElementById('albumStatus');"
        "if(!f){alert('请先选择图片');return false;}"
        "if(f.size>8*1024*1024){alert('原图过大(超过8MB)，请先自行压缩');return false;}"
        "st.textContent='正在压缩...';"
        "var img=new Image();"
        "img.onload=function(){"
        "var MW=240,MH=320;"
        "var sc=Math.min(MW/img.width,MH/img.height,1);"
        "var w=Math.max(1,Math.round(img.width*sc));"
        "var h=Math.max(1,Math.round(img.height*sc));"
        "var c=document.createElement('canvas');c.width=w;c.height=h;"
        "var cx=c.getContext('2d');cx.drawImage(img,0,0,w,h);"
        "c.toBlob(function(blob){"
        "if(!blob){st.textContent='压缩失败，请换一张图片';return;}"
        "var fd=new FormData();fd.append('photo',blob,'album.jpg');"
        "st.textContent='正在上传('+w+'x'+h+', '+Math.round(blob.size/1024)+'KB)...';"
        "fetch('/upload_album',{method:'POST',body:fd})"
        ".then(function(r){st.textContent='上传完成，正在刷新...';setTimeout(function(){location.reload();},600);})"
        ".catch(function(e){st.textContent='上传失败: '+e;});"
        "},'image/jpeg',0.8);"
        "};"
        "img.onerror=function(){st.textContent='图片读取失败';};"
        "img.src=URL.createObjectURL(f);"
        "return false;}"
        "function escHtml(s){return String(s).replace(/[&<>\"']/g,function(c){"
        "return {\"&\":\"&amp;\",\"<\":\"&lt;\",\">\":\"&gt;\",\"\\\"\":\"&quot;\",\"'\":\"&#39;\"}[c];});}"
        "function pollDanmaku(){"
        // 仅当"抖音弹幕"标签页处于激活状态、且总开关已打开时才轮询，
        // 避免停在其它标签页时仍每 3 秒打一次接口（这正是串口 404 刷屏的诱因之一）。
        "var tp=document.getElementById('tab-danmaku');"
        "if(!tp||!tp.classList.contains('active')||DK_ON!==true)return;"
        "if(document.hidden)return;"
        "fetch('/danmaku_status').then(function(r){return r.json();}).then(function(d){"
        "var st=document.getElementById('dk_state');if(st)st.textContent=d.state;"
        "var tt=document.getElementById('dk_total');if(tt)tt.textContent=d.total;"
        "var el=document.getElementById('dk_list');"
        "if(el){"
        "if(d.msgs&&d.msgs.length){el.innerHTML=d.msgs.map(function(m){"
        "return '<div class=\\'dkm\\'>'+escHtml(m)+'</div>';}).join('');}"
        "else{el.innerHTML='<div class=\\'hint\\'>暂无弹幕</div>';}"
        "}"
        "}).catch(function(){});}"
        "setInterval(pollDanmaku,3000);"
        // ===== 自动刷新：状态页每 5 秒 fetch /status_data 局部更新 JSON，不再重载整页（~15KB），
        //        切到其它标签/后台(visibilitychange)时暂停，避免弱网/后台触发 EAGAIN send:11 =====
        "function refreshStatus(){"
        "var st=document.getElementById('tab-status');if(!st||!st.classList.contains('active'))return;"
        "if(document.hidden)return;"
        "fetch('/status_data',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){"
        "var b=document.getElementById('st_bat');if(b)b.textContent=d.bat+'% ('+d.mv+' mV)';"
        "var chg=document.getElementById('st_chg');if(chg){"
        "if(d.chg==='charging'){chg.innerHTML=\"<span class='status-badge ok'>USB 供电 / 充电中</span>\";}"
        "else if(d.chg==='battery'){chg.innerHTML=\"<span class='status-badge ok'>电池供电</span>\";}"
        "else{chg.innerHTML=\"<span class='status-badge warn'>电量计未就绪（ADC 估算）</span>\";}"
        "}"
        "var dt=document.getElementById('st_date');if(dt)dt.textContent=d.date+' '+d.week;"
        "var tm=document.getElementById('st_time');if(tm)tm.textContent=d.time;"
        "var ts=document.getElementById('st_tsrc');if(ts){"
        "ts.innerHTML=(d.tsrc===2)?\"<span class='status-badge ok'>网络授时已同步</span>\":"
        "(d.tsrc===1?\"<span class='status-badge warn'>关机时间已恢复（待NTP校正）</span>\":"
        "\"<span class='status-badge err'>未同步</span>\");}"
        "var store=document.getElementById('st_store');if(store)store.textContent="
        "'本机存储用量 '+d.pct+'%（已用 '+d.used+' KB / 共 '+d.total+' KB，剩余 '+d.free+' KB）';"
        "var bar=document.getElementById('st_bar');if(bar){"
        "bar.style.width=d.pct+'%';"
        "bar.style.background='linear-gradient(90deg,#0f0 0%,#0a0 '+d.pct+'%,#300 '+d.pct+'%,#500 100%)';"
        "}"
        "}).catch(function(){});"
        "}"
        "refreshStatus();"
        "var _refT=setInterval(refreshStatus,5000);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)refreshStatus();});"
        "window.addEventListener('beforeunload',function(){clearInterval(_refT);});"
        "(function(){var s=null;try{s=sessionStorage.getItem('mxtab');}catch(e){}"
        "if(s&&document.getElementById(s))switchTab(s);})();"
        "</script>"
        "</head><body>"
        "<h1>🟢 MATRIX PASSPORT 管理中心</h1>"
        "<div class='tabs'>"
        "<div class='tab active' data-tab='tab-status' onclick='switchTab(\"tab-status\")'>📊 状态</div>"
        "<div class='tab' data-tab='tab-settings' onclick='switchTab(\"tab-settings\")'>⚙️ 设置</div>"
        "<div class='tab' data-tab='tab-profile' onclick='switchTab(\"tab-profile\")'>👤 个人信息</div>"
        "<div class='tab' data-tab='tab-album' onclick='switchTab(\"tab-album\")'>🖼️ 相册</div>"
        "<div class='tab' data-tab='tab-danmaku' onclick='switchTab(\"tab-danmaku\")'>🎤 抖音弹幕</div>"
        "</div>"
    );

    // ====== 标签页1: 状态 ======
    static char chunk[HTML_CHUNK_SIZE];   // 放静态区，避免 5 个标签页的大片段撑爆 8KB 的 httpd 任务栈
    int st_total_kb = 0;
    int st_pct = get_storage_used_kb(&st_total_kb);
    int st_free_pct = 100 - st_pct;
    snprintf(chunk, sizeof(chunk),
        "<div id='tab-status' class='tab-content active' data-status-page>"
        "<div class='card'><div class='card-title'>🔌 网络状态</div>"
        "<div class='row'><span class='label'>当前WiFi</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>连接状态</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>IP地址</span><span class='value'>192.168.4.1 (AP)</span></div>"
        "<div class='row'><span class='label'>MAC地址</span><span class='value' id='mac'>-</span></div>"
        "</div>"
        "<div class='card'><div class='card-title'>🔋 电源状态</div>"
        "<div class='row'><span class='label'>电池电量</span><span class='value' id='st_bat'>%s</span></div>"
        "<div class='row'><span class='label'>充电状态</span><span class='value' id='st_chg'>%s</span></div>"
        "</div>"
        "<div class='card'><div class='card-title'>🕐 时间同步</div>"
        "<div class='row'><span class='label'>NTP服务器</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>自动同步频率</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>整点报时</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>时间来源</span><span class='value' id='st_tsrc'>%s</span></div>"
        "<div class='row'><span class='label'>当前日期</span><span class='value' id='st_date'>%s %s</span></div>"
        "<div class='row'><span class='label'>当前时间</span><span class='value' style='font-size:28px' id='st_time'>%s</span></div>"
        "<form action='/sync' method='POST' style='margin-top:10px'>"
        "<button type='submit'>🔄 立即同步时间</button></form>"
        "</div>"
        "<div class='card'><div class='card-title'>💾 存储状态</div>"
        "<div class='row'><span class='label'>文件系统</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>头像</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>开机音乐</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>相册图片</span><span class='value'>%d / 30 张</span></div>"
        "<div class='row'><span class='label'>当前屏保</span><span class='value'>%s</span></div>"
        "<hr>"
        "<div style='font-size:14px;color:#0a0;margin-bottom:5px' id='st_store'>本机存储用量 %d%%（已用 %d KB / 共 %d KB，剩余 %d KB）</div>"
        "<div class='progress-bar' title='已用 %d%% / 空闲 %d%%'>"
        "<div class='progress-fill' id='st_bar' style='width:%d%%;background:linear-gradient(90deg,#0f0 0%%,#0a0 %d%%,#300 %d%%,#500 100%%'></div>"
        "</div>"
        "<div style='display:flex;justify-content:space-between;font-size:13px;color:#060'>"
        "<span>■ 已占用 %d%%</span><span>□ 未占用 %d%%</span>"
        "</div>"
        "</div>"
        "<p style='text-align:center;margin-top:15px;font-size:14px;color:#060'>状态页每 5 秒自动局部刷新（仅更新约 200B 数据）；后台时暂停刷新</p>"
        "</div>",
        (current_wifi_idx >= 0 && current_wifi_idx < saved_wifi_count) ? saved_ssid_list[current_wifi_idx] : "未保存",
        conn_str, battery_buf, chg_buf, ntp_server_addr,
        sync_mode_str,
        chime_stat_buf,
        time_is_synced() ? "<span class='status-badge ok'>网络授时已同步</span>"
                         : (time_is_restored() ? "<span class='status-badge warn'>关机时间已恢复（待NTP校正）</span>"
                                               : "<span class='status-badge err'>未同步</span>"),
        date_buf, weekday_buf, time_buf,
        is_fs_ready() ? "<span class='status-badge ok'>正常</span>" : "<span class='status-badge err'>不可用</span>",
        avatar_exists ? "<span class='status-badge ok'>已上传</span>" : "<span class='status-badge err'>未上传</span>",
        boot_music_exists ? "<span class='status-badge ok'>已上传</span>" : "<span class='status-badge err'>默认音乐</span>",
        get_album_file_count(),
        screensaver_theme == 1 ? "黑客帝国数字雨" : "无",
        st_pct, (int)((long long)st_total_kb * st_pct / 100), st_total_kb, (int)((long long)st_total_kb * st_free_pct / 100),
        st_pct, st_free_pct, st_pct, st_pct, st_pct,
        st_pct, st_free_pct
    );
    httpd_resp_sendstr_chunk(req, chunk);

    // ====== 标签页2: 设置 ======
    // ★ 这两个缓冲原先在 httpd 任务栈上，与下面的 wifi_list_html / fnames 等
    //   合计数 KB，而 httpd 任务栈只有 8KB，已逼近极限。栈溢出不会立刻 panic，
    //   只会在栈尾静默破坏相邻内存（表现为随机的响应异常），因此全部移入静态区。
    //   注意 static 不会自动清零，每次进入必须手动置首字节为 0。
    //   容量同时扩大：5 个 WiFi 项每项最坏 347 字节（两处 SSID 回显 32+32）
    //   × 5 项 + 结束符 = 1736 字节，原 1024 必被 strncat 截断导致列表显示不全，故扩到 2048。
    static char wifi_list_html[2048];
    // tmp 由 WiFi 项与相册项共用：相册项模板固定文本约 490B + 6 处文件名（fnames 每项最多 63B），
    // 最坏约 868B。原来只有 512B，会触发 ESP-IDF 默认开启的 -Werror=format-truncation 编译失败。
    static char tmp[1024];
    wifi_list_html[0] = 0;
    for (int i = 0; i < saved_wifi_count; i++) {
        snprintf(tmp, sizeof(tmp),
            "<div class='wifi-item'>"
            "<span class='wifi-name'>%s %s</span>"
            "<div>"
            "<button class='mini' onclick='location.href=\"/wifi_connect?idx=%d\"'>连接</button> "
            "<button class='mini danger' onclick='if(confirmDel(\"确定删除WiFi: %s?\"))location.href=\"/wifi_delete?idx=%d\"'>删除</button>"
            "</div></div>",
            saved_ssid_list[i], (i == current_wifi_idx) ? "🔗" : "", i, saved_ssid_list[i], i);
        strncat(wifi_list_html, tmp, sizeof(wifi_list_html) - strlen(wifi_list_html) - 1);
    }
    if (saved_wifi_count == 0) {
        strcpy(wifi_list_html, "<p style='color:#060;text-align:center;padding:10px'>暂无保存的WiFi网络</p>");
    }

    snprintf(chunk, sizeof(chunk),
        "<div id='tab-settings' class='tab-content'>"
        // WiFi设置
        "<div class='card'><div class='card-title'>📶 WiFi设置</div>"
        "<div class='form-group'>"
        "<label>已保存的WiFi网络</label>"
        "%s"
        "</div>"
        "<hr>"
        "<form action='/save_wifi' method='POST'>"
        "<div class='form-group'><label>WiFi名称 (SSID)</label>"
        "<input type='text' name='ssid' placeholder='输入路由器SSID' required></div>"
        "<div class='form-group'><label>WiFi密码</label>"
        "<input type='password' name='pass' placeholder='输入WiFi密码'></div>"
        "<button type='submit'>➕ 添加并连接新WiFi</button>"
        "</form>"
        "</div>"
        // ★ 修复：整页「设置」标签只允许存在【一个】/save_settings 表单。
        //   历史版本这里曾有第二个 <form action='/save_settings'>（NTP 单独一个）与下方
        //   统一表单并存：两者 action 相同但字段不同——NTP 表单里只有 ntp，统一表单里有
        //   saver_theme/saver_timeout/sleep_key/album_interval。这会造成两类问题：
        //     1) 用户在 NTP 输入框里按回车 → 浏览器提交的是 NTP 那个表单，请求体只有 ntp=，
        //        服务端两侧取值来源不一致，saver_theme/timeout/album_interval 会被默认值覆盖；
        //     2) 点"💾 保存所有设置"提交的是统一表单，NTP 地址不在其中 → 改了 NTP 也不生效。
        //   即"同一个设置页、两套提交路径、各丢一半字段"，是熄屏按键等设置时灵时不灵的祸根。
        //   现做法：全标签页只有一个 /save_settings 表单，从 NTP 卡片起、依次包住
        //   NTP / 屏保 / 熄屏按键 / 功能菜单按键 / 相册 五张卡片，保证"保存所有设置"一次性提交全部字段。
        //   注意：开机音乐卡片自带独立表单，必须放在统一表单【之外】，否则会截断统一表单。
        "<form action='/save_settings' method='POST' onsubmit='return checkKeyConflict(this)'>"
        // NTP设置
        "<div class='card'><div class='card-title'>🕐 NTP时间服务器</div>"
        "<div class='form-group'><label>NTP服务器地址</label>"
        "<input type='text' name='ntp' value='%s'>"
        "<div class='hint'>推荐: ntp.ntsc.ac.cn (国家授时中心)</div></div>"
        "<div class='form-group'><label>自动同步频率</label>"
        "<select name='ntp_sync'>"
        "<option value='0'%s>每小时整点</option>"
        "<option value='1'%s>每天零时</option>"
        "</select>"
        "<div class='hint'>按选定频率自动与 NTP 服务器对时；默认「每小时」。</div></div>"
        "<div class='form-group'><label style='display:flex;align-items:center;gap:10px;justify-content:space-between'>"
        "<span>整点报时<span class='hint' style='font-weight:normal;margin-left:6px'>每到整点通过喇叭播放报时音</span></span>"
        "<label class='switch-wrap'><input type='checkbox' name='chime_en' value='1' %s onchange='onChimeToggle(this)'><span class='switch-btn'></span></label>"
        "</label></div>"
        "<div class='form-group' id='chime_sil_group'><label>静默时间段（此时段内不播报）</label>"
        "<div style='display:flex;gap:8px;align-items:center'>"
        "<select name='chime_s' style='flex:1'>"
        "<option value='22' selected>22</option>"
        "</select>"
        "<span style='color:#0a0;font-size:16px'>点 ~</span>"
        "<select name='chime_e' style='flex:1'>"
        "<option value='7' selected>7</option>"
        "</select>"
        "<span style='color:#0a0;font-size:16px'>点</span>"
        "</div>"
        "<div class='hint'>默认 22:00 ~ 次日 7:00（22点整~7点整都不响，8点起恢复）。整点报时关闭时该设置无效。</div>"
        "<script>(function(){"
        "function fill(id,sel){var el=document.querySelector('select[name='+id+']');"
        "if(!el)return;el.innerHTML='';"
        "for(var i=0;i<24;i++){var o=document.createElement('option');o.value=i;o.textContent=i+' 点整';"
        "if(i==sel)o.selected=true;el.appendChild(o);}"
        "}"
        "fill('chime_s',%d);fill('chime_e',%d);"
        "})();</script>"
        "</div>"
        "</div>"
        // 屏保设置
        "<div class='card'><div class='card-title'>🖥️ 屏保设置</div>"
        "<div class='form-group'><label>屏保主题</label>"
        "<select name='saver_theme'>"
        "<option value='0' %s>无屏保</option>"
        "<option value='1' %s>黑客帝国数字雨</option>"
        "</select></div>"
        "<div class='form-group'><label>无操作进入屏保时间 (秒)</label>"
        "<input type='number' name='saver_timeout' value='%d' min='10' max='300'>"
        "<div class='hint'>默认30秒无按键自动进入屏保</div></div>"
        "</div>"
        // 熄屏按键设置
        "<div class='card'><div class='card-title'>🌙 熄屏按键</div>"
        "<div class='form-group'><label>长按触发熄屏的按键</label>"
        "<select name='sleep_key'>"
        "<option value='0' %s>UP键</option>"
        "<option value='1' %s>DOWN键</option>"
        "<option value='2' %s>OK键</option>"
        "</select>"
        "<div class='hint'>长按所选按键即可关闭屏幕（熄屏）；屏幕熄灭后，短按任意按键即可点亮屏幕。</div></div>"
        "</div>"
        // 功能菜单按键设置（与熄屏按键互斥，保存时做冲突检查）
        "<div class='card'><div class='card-title'>📋 功能菜单按键</div>"
        "<div class='form-group'><label>短按唤出功能菜单的按键</label>"
        "<select name='menu_key'>"
        "<option value='0' %s>UP键</option>"
        "<option value='1' %s>DOWN键</option>"
        "<option value='2' %s>OK键</option>"
        "</select>"
        "<div class='hint'>短按所选按键一次，屏幕中央弹出功能菜单（个人名片 / 相册 / 天气预报 / 手电筒 / 取消）。"
        "菜单内用 UP键、DOWN键 移动选择，OK键 确认。默认：OK键。</div>"
        "<div class='hint'>关闭菜单的方式：把选择移到最下面一项「取消」，再按 OK键 确认。</div>"
        "<div class='hint' style='color:#f80'>⚠️ 不能与「熄屏按键」选同一个键，否则保存时会提示冲突。</div></div>"
        "</div>"
        // 相册设置
        "<div class='card'><div class='card-title'>🖼️ 相册设置</div>"
        "<div class='form-group'><label>自动播放间隔 (秒)</label>"
        "<input type='number' name='album_interval' value='%d' min='3' max='60'>"
        "<div class='hint'>默认每10秒切换一张图片</div></div>"
        "<button type='submit'>💾 保存所有设置</button>"
        "</div>"
        "</form>"
        // 开机音乐（自带独立表单，放在统一表单之外）
        "<div class='card'><div class='card-title'>🎵 开机音乐 (MP3)</div>"
        "<div class='hint'>支持MP3格式，最大256KB，开机自动播放</div>"
        "%s"
        "<form action='/upload_music' method='POST' enctype='multipart/form-data' style='margin-top:8px'>"
        "<input type='file' name='music' accept='audio/mpeg,.mp3' onchange='checkUploadSize(this,256)'>"
        "<button type='submit'>上传开机音乐</button>"
        "</form>"
        "<form action='/delete_music' method='POST' style='margin-top:5px'>"
        "<button type='submit' class='danger'>删除自定义音乐，恢复默认</button>"
        "</form>"
        "</div>"
        "</div>",
        wifi_list_html, ntp_server_addr,
        (ntp_sync_mode == SYNC_MODE_HOURLY) ? "selected" : "",
        (ntp_sync_mode == SYNC_MODE_DAILY)  ? "selected" : "",
        chime_enabled ? "checked" : "",
        chime_silent_start, chime_silent_end,
        screensaver_theme == 0 ? "selected" : "",
        screensaver_theme == 1 ? "selected" : "",
        saver_timeout_sec,
        (sleep_key == SLEEP_KEY_UP) ? "selected" : "",
        (sleep_key == SLEEP_KEY_DOWN) ? "selected" : "",
        (sleep_key == SLEEP_KEY_OK) ? "selected" : "",
        (menu_key == SLEEP_KEY_UP) ? "selected" : "",
        (menu_key == SLEEP_KEY_DOWN) ? "selected" : "",
        (menu_key == SLEEP_KEY_OK) ? "selected" : "",
        album_interval_sec,
        boot_music_exists ? "<div class='status-badge ok'>当前已上传自定义开机音乐</div>" : "<div class='status-badge err'>使用默认开机音乐</div>"
    );
    httpd_resp_sendstr_chunk(req, chunk);

    // ====== 标签页3: 个人信息 ======
    // NFC 内容需转义后再回显到 value 属性，避免含 < & " 时破坏页面结构
    static char nfc_esc[NFC_CONTENT_MAX * 6 + 8];
    html_attr_escape(nfc_content, nfc_esc, sizeof(nfc_esc));
    snprintf(chunk, sizeof(chunk),
        "<div id='tab-profile' class='tab-content'>"
        "<div class='card'><div class='card-title'>👤 设备显示名称</div>"
        "<form action='/save_name' method='POST'>"
        "<div class='form-group'><label>显示名称（将在主界面显示）</label>"
        "<input type='text' name='name' value='%s' maxlength='16' required>"
        "<button type='submit'>💾 保存名称</button>"
        "</form>"
        "</div>"
        "<div class='card'><div class='card-title'>📇 NFC 标签内容</div>"
        "<form action='/save_nfc' method='POST'>"
        "<div class='form-group'><label>自定义 NFC 标签内容</label>"
        "<input type='text' name='nfc' value='%s' maxlength='%d' placeholder='例如网址或文本，最长 %d 字节'>"
        "<div class='hint'>写入的文本会被作为 NFC 标签（NDEF 文本记录）内容使用，最多 %d 字节</div></div>"
        "<button type='submit'>💾 保存 NFC 内容</button>"
        "</form>"
        "<form action='/clear_nfc' method='POST' style='margin-top:5px'>"
        "<button type='submit' class='danger'>清空 NFC 内容</button>"
        "</form>"
        "</div>"
        "<div class='card'><div class='card-title'>🖼️ 头像设置</div>"
        "<div class='hint'>最大分辨率: 90×90像素，用于个人信息展示</div>"
        "%s"
        "<form action='/upload_avatar' method='POST' enctype='multipart/form-data' style='margin-top:10px'>"
        "<input type='file' name='avatar' accept='image/*' onchange='checkUploadSize(this, 32)'>"
        "<button type='submit'>上传头像</button>"
        "</form>"
        "<form action='/delete_avatar' method='POST' style='margin-top:5px'>"
        "<button type='submit' class='danger'>删除头像</button>"
        "</form>"
        "</div>"
        "</div>"
        // ★ 修复：原版本本页只有 9 个 <div> 却只闭合 8 个，缺一个 </div>。
        //   当时本页是最后一个标签页，浏览器把未闭合的 div 一直吞到 </body>，视觉上无异常；
        //   一旦在本页之后新增标签页（如"相册""抖音弹幕"），它们会被嵌套进本页内部，
        //   切换过去时父级仍是 display:none，整页表现为空白。此处补上缺失的闭合标签。
        "</div>",
        current_custom_name,
        nfc_esc, NFC_CONTENT_MAX - 1, NFC_CONTENT_MAX - 1, NFC_CONTENT_MAX - 1,
        avatar_exists ? "<div class='status-badge ok'>✅ 已上传自定义头像</div>" : "<div class='status-badge err'>❌ 未上传头像，使用默认</div>"
    );
    httpd_resp_sendstr_chunk(req, chunk);

    // ====== 标签页5: 抖音弹幕 ======
    {
        static char dk_url_esc[DANMAKU_URL_MAX * 6 + 8];
        html_attr_escape(danmaku_get_url(), dk_url_esc, sizeof(dk_url_esc));
        snprintf(chunk, sizeof(chunk),
            "<div id='tab-danmaku' class='tab-content'>"
            "<div class='card'><div class='card-title'>🔌 弹幕接收总开关</div>"
            "<form action='/danmaku_switch' method='POST'>"
            "<div class='row'>"
            "<span class='label'>开启弹幕接收服务</span>"
            "<span class='value'>%s</span>"
            "</div>"
            "<div class='hint'>默认关闭。关闭时不连接服务器、不产生任何网络请求；打开后才会按下面的地址建立连接。</div>"
            "<input type='hidden' name='on' value='%d'>"
            "<button type='submit' %s>%s</button>"
            "</form>"
            "<form action='/danmaku_save' method='POST'>"
            "<div class='form-group' style='margin-top:10px'><label>WebSocket 服务器地址</label>"
            "<input type='text' name='url' value='%s' placeholder='ws://192.168.1.100:8080/danmaku'>"
            "<div class='hint'>支持 ws:// 与 wss:// 两种协议；总开关打开后点此保存即开始接收。留空保存则停止接收。</div>"
            "</div>"
            "<button type='submit'>💾 保存地址</button>"
            "</form>"
            "<form action='/danmaku_disconnect' method='POST' style='margin-top:5px'>"
            "<button type='submit' class='danger'>⛔ 断开当前连接</button>"
            "</form>"
            "</div>"
            "<div class='card'><div class='card-title'>📡 连接状态</div>"
            "<div class='row'><span class='label'>当前状态</span><span class='value' id='dk_state'>%s</span></div>"
            "<div class='row'><span class='label'>已接收弹幕</span><span class='value' id='dk_total'>%u</span></div>"
            "</div>"
            "<div class='card'><div class='card-title'>💬 最近收到的弹幕</div>"
            "<div id='dk_list'><div class='hint'>正在读取...</div></div>"
            "<div class='hint' style='margin-top:8px'>列表每 3 秒自动刷新，最多显示最近 8 条</div>"
            "</div>"
            "</div>",
            danmaku_get_enabled()
                ? "<span class='status-badge ok'>● 已开启</span>"
                : "<span class='status-badge err'>○ 已关闭</span>",
            danmaku_get_enabled() ? 0 : 1,
            danmaku_get_enabled() ? "" : "disabled",
            danmaku_get_enabled() ? "⏹ 关闭弹幕服务" : "▶ 开启弹幕服务（地址为空则暂不连接）",
            dk_url_esc, danmaku_get_state_str(), danmaku_get_total());
        httpd_resp_sendstr_chunk(req, chunk);
    }

    // ====== 标签页4: 相册 ======
    int album_cnt = get_album_file_count();
    // 文件名列表在这里取好，实际 HTML 在下面逐项直发（见 album-grid 发送处）
    static char fnames[30][64];
    int album_list_cnt = 0;
    if (album_cnt > 0) get_album_filenames(fnames, &album_list_cnt);

    snprintf(chunk, sizeof(chunk),
        "<div id='tab-album' class='tab-content'>"
        "<div class='card'><div class='card-title'>🖼️ 相册管理</div>"
        "<div class='progress-bar'><div class='progress-fill' style='width:%d%%'></div></div>"
        "<p style='text-align:center'>已存 %d / 30 张图片（%d%%）</p>"
        "<hr>"
        "<div style='margin-bottom:6px;font-weight:bold;color:#0f0'>📤 上传照片</div>"
        "<label>选择图片（最大分辨率240×320）</label>"
        "<div class='hint'>图片会在浏览器端自动等比缩放并压缩为 JPEG 后上传（约 15~25 KB / 张）</div>"
        "<form onsubmit='return uploadAlbum(event)'>"
        "<input type='file' id='albumFile' name='photo' accept='image/*'>"
        "<button type='submit'>📤 上传到相册</button>"
        "<div class='hint' id='albumStatus'></div>"
        "</form>"
        "<hr>"
        "<div style='margin-bottom:6px;font-weight:bold;color:#0f0'>已上传的照片（%d 张）</div>"
        "<div class='hint' style='margin-bottom:6px'>点击缩略图或「查看」可在新窗口打开原图；点击「删除」移除该张。</div>",
        album_cnt * 100 / MAX_ALBUM_IMAGES, album_cnt, album_cnt * 100 / MAX_ALBUM_IMAGES,
        album_cnt
    );
    httpd_resp_sendstr_chunk(req, chunk);
    // 相册列表：逐项单独发送（每项最坏约 868B），不再预先拼进一个大缓冲。
    // 原实现用 6144B 缓冲装 30 项，但新增缩略图后每项约 556B（文件名形如 img_001.jpg），
    // 30 项约需 16.7KB，缓冲必被 strncat 截断、后半部分照片不显示；逐项直发同时省下约 6KB 静态内存。
    if (album_list_cnt > 0) {
        httpd_resp_sendstr_chunk(req, "<div class='album-grid'>");
        for (int i = 0; i < album_list_cnt; i++) {
            // 每张已上传图片都显示缩略图（原图只有 240x320 以内、约 15~25KB，可直接展示），
            // 并在下方提供独立的"查看 / 删除"按钮。
            snprintf(tmp, sizeof(tmp),
                "<div class='album-item'>"
                "<a href='http://192.168.4.1/album/%s' target='_blank' title='查看原图'>"
                "<img src='/album/%s' alt='%s' style='width:100%%;height:70px;object-fit:cover;border-radius:3px;display:block;background:#000'>"
                "</a>"
                "<div class='album-name'>%s</div>"
                "<div class='album-btns'>"
                "<a class='mini-btn' href='http://192.168.4.1/album/%s' target='_blank'>查看</a>"
                "<a class='mini-btn danger' href='/album_delete?name=%s' onclick='return confirmDel(\"确定删除这张图片?\")'>删除</a>"
                "</div>"
                "</div>",
                fnames[i], fnames[i], fnames[i], fnames[i], fnames[i], fnames[i]);
            httpd_resp_sendstr_chunk(req, tmp);
        }
        httpd_resp_sendstr_chunk(req, "</div>");
    } else {
        httpd_resp_sendstr_chunk(req, "<p style='color:#060;text-align:center;padding:20px'>相册为空，请上传图片</p>");
    }
    snprintf(chunk, sizeof(chunk),
        "<form action='/album_delete_all' method='POST' style='margin:10px 0 0 0'>"
        "<button type='submit' class='danger' onclick='return confirmDel(\"确定批量删除所有相册图片?此操作不可恢复!\")'>🗑️ 批量删除全部</button>"
        "</form>"
        "<div class='hint' style='margin-top:10px'>💡 提示: 设备将按上传顺序自动播放相册图片，间隔可在「设置」页调整</div>"
        "</div>"
        "</div>"
        "</body></html>"
    );
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req, NULL);
    ESP_LOGI(TAG, "HTTP GET / → 首页响应发送完成（空闲 %d KB）", heap_caps_get_free_size(MALLOC_CAP_8BIT)/1024);
    return ESP_OK;
}

// ---------------------------------------------------------
// URL解码（统一使用带长度保护的版本，避免超长参数越界写栈）
// ---------------------------------------------------------
static void url_decode_bounded(char* dst, size_t dst_len, const char* src) {
    size_t o = 0;
    char a, b;
    if (dst_len == 0) return;
    while (*src && o + 1 < dst_len) {
        if ((*src == '%') && (a = src[1]) && (b = src[2]) &&
            isxdigit((unsigned char)a) && isxdigit((unsigned char)b)) {
            a = (a >= 'a') ? a - 32 : a; a = (a >= 'A') ? a - '7' : a - '0';
            b = (b >= 'a') ? b - 32 : b; b = (b >= 'A') ? b - '7' : b - '0';
            dst[o++] = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            dst[o++] = ' '; src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = 0;
}

// 通用成功返回页面
static void send_ok_page(httpd_req_t* req, const char* msg, const char* extra) {
    // ★ 修复：改 static。原为 2048B 栈上缓冲，而 httpd 任务栈仅 4096B，
    //   在保存 WiFi（extra 较长）等场景叠加 httpd 调用链会触发栈溢出 panic→真机重启。
    //   静态部分 952B + 最坏 msg/extra 384B ≈ 1336B；原 1024 还会静默截断页面尾部的返回按钮。
    //   v24.3: body 字号提到 26px、标题 42px、按钮 28px、提示 20px，手机小屏不用放大就能看清；
    //   补 user-scalable=no 避免被移动端浏览器自动缩小；顶部 padding 从 30 减到 30 避免首屏被压扁。
    static char buf[2048];
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<meta http-equiv='refresh' content='5;url=/'>"
        "<style>*{box-sizing:border-box;margin:0;padding:0}"
        "html,body{min-height:100%%}"
        "body{background:#0a0a0a;color:#0f0;text-align:center;padding:30px 18px;"
        "font-family:'Consolas','Courier New',monospace;font-size:26px;line-height:1.7;-webkit-text-size-adjust:100%%}"
        "h2{font-size:42px;margin-bottom:22px;text-shadow:0 0 10px #0f0;word-break:break-word}"
        ".btn{display:inline-block;margin-top:28px;padding:18px 36px;background:#0f0;color:#000;"
        "font-weight:bold;font-size:28px;text-decoration:none;border-radius:10px;"
        "box-shadow:0 0 15px #0f0;transition:all .2s}"
        ".btn:hover{background:#0c0;box-shadow:0 0 25px #0f0}"
        ".tip{margin-top:20px;color:#060;font-size:20px;line-height:1.7}"
        ".body-text{max-width:540px;margin:0 auto;line-height:1.8;font-size:26px;word-break:break-word}"
        "</style></head><body>"
        "<h2>✅ %s</h2>"
        "<div class='body-text'>%s</div>"
        "<div class='tip'>⏳ 设备可能需要 3~5 秒完成网络切换，请稍候...<br>5 秒后将自动返回管理页面</div>"
        "<a href='/' class='btn'>🔙 立即手动返回首页</a>"
        "</body></html>",
        msg, extra ? extra : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// ★ 错误页（带补充说明版）：msg 是标题，extra 是可选的正文补充 HTML（可为 NULL）。
//   新增原因：按键冲突等场景需要在错误页里把"为什么没保存、当前值是什么"讲清楚，
//   原来的 send_err_page 只能给一句标题，用户看不到细节。
static void send_err_page_ex(httpd_req_t* req, const char* msg, const char* extra);
static void send_err_page(httpd_req_t* req, const char* msg) {
    send_err_page_ex(req, msg, NULL);
}

static void send_err_page_ex(httpd_req_t* req, const char* msg, const char* extra) {
    // ★ 修复：改 static（原 1024B 栈上缓冲），理由同 send_ok_page，避免 httpd 栈溢出。
    //   冲突提示的 extra 最长约 256B，2048 缓冲对更大字号/更宽按钮留有充足余量。
    //   v24.3: 同 send_ok_page 增大到 26/42/28/20px，并加禁止缩放 meta，移动端不再被自动缩小。
    static char buf[2048];
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<meta http-equiv='refresh' content='5;url=/'>"
        "<style>*{box-sizing:border-box;margin:0;padding:0}"
        "html,body{min-height:100%%}"
        "body{background:#0a0a0a;color:#f00;text-align:center;padding:30px 18px;"
        "font-family:'Consolas','Courier New',monospace;font-size:26px;line-height:1.7;-webkit-text-size-adjust:100%%}"
        "h2{font-size:42px;margin-bottom:22px;text-shadow:0 0 10px #f00;word-break:break-word}"
        "p{color:#fa0;font-size:26px;line-height:1.8;margin:8px 0;word-break:break-word}"
        "b{color:#ff0}"
        ".btn{display:inline-block;margin-top:28px;padding:18px 36px;background:#f00;color:#000;"
        "font-weight:bold;font-size:28px;text-decoration:none;border-radius:10px;"
        "box-shadow:0 0 15px #f00}"
        ".tip{margin-top:20px;color:#600;font-size:20px;line-height:1.7}"
        "</style></head><body>"
        "<h2>❌ %s</h2>"
        "<div style='max-width:540px;margin:0 auto;word-break:break-word'>%s</div>"
        "<div class='tip'>5 秒后将自动返回管理页面</div>"
        "<a href='/' class='btn'>🔙 返回首页</a>"
        "</body></html>", msg, extra ? extra : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------
// POST: 保存通用设置（名称+NTP+屏保+熄屏按键+功能菜单按键+相册间隔）
// ---------------------------------------------------------
static esp_err_t save_settings_handler(httpd_req_t *req) {
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret <= 0) return ESP_FAIL;
    char ntp[64]={0}, name[32]={0};
    int theme = screensaver_theme, timeout = saver_timeout_sec, interval = album_interval_sec;
    int skey = sleep_key;
    int mkey = menu_key;
    int nmode = ntp_sync_mode;
    int cen = chime_enabled ? 1 : 0;
    int cs = chime_silent_start, ce = chime_silent_end;
    char *p = strtok(buf, "&");
    while (p) {
        if (strncmp(p,"ntp=",4)==0) url_decode_bounded(ntp, sizeof(ntp), p+4);
        else if (strncmp(p,"name=",5)==0) url_decode_bounded(name, sizeof(name), p+5);
        else if (strncmp(p,"saver_theme=",12)==0) theme = atoi(p+12);
        else if (strncmp(p,"saver_timeout=",14)==0) timeout = atoi(p+14);
        else if (strncmp(p,"album_interval=",15)==0) interval = atoi(p+15);
        else if (strncmp(p,"sleep_key=",10)==0) skey = atoi(p+10);
        // ★ 注意：menu_key 必须放在 sleep_key 之后判断——strncmp 前缀不同（menu_ / sleep_），
        //   这里顺序其实无关，但仍显式写明，防止后续有人误改成 startsWith 之类的匹配方式。
        //   另外 "saver_timeout" 以 "saver_" 开头，不能写成前缀 "saver" 匹配，否则会误吞。
        else if (strncmp(p,"menu_key=",9)==0) mkey = atoi(p+9);
        else if (strncmp(p,"ntp_sync=",9)==0) nmode = atoi(p+9);
        else if (strncmp(p,"chime_en=",9)==0) cen = atoi(p+9);
        else if (strncmp(p,"chime_s=",8)==0) cs = atoi(p+8);
        else if (strncmp(p,"chime_e=",8)==0) ce = atoi(p+8);
        p = strtok(NULL, "&");
    }
    if (timeout < 10) timeout = 10;
    if (interval < 3) interval = 3;
    if (skey < SLEEP_KEY_UP || skey > SLEEP_KEY_OK) skey = SLEEP_KEY_UP;   // 越界回落到默认"UP键"
    if (mkey < SLEEP_KEY_UP || mkey > SLEEP_KEY_OK) mkey = SLEEP_KEY_OK;   // 越界回落到默认"OK键"
    if (nmode != SYNC_MODE_HOURLY && nmode != SYNC_MODE_DAILY) nmode = SYNC_MODE_HOURLY;
    bool cen_bool = (cen != 0);
    if (cs < 0 || cs > 23) cs = CHIME_SILENT_DEFAULT_START;
    if (ce < 0 || ce > 23) ce = CHIME_SILENT_DEFAULT_END;

    // ★★ 冲突性检查（用户需求 1、2）：熄屏按键 与 功能菜单按键 不允许选同一个键。
    //   两端都查：网页端用 JS 先拦一道（体验好、不用等一个来回），服务端这里再兜底一道
    //   （防止直接构造 POST、或浏览器 JS 被禁用/改坏时写入冲突配置）。
    if (skey == mkey) {
        // ★ 该提示的固定 HTML 文案本身就接近 300 字节，再叠加 3 个 %s，
        //   ESP-IDF 默认开启的 -Werror=format-truncation 会在编译期直接判为错误
        //   （报错原文："directive output truncated writing 31 bytes into a region of size 3"）。
        //   两点一起改才稳：
        //     1) 缓冲从 256 扩到 512，给足空间；
        //     2) 每个 %s 加精度上限 %.16s，让编译器能算出确定的最大长度，
        //        不再因"长度不可知"而误判截断（按键名最长 "DOWN键" 仅 7 字节，16 足够）。
        static char extra[512];
        snprintf(extra, sizeof(extra),
                 "<p style='color:#f80'>你为「熄屏按键」和「功能菜单按键」选择了同一个按键：<b>%.16s</b>。</p>"
                 "<p>两个功能不能绑在同一个按键上，请返回改成不同的按键再保存。</p>"
                 "<p>当前生效值仍为：熄屏按键 = <b>%.16s</b>，功能菜单按键 = <b>%.16s</b>。</p>",
                 get_sleep_key_name(skey),
                 get_sleep_key_name(sleep_key), get_sleep_key_name(menu_key));
        send_err_page_ex(req, "按键设置冲突，未保存", extra);
        ESP_LOGW(TAG, "保存被拒绝：熄屏按键与功能菜单按键冲突（同为 %s）", get_sleep_key_name(skey));
        return ESP_OK;
    }

    bool skey_changed = (sleep_key != skey);
    bool mkey_changed = (menu_key  != mkey);
    bool nmode_changed = (ntp_sync_mode != nmode);
    bool ch_en_changed  = (chime_enabled != cen_bool);
    bool ch_sil_changed = (chime_silent_start != cs) || (chime_silent_end != ce);
    sleep_key = skey;
    menu_key  = mkey;
    ntp_sync_mode = nmode;
    chime_enabled = cen_bool;
    chime_silent_start = cs;
    chime_silent_end   = ce;
    // ★ 关键：立即把新按键同步给按键任务（gui_main.c 自持一份运行时值）。
    //   按键任务每次判定都现取，所以这里设完立刻生效，无需重启。
    gui_key_set_sleep_key(skey);
    gui_key_set_menu_key(mkey);
    if (skey_changed) ESP_LOGI(TAG, "熄屏按键已改为 %s", get_sleep_key_name(skey));
    if (mkey_changed) ESP_LOGI(TAG, "功能菜单按键已改为 %s", get_sleep_key_name(mkey));
    if (nmode_changed) ESP_LOGI(TAG, "NTP 自动同步频率已改为 %s", get_ntp_sync_mode_name(nmode));
    if (ch_en_changed) ESP_LOGI(TAG, "整点报时已%s", chime_enabled ? "开启" : "关闭");
    if (ch_sil_changed) ESP_LOGI(TAG, "整点报时静默时段已设为 %d 点 ~ %d 点", cs, ce);
    save_config_to_nvs((strlen(name)>0)?name:NULL, (strlen(ntp)>0)?ntp:NULL, theme, timeout, interval);
    // 把 NTP 频率/整点报时/静默时段单独写回 NVS（save_config_to_nvs 只管原来几个键）
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, NVS_KEY_NTP_SYNC, ntp_sync_mode);
            nvs_set_i8 (h, NVS_KEY_CHIME_EN, chime_enabled ? 1 : 0);
            nvs_set_i32(h, NVS_KEY_CHIME_SIL_S, chime_silent_start);
            nvs_set_i32(h, NVS_KEY_CHIME_SIL_E, chime_silent_end);
            nvs_commit(h); nvs_close(h);
        }
    }
    if (strlen(ntp) > 0) sync_time_manual();
    {
        // 回显实际生效值，便于用户从返回页直接确认设置已被服务端接受
        static char extra[512];
        snprintf(extra, sizeof(extra),
                 "<p>熄屏按键：<b>%s</b></p><p>功能菜单按键：<b>%s</b></p>"
                 "<p>NTP服务器：<b>%s</b></p>"
                 "<p>自动同步频率：<b>%s</b></p>"
                 "<p>整点报时：<b>%s</b>（静默 %d 点 ~ %d 点）</p>"
                 "<p>屏保主题：%s</p><p>相册间隔：%d 秒</p>",
                 get_sleep_key_name(sleep_key), get_sleep_key_name(menu_key),
                 ntp_server_addr,
                 get_ntp_sync_mode_name(ntp_sync_mode),
                 chime_enabled ? "开启" : "关闭", chime_silent_start, chime_silent_end,
                 (theme == 1) ? "黑客帝国数字雨" : "无屏保", interval);
        send_ok_page(req, "设置保存成功", extra);
    }
    return ESP_OK;
}

static esp_err_t save_name_handler(httpd_req_t *req) {
    char buf[128] = {0};
    httpd_req_recv(req, buf, sizeof(buf)-1);
    char name[32] = {0};
    if (strncmp(buf,"name=",5)==0) url_decode_bounded(name, sizeof(name), buf+5);
    save_config_to_nvs(name, NULL, screensaver_theme, saver_timeout_sec, album_interval_sec);
    send_ok_page(req, "显示名称已更新", "");
    return ESP_OK;
}

// ---------------------------------------------------------
// POST: 保存 / 清空 NFC 标签内容（个人信息页）
// ---------------------------------------------------------
static esp_err_t save_nfc_handler(httpd_req_t *req) {
    char buf[256] = {0};
    int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (r <= 0) { send_err_page(req, "读取请求失败"); return ESP_OK; }
    buf[r] = 0;

    char nfc[NFC_CONTENT_MAX] = {0};
    char* p = strtok(buf, "&");
    while (p) {
        if (strncmp(p, "nfc=", 4) == 0) url_decode_bounded(nfc, sizeof(nfc), p + 4);
        p = strtok(NULL, "&");
    }
    if (strlen(nfc) >= NFC_CONTENT_MAX) {
        send_err_page(req, "NFC 内容过长");
        return ESP_OK;
    }
    save_nfc_content_to_nvs(nfc);
    send_ok_page(req, "NFC 内容已保存", nfc[0] ? "" : "<p>（当前为空）</p>");
    return ESP_OK;
}

static esp_err_t clear_nfc_handler(httpd_req_t *req) {
    httpd_req_recv(req, NULL, 0);
    save_nfc_content_to_nvs("");
    send_ok_page(req, "NFC 内容已清空", "");
    return ESP_OK;
}

// ---------------------------------------------------------
// POST: 添加新WiFi
// ---------------------------------------------------------

// ★ 延迟切网任务：等 HTTP 响应彻底发出（连接关闭）后再切换 STA。
//   直接在 handler 里 delay 后调用 esp_wifi_connect() 的问题：
//   1) 切网瞬间 AP 信道随 STA 变化、TCP 连接被物理切断，浏览器常收不全响应
//      → 表现为"卡在保存页、看不到手动返回按钮"；
//   2) esp_wifi_connect() 内部会阻塞较久，在 httpd 任务上下文里执行有风险。
//   改成独立任务 + 更长的等待时间，让 httpd 有机会把整个响应体刷出 socket。
static void wifi_switch_task(void *arg) {
    wifi_switch_param_t *p = (wifi_switch_param_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(800));   // 给 httpd 充分时间把响应发完

    wifi_config_t cfg = {0};
    strlcpy((char*)cfg.sta.ssid, p->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, p->pass, sizeof(cfg.sta.password));
    ESP_LOGI(TAG, "切换 STA 到 %s", p->ssid);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    free(p);
    vTaskDelete(NULL);
}

static esp_err_t save_wifi_handler(httpd_req_t *req) {
    char buf[256] = {0};
    httpd_req_recv(req, buf, sizeof(buf)-1);
    char ssid[32]={0}, pass[64]={0};
    char *p = strtok(buf, "&");
    while (p) {
        if (strncmp(p,"ssid=",5)==0) url_decode_bounded(ssid, sizeof(ssid), p+5);
        else if (strncmp(p,"pass=",5)==0) url_decode_bounded(pass, sizeof(pass), p+5);
        p = strtok(NULL, "&");
    }
    if (strlen(ssid) == 0) {
        send_err_page(req, "SSID不能为空");
        return ESP_OK;
    }
    int idx = save_wifi_to_list(ssid, pass);
    (void)idx; // 避免未使用变量警告

    // 获取当前AP热点名称（与 web_config_init 保持一致：Passport + MAC后三位）
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid_name[64];
    snprintf(ap_ssid_name, sizeof(ap_ssid_name), "Passport%02X%02X%02X", mac[3], mac[4], mac[5]);

    // 关键修复：先把完整 HTTP 响应发给浏览器，再启动 WiFi 连接
    // 否则 esp_wifi_connect() 触发 AP+STA 切换时 TCP 连接会被物理切断，
    // 浏览器收不到响应 → 白屏/无反应，串口出现 EHOSTUNREACH(113)/ENOTCONN(128)。
    // ★ static：512B 放栈上叠加 send_ok_page 调用后本函数栈帧达 1232B，
    //   在偏紧的 httpd 栈下与 snprintf 叠加有溢出风险。httpd 单任务串行，static 安全。
    static char extra[512];
    snprintf(extra, sizeof(extra),
             "<p>已保存WiFi: <b>%s</b></p>"
             "<p style='margin-top:12px;color:#0a0'>📶 设备正在连接路由器，AP热点可能会因信道同步导致短暂断连（1~3秒），请稍候。<br>"
             "若手机自动断开热点，请重新连接 <b>%s</b> 后访问 <b>192.168.4.1</b>。</p>",
             ssid, ap_ssid_name);
    send_ok_page(req, "WiFi保存成功", extra);

    // 起独立任务延迟切网：先让 httpd 把上面这个"WiFi保存成功 + 手动返回按钮"页面
    // 完整发到手机，再切 STA，避免"卡在保存页、看不到返回按钮"。
    wifi_switch_param_t *wp = (wifi_switch_param_t *)malloc(sizeof(wifi_switch_param_t));
    if (wp) {
        memset(wp, 0, sizeof(*wp));
        strlcpy(wp->ssid, ssid, sizeof(wp->ssid));
        strlcpy(wp->pass, pass, sizeof(wp->pass));
        xTaskCreate(wifi_switch_task, "wifi_sw", 3072, wp, 5, NULL);
    } else {
        // 内存不足时退回原逻辑（极罕见）
        vTaskDelay(pdMS_TO_TICKS(300));
        wifi_config_t cfg = {0};
        strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
        strlcpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_connect();
    }
    return ESP_OK;
}

// ---------------------------------------------------------
// GET: 连接指定WiFi / 删除指定WiFi
// ---------------------------------------------------------
static esp_err_t wifi_connect_handler(httpd_req_t *req) {
    char* idx_str = strstr(req->uri, "?idx=");
    if (!idx_str) { send_err_page(req, "参数错误"); return ESP_OK; }
    int idx = atoi(idx_str+5);
    if (idx <0 || idx >= saved_wifi_count) { send_err_page(req, "WiFi不存在"); return ESP_OK; }
    char pass[64] = {0};
    load_password_by_idx(idx, pass, sizeof(pass));
    // 关键修复：先把响应发完，再启动 WiFi 连接
    // （与 save_wifi_handler 同理：避免 STA 切换时 TCP 连接被中途切断导致浏览器收不到响应）
    nvs_handle_t h;
    if (nvs_open(WIFI_LIST_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "current", idx);
        nvs_commit(h); nvs_close(h);
    }
    current_wifi_idx = idx;
    char extra[256];
    snprintf(extra, sizeof(extra),
             "<p>正在切换到WiFi: <b>%s</b></p>"
             "<p style='margin-top:12px;color:#0a0'>📶 设备正在切换网络，AP热点可能短暂断连（1~3秒），请稍候重新连接热点后访问。</p>",
             saved_ssid_list[idx]);
    send_ok_page(req, "切换WiFi", extra);

    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_config_t cfg = {0};
    strlcpy((char*)cfg.sta.ssid, saved_ssid_list[idx], sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    return ESP_OK;
}

static esp_err_t wifi_delete_handler(httpd_req_t *req) {
    char* idx_str = strstr(req->uri, "?idx=");
    if (!idx_str) { send_err_page(req, "参数错误"); return ESP_OK; }
    int idx = atoi(idx_str+5);
    if (delete_wifi_from_list(idx)) {
        send_ok_page(req, "WiFi已删除", "");
    } else {
        send_err_page(req, "删除失败");
    }
    return ESP_OK;
}

// ---------------------------------------------------------
// POST: 手动同步时间
// ---------------------------------------------------------
static esp_err_t sync_post_handler(httpd_req_t *req) {
    sync_time_manual();
    char extra[128];
    snprintf(extra, sizeof(extra), "<p>NTP服务器: %s</p>", ntp_server_addr);
    send_ok_page(req, "时间同步请求已发送", extra);
    return ESP_OK;
}

// ---------------------------------------------------------
// 通用文件上传处理框架（保存到指定路径，限制大小）
// ---------------------------------------------------------
typedef struct {
    const char* target_path;
    size_t max_size;
    const char* success_msg;
    const char* nvs_flag_key; // 上传成功后置1
} upload_ctx_t;

static esp_err_t generic_upload_handler(httpd_req_t *req, upload_ctx_t* ctx) {
    // ★ 必须 static：2048B 放在栈上时本函数栈帧达 2624B，叠加 httpd 框架与 snprintf
    //   会超出 httpd 任务栈导致栈溢出重启（见 HTTPD_TASK_STACK 处说明）。
    //   httpd 单任务串行处理请求，同一时刻只有一个请求在用该缓冲，static 安全。
    static char buf[HTTP_RECV_BUF_SIZE];
    char boundary[80];
    char content_type[128] = {0};
    // ESP-IDF v5.x 用官方API获取Content-Type请求头
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        !find_boundary(content_type, boundary, sizeof(boundary))) {
        send_err_page(req, "无法解析上传请求");
        return ESP_OK;
    }
    size_t total = (size_t)req->content_len;
    if (total > ctx->max_size + 1024) { // 留1KB给表单头
        send_err_page(req, "文件大小超过限制");
        return ESP_OK;
    }
    FILE* f = fopen(ctx->target_path, "wb");
    if (!f) { send_err_page(req, "无法创建文件"); return ESP_OK; }

    size_t received = 0;
    int file_started = 0;
    char header_search[256] = {0};
    int hs_pos = 0;
    size_t total_written = 0;

    while (received < total) {
        int r = httpd_req_recv(req, buf, MIN((int)(total - received), (int)sizeof(buf)));
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; fclose(f); remove(ctx->target_path); return ESP_FAIL; }
        received += r;
        if (!file_started) {
            // 搜索文件数据起始（\r\n\r\n之后）
            int i;
            for (i = 0; i < r; i++) {
                header_search[hs_pos++] = buf[i];
                if (hs_pos >= 4 && memcmp(header_search + hs_pos -4, "\r\n\r\n", 4) == 0) {
                    file_started = 1;
                    // 写入剩余数据（i+1 到结尾，去掉最后的boundary）
                    if (i+1 < r) {
                        int to_write = r - (i+1);
                        // 检查末尾是否有boundary（简单处理：末尾最后2字节可能是--或\r\n，这里直接写入，最后截断）
                        fwrite(buf + i + 1, 1, to_write, f);
                        total_written += to_write;
                    }
                    break;
                }
                if (hs_pos >= (int)sizeof(header_search)-1) hs_pos = 0;
            }
            if (!file_started) continue;
        } else {
            // 写入数据，最后需要去掉末尾的 \r\n--boundary--\r\n
            // 简单方案：写入全部，最后truncate掉末尾50字节（包含boundary标记）
            fwrite(buf, 1, r, f);
            total_written += r;
        }
    }
    // 截断末尾boundary（大约从尾部往前找--标记，简单截断最后60字节）
    fclose(f);
    // 重新打开裁剪掉末尾的boundary部分
    if (total_written > 80) {
        // truncate 在部分 libc 上带 warn_unused_result 属性，(void) 强制转换并不能
        // 抑制该告警（GCC 对 warn_unused_result 的特殊规则），必须真正使用返回值。
        // 截断失败只会残留末尾 boundary，记录告警即可，不中断上传流程，行为不变。
        int trc = truncate(ctx->target_path, total_written - 60);
        if (trc != 0) ESP_LOGW(TAG, "截断上传文件末尾 boundary 失败(%d)：%s", trc, ctx->target_path);
    }
    // 设置NVS标记
    if (ctx->nvs_flag_key) {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i8(h, ctx->nvs_flag_key, 1);
            nvs_commit(h); nvs_close(h);
        }
        if (strcmp(ctx->nvs_flag_key, NVS_KEY_AVATAR_SET) == 0) avatar_exists = true;
        if (strcmp(ctx->nvs_flag_key, NVS_KEY_BOOT_MUSIC_SET) == 0) boot_music_exists = true;
    }
    ESP_LOGI(TAG, "File saved: %s (%d bytes)", ctx->target_path, (int)total_written);
    send_ok_page(req, ctx->success_msg, "");
    return ESP_OK;
}

// ---------------------------------------------------------
// 各个上传端点
// ---------------------------------------------------------
static esp_err_t upload_avatar_handler(httpd_req_t *req) {
    upload_ctx_t ctx = {
        .target_path = STORAGE_MOUNT_POINT "/avatar/avatar.bin",
        .max_size = 32 * 1024,
        .success_msg = "头像上传成功",
        .nvs_flag_key = NVS_KEY_AVATAR_SET
    };
    return generic_upload_handler(req, &ctx);
}

static esp_err_t upload_music_handler(httpd_req_t *req) {
    upload_ctx_t ctx = {
        .target_path = STORAGE_MOUNT_POINT "/music/boot.mp3",
        .max_size = MAX_BOOT_MUSIC_SIZE,
        .success_msg = "开机音乐上传成功",
        .nvs_flag_key = NVS_KEY_BOOT_MUSIC_SET
    };
    return generic_upload_handler(req, &ctx);
}

// 扫描相册目录，返回一个未被占用的编号（文件名 img_003.jpg，共 11 字符，
// 远小于 CONFIG_SPIFFS_OBJ_NAME_LEN=32 的限制）
static int next_album_index(void) {
    int max_id = -1;
    DIR* d = opendir(STORAGE_MOUNT_POINT "/album");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            int id = -1;
            if (sscanf(e->d_name, "img_%d.jpg", &id) == 1 && id > max_id) max_id = id;
        }
        closedir(d);
    }
    return max_id + 1;
}

static esp_err_t upload_album_handler(httpd_req_t *req) {
    int cnt = get_album_file_count();
    if (cnt >= MAX_ALBUM_IMAGES) {
        send_err_page(req, "相册已满，最多保存30张图片，请先删除部分");
        return ESP_OK;
    }
    char path[64];
    // 短文件名：img_NNN.jpg
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/album/img_%03d.jpg", next_album_index());
    upload_ctx_t ctx = {
        .target_path = path,
        .max_size = 64 * 1024,   // 浏览器端已压成 JPEG，单张通常 15~25KB
        .success_msg = "相册图片上传成功",
        .nvs_flag_key = NULL
    };
    return generic_upload_handler(req, &ctx);
}

// ---------------------------------------------------------
// 删除端点
// ---------------------------------------------------------
static esp_err_t delete_music_handler(httpd_req_t *req) {
    httpd_req_recv(req, NULL, 0); // 消耗POST数据
    remove(STORAGE_MOUNT_POINT "/music/boot.mp3");
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i8(h, NVS_KEY_BOOT_MUSIC_SET, 0);
        nvs_commit(h); nvs_close(h);
    }
    boot_music_exists = false;
    send_ok_page(req, "已删除自定义开机音乐", "将使用默认开机音乐");
    return ESP_OK;
}

static esp_err_t delete_avatar_handler(httpd_req_t *req) {
    httpd_req_recv(req, NULL, 0);
    remove(STORAGE_MOUNT_POINT "/avatar/avatar.bin");
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i8(h, NVS_KEY_AVATAR_SET, 0);
        nvs_commit(h); nvs_close(h);
    }
    avatar_exists = false;
    send_ok_page(req, "头像已删除", "");
    return ESP_OK;
}

static esp_err_t album_delete_handler(httpd_req_t *req) {
    char* name = strstr(req->uri, "?name=");
    if (!name) { send_err_page(req, "参数错误"); return ESP_OK; }
    name += 6;
    char path[320];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/album/%s", name);
    remove(path);
    send_ok_page(req, "图片已删除", "");
    return ESP_OK;
}

// GET /album/<文件名>：把已上传的照片回传给浏览器（相册页的缩略图与原图查看都用它）
// 只允许访问相册目录下的简单文件名（拒绝 / 与 .. ，防止目录穿越）
static esp_err_t album_file_handler(httpd_req_t *req) {
    const char* raw = req->uri;
    const char* slash = strrchr(raw, '/');
    if (!slash || !slash[1]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_OK; }

    char name[80];
    url_decode_bounded(name, sizeof(name), slash + 1);
    if (strchr(name, '/') || strstr(name, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }

    char path[160];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/album/%s", name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    FILE* f = fopen(path, "rb");
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open fail"); return ESP_OK; }

    // 相册里可能是新版 .jpg，也可能是旧版 .bin，按扩展名给 Content-Type
    if (strstr(name, ".jpg") || strstr(name, ".jpeg")) httpd_resp_set_type(req, "image/jpeg");
    else httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_set_hdr(req, "Connection", "close");

    // 分块回传，避免为一张图再申请大块内存（httpd 任务栈只有 8KB）
    char fbuf[512];
    size_t r;
    esp_err_t err = ESP_OK;
    while ((r = fread(fbuf, 1, sizeof(fbuf), f)) > 0) {
        if (httpd_resp_send_chunk(req, fbuf, (ssize_t)r) != ESP_OK) { err = ESP_FAIL; break; }
    }
    fclose(f);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t album_delete_all_handler(httpd_req_t *req) {
    httpd_req_recv(req, NULL, 0);
    DIR* d = opendir(STORAGE_MOUNT_POINT "/album");
    if (d) {
        struct dirent* e;
        char path[320];
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_REG) {
                snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/album/%s", e->d_name);
                remove(path);
            }
        }
        closedir(d);
    }
    send_ok_page(req, "已批量删除所有相册图片", "");
    return ESP_OK;
}

// ---------------------------------------------------------
// 抖音弹幕：保存地址 / 断开连接 / 状态查询(JSON)
// ---------------------------------------------------------
/* 把字符串转义成合法 JSON 字符串体 */
static void json_escape(const char* in, char* out, size_t out_len) {
    size_t o = 0;
    if (!in) in = "";
    for (const char* p = in; *p && o + 7 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  memcpy(out + o, "\\\"", 2); o += 2; break;
            case '\\': memcpy(out + o, "\\\\", 2); o += 2; break;
            case '\n': memcpy(out + o, "\\n", 2);  o += 2; break;
            case '\r': memcpy(out + o, "\\r", 2);  o += 2; break;
            case '\t': memcpy(out + o, "\\t", 2);  o += 2; break;
            default:
                if (c < 0x20) {
                    int w2 = snprintf(out + o, out_len - o, "\\u%04x", c);
                    if (w2 > 0) o += (size_t)w2;
                } else {
                    out[o++] = (char)c;
                }
                break;
        }
    }
    out[o] = 0;
}

// 弹幕接收总开关：默认关闭；打开后才会按已保存的地址建立连接
static esp_err_t danmaku_switch_handler(httpd_req_t *req) {
    char buf[64] = {0};
    int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (r > 0) buf[r] = 0;
    int on = 0;
    char* p = strtok(buf, "&");
    while (p) {
        if (strncmp(p, "on=", 3) == 0) on = atoi(p + 3);
        p = strtok(NULL, "&");
    }
    danmaku_set_enabled(on != 0);
    if (on) send_ok_page(req, "弹幕接收已开启", "<p>若地址为空则暂不连接，请在下方填写 WebSocket 地址并保存</p>");
    else    send_ok_page(req, "弹幕接收已关闭", "<p>已停止接收，不再产生任何网络请求</p>");
    return ESP_OK;
}

static esp_err_t danmaku_save_handler(httpd_req_t *req) {
    char buf[512] = {0};
    int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (r <= 0) { send_err_page(req, "读取请求失败"); return ESP_OK; }
    buf[r] = 0;

    char enc[256] = {0};
    char url[DANMAKU_URL_MAX] = {0};
    char* p = strtok(buf, "&");
    while (p) {
        if (strncmp(p, "url=", 4) == 0) snprintf(enc, sizeof(enc), "%s", p + 4);
        p = strtok(NULL, "&");
    }
    url_decode_bounded(url, sizeof(url), enc);

    if (!danmaku_set_url(url)) {
        send_err_page(req, "地址格式不正确，需以 ws:// 或 wss:// 开头");
        return ESP_OK;
    }
    if (!danmaku_get_enabled()) {
        send_ok_page(req, "弹幕地址已保存", "<p>弹幕总开关当前为关闭状态，地址已记住；打开开关后才会连接。</p>");
    } else if (url[0]) {
        send_ok_page(req, "弹幕地址已保存", "<p>已在后台开始连接服务器</p>");
    } else {
        send_ok_page(req, "弹幕接收已停止", "<p>地址为空，总开关保持开启但不会连接</p>");
    }
    return ESP_OK;
}

static esp_err_t danmaku_disconnect_handler(httpd_req_t *req) {
    httpd_req_recv(req, NULL, 0);
    danmaku_set_url("");
    send_ok_page(req, "已断开弹幕连接", "");
    return ESP_OK;
}

static esp_err_t danmaku_status_handler(httpd_req_t *req) {
    static char buf[4096];
    size_t pos = 0;

    int w = snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"total\":%u,\"msgs\":[",
                     danmaku_get_state_str(), danmaku_get_total());
    if (w > 0 && (size_t)w < sizeof(buf)) pos = (size_t)w;

    unsigned total = danmaku_get_total();
    unsigned start = (total > 8) ? (total - 8) : 0;
    bool first = true;
    for (unsigned seq = start; seq < total; seq++) {
        char msg[DANMAKU_MSG_MAX];
        if (!danmaku_get_by_seq(seq, msg, sizeof(msg))) continue;
        char esc[DANMAKU_MSG_MAX * 2 + 16];
        json_escape(msg, esc, sizeof(esc));
        w = snprintf(buf + pos, sizeof(buf) - pos, "%s\"%s\"", first ? "" : ",", esc);
        if (w < 0 || (size_t)w >= sizeof(buf) - pos) { pos = sizeof(buf) - 1; break; }
        pos += (size_t)w;
        first = false;
    }
    if (pos + 3 < sizeof(buf)) {
        w = snprintf(buf + pos, sizeof(buf) - pos, "]}");
        if (w > 0 && (size_t)w < sizeof(buf) - pos) pos += (size_t)w;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, (ssize_t)pos);
    return ESP_OK;
}

// ---------------------------------------------------------
// 浏览器会自动请求 /favicon.ico，直接返回 204 No Content，
// 避免它被当成首页请求处理、占用一个连接槽位。
//
// 手机连热点后系统会自动发 captive portal 探测请求（iOS/Android/Windows/Mac 各一套），
// 如果这些请求走到 index handler 会触发完整 15KB HTML 发送，挤占真正用户请求的内存。
// 这里全部返回 204 No Content，让手机知道"不需要登录跳转"，同时零响应体零内存开销。
// ---------------------------------------------------------
static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t captive_noop_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---------------------------------------------------------
// 状态页局部刷新接口：返回 ~200 字节 JSON，供前端每 5 秒 fetch 一次，
// 替代整页 reload（整页约 15KB，容易在弱网/后台触发 send:11 EAGAIN）。
// ---------------------------------------------------------
static esp_err_t status_data_handler(httpd_req_t *req) {
    read_battery_percent();

    int total_kb = 0;
    int pct = get_storage_used_kb(&total_kb);
    int used_kb = (int)((long long)total_kb * pct / 100);
    int free_kb = total_kb - used_kb;
    int free_pct = 100 - pct;

    char time_buf[40];
    char date_buf[40];
    const char *weekday = "星期--";
    strcpy(time_buf, "未同步");
    strcpy(date_buf, "----/--/--");

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    // 与状态页/自动补同步任务统一口径：年份达到当前量级才算"已被 NTP 校正"。
    // 仅从 NVS 恢复的历史时间（年份可能对但已滞后）不算已同步。
    bool synced = time_is_synced();
    if (ti.tm_year > (2024 - 1900)) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        static const char *wd[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        weekday = wd[ti.tm_wday];
    }
    // 时间来源标记：2=NTP 已同步 1=关机时间已恢复(待校正) 0=未同步
    int tsrc = synced ? 2 : (time_is_restored() ? 1 : 0);

    // 充电状态字段：返回纯文本枚举（前端 JS 自己包 status-badge 样式），
    // 避免在 JSON 字符串里嵌带引号的 HTML 片段被 JSON 解析器破坏。
    //   "charging"   → USB 供电/充电中
    //   "battery"    → 电池供电
    //   "unknown"    → 电量计未就绪
    const char *chg_str = bsp_batt_ok ? (battery_charging ? "charging" : "battery") : "unknown";

    // 最坏长度：固定字段 ~70B + time 8 + date 10 + week 9 + chg 约 10B + 数字若干 ≈ 200B。
    // 512B 缓冲有充足余量，不会触发 -Wformat-truncation。
    static char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "{\"bat\":%d,\"mv\":%d,\"chg\":\"%s\","
                     "\"time\":\"%s\",\"date\":\"%s\",\"week\":\"%s\","
                     "\"tsrc\":%d,"
                     "\"pct\":%d,\"used\":%d,\"total\":%d,\"free\":%d,\"freepct\":%d}",
                     battery_percent, battery_mv > 0 ? battery_mv : 0, chg_str,
                     time_buf, date_buf, weekday, tsrc,
                     pct, used_kb, total_kb, free_kb, free_pct);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status too long");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, (ssize_t)n);
    return ESP_OK;
}

// ---------------------------------------------------------
// 启动Web服务器（带内存检查与自动重试）
//
// 根因：开机启动期 LVGL(约60KB) + 画布(38KB) + WiFi驱动(约30KB) +
//      ADC/LCD/NVS/事件循环等总共占用约 160~170KB，
//      ESP32-C3 可用堆约 180KB。开机后第 1 秒内剩余堆可能只有 15~30KB，
//      而 httpd 需要 ~30~40KB（task stack + 每 socket 收发缓冲），
//      此时 httpd_start() 会因内存不足返回 ESP_ERR_HTTPD_ALLOC_MEM（-1），
//      直接报错 "Failed to start web server!"——这就是串口看到的 E 错误。
//      后续 SPIFFS 挂载、静态内存布局稳定后，可用堆会回升到 50~80KB，
//      此时重试即可正常启动。
// ---------------------------------------------------------
// ★ 实测依据（gcc -fstack-usage 对本文件真实测量）：
//   generic_upload_handler 栈帧曾达 2624 字节（内含 char buf[HTTP_RECV_BUF_SIZE]=2048B），
//   save_wifi_handler 1232 字节。httpd 任务还要叠加自身框架（约 400B）与
//   snprintf/_svfprintf_r（约 1KB），4KB 栈在"保存 WiFi / 上传头像·音乐·相册"路径上会溢出。
//   现场已复现：Guru Meditation Error: Core 0 panic'ed (Stack protection fault)，崩在 httpd 的 snprintf。
//   对策（双保险）：① 上传接收缓冲改 static，消除最大单点；② 任务栈提到 6KB 兜底。
#define HTTPD_TASK_STACK 6144       // 6KB（原 4KB 偏紧）
#define HTTPD_MAX_SOCKETS  2        // 单用户2个槽：1主响应 + 1浏览器预连；多了占内存
#define HTTPD_REQUIRED_FREE (22 * 1024)  // 启动前至少留 22KB 空闲（画布从38KB缩到17KB后够用）

static httpd_handle_t s_server = NULL;

static void start_web_server(void) {
    if (s_server) return; // 已启动就别再启

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = HTTPD_TASK_STACK;
    config.max_uri_handlers  = 32;     // 实际注册 30 条（含通配 /album/*），留 2 条余量
    config.max_open_sockets  = 1;      // 单AP单用户场景，仅保留1个TCP连接槽，省~5KB socket收发缓冲
    config.send_wait_timeout = 15;     // 15秒发送超时（短连接够用，避免卡死占资源）
    config.recv_wait_timeout = 3;      // 3秒接收超时
    config.lru_purge_enable  = true;   // 自动回收最久不用的socket（防止占满槽位）
    config.keep_alive_enable = false;  // 短连接模式：发完立即关TCP，释放内存
    config.backlog_conn      = 1;      // listen backlog=1，避免手机预连堆在backlog里

    size_t free_mem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_mem < HTTPD_REQUIRED_FREE) {
        // 内存不足时不在这里阻塞，交给下面的重试任务处理
        ESP_LOGW(TAG, "空闲内存仅 %d KB，不够启动 Web 服务器（需 %d KB），稍后重试",
                 (int)(free_mem/1024), HTTPD_REQUIRED_FREE/1024);
        return;
    }

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s (空闲 %d KB)", esp_err_to_name(err), (int)(free_mem/1024));
        return;
    }

    httpd_uri_t uris[] = {
        { "/",                  HTTP_GET,  index_get_handler,           NULL },
        { "/favicon.ico",       HTTP_GET,  favicon_handler,             NULL },
        // —— Captive portal 探测路径（手机一连热点就会发，全部返回 204 避免占资源）——
        { "/generate_204",      HTTP_GET,  captive_noop_handler,        NULL },  // Android/Chrome
        { "/gen_204",           HTTP_GET,  captive_noop_handler,        NULL },  // Android 旧版
        { "/hotspot-detect.html", HTTP_GET, captive_noop_handler,       NULL },  // iOS/Mac
        { "/library/test/success.html", HTTP_GET, captive_noop_handler, NULL },  // iOS 旧版
        { "/ncsi.txt",          HTTP_GET,  captive_noop_handler,        NULL },  // Windows
        { "/connecttest.txt",   HTTP_GET,  captive_noop_handler,        NULL },  // Windows 10+
        { "/connecttest.html",  HTTP_GET,  captive_noop_handler,        NULL },  // Windows 备用
        // —— 业务路由 ——
        { "/save_settings", HTTP_POST, save_settings_handler,   NULL },
        { "/save_name",     HTTP_POST, save_name_handler,       NULL },
        { "/save_nfc",      HTTP_POST, save_nfc_handler,        NULL },
        { "/clear_nfc",     HTTP_POST, clear_nfc_handler,       NULL },
        { "/save_wifi",     HTTP_POST, save_wifi_handler,       NULL },
        { "/wifi_connect",  HTTP_GET,  wifi_connect_handler,    NULL },
        { "/wifi_delete",   HTTP_GET,  wifi_delete_handler,     NULL },
        { "/sync",          HTTP_POST, sync_post_handler,       NULL },
        { "/upload_avatar", HTTP_POST, upload_avatar_handler,   NULL },
        { "/upload_music",  HTTP_POST, upload_music_handler,    NULL },
        { "/upload_album",  HTTP_POST, upload_album_handler,    NULL },
        { "/delete_music",  HTTP_POST, delete_music_handler,    NULL },
        { "/delete_avatar", HTTP_POST, delete_avatar_handler,   NULL },
        { "/album_delete",  HTTP_GET,  album_delete_handler,    NULL },
        { "/album_delete_all", HTTP_POST, album_delete_all_handler, NULL },
        { "/album/*",       HTTP_GET,  album_file_handler,      NULL },
        { "/danmaku_switch",     HTTP_POST, danmaku_switch_handler,     NULL },
        { "/danmaku_save",       HTTP_POST, danmaku_save_handler,       NULL },
        { "/danmaku_disconnect", HTTP_POST, danmaku_disconnect_handler, NULL },
        { "/danmaku_status",     HTTP_GET,  danmaku_status_handler,     NULL },
        { "/status_data",        HTTP_GET,  status_data_handler,        NULL },
    };
    // 逐条检查注册结果：早期版本 max_uri_handlers 不足时，末尾路由会静默注册失败
    // （IDF 只给 W 级 "no slots left"），浏览器每 3 秒轮询就是一次 404 刷屏，
    // 却很难定位是哪条路由丢了。这里显式报 ERROR，一眼可见。
    int reg_ok = 0, reg_fail = 0;
    for (int i = 0; i < (int)(sizeof(uris)/sizeof(uris[0])); i++) {
        esp_err_t reg_err = httpd_register_uri_handler(s_server, &uris[i]);
        if (reg_err == ESP_OK) {
            reg_ok++;
        } else {
            reg_fail++;
            ESP_LOGE(TAG, "路由注册失败(%s): %s", esp_err_to_name(reg_err), uris[i].uri);
        }
    }
    ESP_LOGI(TAG, "Web server started, register %d/%d endpoints%s (空闲内存 %d KB)",
             reg_ok, (int)(sizeof(uris)/sizeof(uris[0])),
             reg_fail ? " ★有失败" : "",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024));
}

// 延迟重试任务：每 2 秒检查一次，启动成功后自动退出
static void webserver_retry_task(void *arg) {
    (void)arg;
    for (int retry = 0; retry < 30; retry++) {  // 最多重试 60 秒（30×2秒）
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (s_server) break;
        size_t free_mem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "尝试启动 Web 服务器（第 %d 次，空闲 %d KB）...", retry + 1, (int)(free_mem/1024));
        start_web_server();
        if (s_server) break;
    }
    if (!s_server) {
        ESP_LOGE(TAG, "60 秒内始终无法启动 Web 服务器，放弃");
    }
    vTaskDelete(NULL);
}

static esp_netif_t *s_ap_netif = NULL;

// 确认 AP 网段（默认 192.168.4.0/24）已就绪，并顺带检测网段重叠。
//
// 说明：ESP-IDF 的 esp_netif 已在 lwIP 中注册了路由钩子（LWIP_HOOK_IP4_ROUTE_SRC），
// 会按各接口的 route_prio（STA=100、AP=10）与子网匹配自动决定发送出口：
// 目的地址落在 AP 子网（192.168.4.x）时，流量自动走 AP 接口，
// 因此无需手动添加静态路由（ESP-IDF 并没有 esp_netif_add_route 这个 API）。
//
// 真正需要警惕的是：上游路由器分配的网段与 AP 网段重叠（例如路由器也是 192.168.4.x）。
// 此时两个接口争抢同一网段的路由，发往手机的响应会被送到 STA 口，
// 表现为 httpd 报“error in send : 113 (EHOSTUNREACH)”、手机上的网页打不开。
// 遇到这种情况，应把 AP 换成其它网段（例如 192.168.33.1/24）。
static void ensure_ap_subnet_route(void) {
    if (!s_ap_netif) return;

    esp_netif_ip_info_t ap_ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) != ESP_OK) {
        ESP_LOGW(TAG, "无法获取 AP IP 信息");
        return;
    }

    char ap_ip_str[16]  = {0};
    char ap_msk_str[16] = {0};
    esp_ip4addr_ntoa(&ap_ip.ip, ap_ip_str, sizeof(ap_ip_str));
    esp_ip4addr_ntoa(&ap_ip.netmask, ap_msk_str, sizeof(ap_msk_str));

    // 检测 AP 网段是否与 STA 拿到的网段重叠
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_ip_info_t sta_ip;
        if (esp_netif_get_ip_info(sta_netif, &sta_ip) == ESP_OK && sta_ip.ip.addr != 0) {
            char sta_ip_str[16] = {0};
            esp_ip4addr_ntoa(&sta_ip.ip, sta_ip_str, sizeof(sta_ip_str));
            if ((sta_ip.ip.addr & ap_ip.netmask.addr) == (ap_ip.ip.addr & ap_ip.netmask.addr)) {
                ESP_LOGW(TAG, "警告: STA 网段 %s 与 AP 网段 %s/%s 重叠，"
                              "手机上的网页可能无法访问，建议把 AP 换到其它网段",
                         sta_ip_str, ap_ip_str, ap_msk_str);
            }
        }
    }

    ESP_LOGI(TAG, "AP 网段就绪: %s / %s", ap_ip_str, ap_msk_str);
}

// ---------------------------------------------------------
// WiFi事件处理
// ---------------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_connected_status = true;
        ESP_LOGI(TAG, "WiFi Connected");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected_status = false;
        ESP_LOGI(TAG, "WiFi Disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP, starting SNTP");
        initialize_sntp();
        ensure_ap_subnet_route();   // 拿到外网 IP 后，给 AP 网段补路由，避免回包走错接口
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        // 手机连入 AP 的瞬间补一次路由（覆盖首次启动 STA 尚未拿到 IP 的场景）
        ensure_ap_subnet_route();
    }
}

// ---------------------------------------------------------
// 对外公开接口供GUI层调用
// ---------------------------------------------------------
const char* get_dev_name(void) { return current_custom_name; }
const char* get_nfc_content(void) { return nfc_content; }
int get_sleep_key(void) { return sleep_key; }
int get_menu_key(void)  { return menu_key; }

// 熄屏按键编号 → 网页/串口显示用的名称（与真机丝印一致的 UP键 / DOWN键 / OK键）
const char* get_sleep_key_name(int id) {
    switch (id) {
        case SLEEP_KEY_DOWN: return "DOWN键";
        case SLEEP_KEY_OK:   return "OK键";
        case SLEEP_KEY_UP:
        default:             return "UP键";
    }
}

// 功能菜单按键编号 → 网页/串口显示用的名称（越界按默认 OK键）
const char* get_menu_key_name(int id) {
    switch (id) {
        case SLEEP_KEY_UP:   return "UP键";
        case SLEEP_KEY_DOWN: return "DOWN键";
        case SLEEP_KEY_OK:
        default:             return "OK键";
    }
}

// NTP 自动同步频率
int get_ntp_sync_mode(void) { return ntp_sync_mode; }
const char* get_ntp_sync_mode_name(int mode) {
    switch (mode) {
        case SYNC_MODE_DAILY:  return "每天零时";
        case SYNC_MODE_HOURLY:
        default:               return "每小时";
    }
}

// 整点报时配置
bool get_hour_chime_enabled(void) { return chime_enabled; }
int  get_chime_silent_start(void) { return chime_silent_start; }
int  get_chime_silent_end(void)   { return chime_silent_end; }

// 电池查询（给 GUI 层功能菜单显示右上角电量用）
int  get_battery_soc(void) {
    // 界面调用频率较低（1Hz 刷新），这里现取一次保证数据最新；失败返回上次缓存。
    read_battery_percent();
    return (battery_percent >= 0) ? battery_percent : 0;
}
int  get_battery_mv(void) {
    read_battery_percent();
    return battery_mv;
}
bool get_battery_charging(void) {
    read_battery_percent();
    return battery_charging && bsp_batt_ok;
}

bool is_wifi_connected(void) { return wifi_connected_status; }
int get_battery_percent(void) { return battery_percent; }
int get_saver_timeout_sec(void) { return saver_timeout_sec; }
int get_album_interval_sec(void) { return album_interval_sec; }
int get_screensaver_theme(void) { return screensaver_theme; }
bool has_custom_avatar(void) { return avatar_exists; }
bool has_custom_boot_music(void) { return boot_music_exists; }
const char* get_storage_mount_point(void) { return STORAGE_MOUNT_POINT; }
int get_storage_used_percent(int *out_total_kb) { return get_storage_used_kb(out_total_kb); }
void* get_battery_adc_handle(void) { return (void*)adc_handle; }

// ---------------------------------------------------------
// 初始化入口（原wifi_init_softap_sta 扩展）
// ---------------------------------------------------------
void web_config_init(void) {
    // 先初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    load_config_from_nvs();

    // ★ 从 NVS 恢复上次保存的时间基准（放在 SNTP 启动之前，
    //   这样 time() 一开机就有相对准确的时间，SNTP 连上后再校正）
    restore_time_from_nvs();

    fs_init();
    init_battery_adc();
    read_battery_percent();

    // 网络初始化
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ret);
    esp_netif_init();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // ★ 内存瘦身：把 WiFi 驱动的默认缓冲池（给双核+PSRAM用的32槽）压缩到
    //   适合单核 ESP32-C3 AP+STA 单用户场景的数值。
    //   动态接收/发送缓冲 32→6，静态接收缓冲 10→2，Block-ACK 窗口 6→2。
    //   仅这一步就能释放约 30~40 KB 内部 SRAM，给 httpd 留出启动内存。
    //   注意：只改 IDF v5.5 里确实存在的字段，不使用高版本新增字段。
    cfg.dynamic_rx_buf_num = 6;
    cfg.dynamic_tx_buf_num = 6;
    cfg.static_rx_buf_num  = 2;
    cfg.cache_tx_buf_num   = 0;       // 单用户场景不需要 TX 缓存缓冲（省 ~10KB）
    cfg.rx_mgmt_buf_num    = 6;
    cfg.rx_ba_win          = 2;
    cfg.beacon_max_len     = 752;
    cfg.mgmt_sbuf_num      = 8;       // 管理缓冲默认 32 → 8（最小值 6）
    cfg.ampdu_rx_enable    = 1;
    cfg.ampdu_tx_enable    = 0;       // 单连接小页面，不用 A-MPDU 聚合并发，省内存
    cfg.nvs_enable         = 0;       // 我们自己用 nvs_flash_init 管，不让 WiFi 驱动重复开
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 获取当前MAC地址，AP 热点名按用户要求改为 "Passport" + MAC后三位十六进制大写
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid[64];
    snprintf(ap_ssid, sizeof(ap_ssid), "Passport%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {
        .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN },
    };
    strlcpy((char*)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    // 如果有保存的WiFi，自动连接
    if (current_wifi_idx >= 0 && current_wifi_idx < saved_wifi_count) {
        char pass[64] = {0};
        load_password_by_idx(current_wifi_idx, pass, sizeof(pass));
        wifi_config_t sta_cfg = {0};
        strlcpy((char*)sta_cfg.sta.ssid, saved_ssid_list[current_wifi_idx], sizeof(sta_cfg.sta.ssid));
        strlcpy((char*)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // ★★★ 本次问题的核心修复：关闭 Wi-Fi 省电模式（Modem-sleep）★★★
    // 本机是 AP+STA 双模。STA 连上路由器后，Wi-Fi 驱动会启用默认的
    // WIFI_PS_MIN_MODEM（串口日志里的 "I (2080) wifi:pm start, type: 1" 就是它），
    // 此后射频会按上游 AP 的 DTIM 周期周期性休眠（本机实测上游 beacon 102.4ms、
    // DTIM period = 3，即约 307ms 才醒一次）。射频休眠期间 SoftAP 侧的下行数据
    // （也就是发给手机的 HTTP 响应）发不出去，只能在发送队列里积压，
    // 最终触发 httpd 的发送超时 —— 表现为串口刷屏 "httpd_txrx: httpd_sock_err:
    // error in send : 11"（EAGAIN 超时），手机浏览器一直白屏、打不开 192.168.4.1。
    // 官方文档也明确：只有关闭 Modem-sleep 才能“最小化实时接收 Wi-Fi 数据的延迟”。
    // AP+STA 共存场景必须关闭，代价仅是多消耗几毫安电流。
    esp_err_t ps_ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_ret != ESP_OK) {
        ESP_LOGW(TAG, "关闭 Wi-Fi 省电模式失败: %s", esp_err_to_name(ps_ret));
    } else {
        wifi_ps_type_t ps_now = WIFI_PS_MIN_MODEM;
        esp_wifi_get_ps(&ps_now);
        ESP_LOGI(TAG, "Wi-Fi 省电模式已关闭 (ps=%d, 0=NONE)，AP 下行不再受 DTIM 休眠影响", (int)ps_now);
    }

    if (current_wifi_idx >= 0) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Auto connecting to saved WiFi: %s", saved_ssid_list[current_wifi_idx]);
    }

    // 第一次尝试启动 Web 服务器（此时内存可能仍紧张，不保证成功；失败会打印告警）
    start_web_server();

    // 若首次启动失败（内存不足），启动后台重试任务，每 2 秒尝试一次直到成功
    if (!s_server) {
        xTaskCreate(webserver_retry_task, "web_retry", 3072, NULL, 2, NULL);
    }

    danmaku_init();   // 启动抖音弹幕接收（地址从 NVS 读取）

    // NTP/整点报时任务：栈提至 3072（新增整点报时合成逻辑在独立任务，但本任务
    // 每 1 秒 localtime/snprintf 一次，原来 2048 余量偏紧）。
    // 音频 codec 改为首次报时时延迟初始化（bsp_audio_init 幂等），开机不常驻占用8~10KB DMA缓冲。
    xTaskCreate(ntp_auto_sync_task, "ntp_sync", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "AP started: [%s], visit http://192.168.4.1", ap_ssid);
}
