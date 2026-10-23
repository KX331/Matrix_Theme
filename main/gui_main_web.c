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
#define WIFI_LIST_NAMESPACE "wifi_list"

#define STORAGE_MOUNT_POINT "/storage"
#define MAX_ALBUM_IMAGES 30
#define ALBUM_MAX_W 240
#define ALBUM_MAX_H 320
#define AVATAR_MAX_W 90
#define AVATAR_MAX_H 90
#define MAX_BOOT_MUSIC_SIZE (256 * 1024) // 开机音乐最大256KB
#define HTTP_RECV_BUF_SIZE 2048
// 单个标签页 HTML 片段缓冲区。按各页"静态文本 + 最坏参数"逐字节核算，
// 最坏的是"设置"页：2184B 静态文本 + WiFi 列表 2047B（5 项，含两处 SSID 回显）
// + NTP 地址 63B + 音乐状态 71B + 屏保选中项 8B×2 + 两个 %d 各 11B = 4403B，
// 原值 4096 会在 -Werror=format-truncation 下编译失败（运行期也会被静默截断）。
// 6144 留 1741B 余量；若后续在设置页继续加内容，需同步按上式重算。
#define HTML_CHUNK_SIZE 6144

static char current_custom_name[32] = "Matrix Clock";
static char ntp_server_addr[64] = "ntp.ntsc.ac.cn";
static char saved_ssid_list[5][32] = {0}; // 最多保存5个WiFi
static int saved_wifi_count = 0;
static int current_wifi_idx = -1;
static bool wifi_connected_status = false;
static int screensaver_theme = 0; // 0=无, 1=黑客帝国数字雨
static int saver_timeout_sec = 30;
static int album_interval_sec = 10;
static bool avatar_exists = false;
static bool boot_music_exists = false;
static int battery_percent = 0;
static adc_oneshot_unit_handle_t adc_handle = NULL;
static bool s_fs_ready = false;   // SPIFFS 是否挂载成功（供状态页显示）

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
// 电池ADC读取 (ESP32-C3 ADC1_CH0 = GPIO0)
// ---------------------------------------------------------
static void init_battery_adc(void) {
    adc_oneshot_unit_init_cfg_t cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&cfg, &adc_handle));
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_0, &chan_cfg));
}

static int read_battery_percent(void) {
    if (!adc_handle) return 50;
    int raw = 0;
    adc_oneshot_read(adc_handle, ADC_CHANNEL_0, &raw);
    // 简化估算：0~4095对应0~3.3V，锂电池3.0V=0%, 4.2V=100%
    int mv = raw * 3300 / 4095 * 2; // 分压电路2倍
    int pct = (mv - 3000) * 100 / 1200;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    battery_percent = pct;
    return pct;
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
        int8_t flag;
        if (nvs_get_i8(handle, NVS_KEY_AVATAR_SET, &flag) == ESP_OK) avatar_exists = (flag != 0);
        if (nvs_get_i8(handle, NVS_KEY_BOOT_MUSIC_SET, &flag) == ESP_OK) boot_music_exists = (flag != 0);
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
        screensaver_theme = theme;
        saver_timeout_sec = timeout;
        album_interval_sec = album_int;
        nvs_commit(handle);
        nvs_close(handle);
    }
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

static void ntp_auto_sync_task(void *pvParameters) {
    bool synced_today = false;
    while (1) {
        time_t now = 0;
        struct tm timeinfo = { 0 };
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2024 - 1900)) {
            if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0 && !synced_today) {
                sync_time_manual();
                synced_today = true;
            } else if (timeinfo.tm_hour != 0) {
                synced_today = false;
            }
        }
        read_battery_percent();
        vTaskDelay(pdMS_TO_TICKS(30000));
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
    char battery_buf[64];
    snprintf(battery_buf, sizeof(battery_buf), "%d%%", battery_percent);

    char time_buf[32], date_buf[32];
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
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Matrix Passport 管理中心</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:'Consolas','Courier New',monospace;background:#0a0a0a;color:#0f0;padding:15px;max-width:500px;margin:0 auto}"
        "h1{text-align:center;font-size:20px;margin-bottom:10px;text-shadow:0 0 8px #0f0}"
        ".tabs{display:flex;border-bottom:2px solid #0f0;margin-bottom:15px}"
        ".tab{flex:1;padding:9px 1px;text-align:center;cursor:pointer;border:1px solid #0f0;border-bottom:none;"
        "background:#111;color:#0a0;font-size:11px;font-weight:bold;transition:all .2s}"
        ".tab.active{background:#0f0;color:#000;box-shadow:0 0 10px #0f0}"
        ".tab:hover:not(.active){background:#003300;color:#0f0}"
        ".tab-content{display:none;padding:15px;border:1px solid #0f0;border-radius:0 0 6px 6px;background:#0d0d0d}"
        ".tab-content.active{display:block}"
        ".card{background:#111;border:1px solid #060;border-radius:6px;padding:12px;margin-bottom:12px}"
        ".card-title{font-weight:bold;border-bottom:1px solid #060;padding-bottom:6px;margin-bottom:10px;color:#0f0}"
        ".row{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px dashed #030}"
        ".row:last-child{border-bottom:none}"
        ".label{color:#0a0}"
        ".value{color:#0f0;font-weight:bold}"
        "input,select,button{width:100%;padding:8px;margin:6px 0;border-radius:4px;border:1px solid #0f0;background:#000;color:#0f0;font-family:inherit;font-size:14px}"
        "button{background:#0f0;color:#000;font-weight:bold;cursor:pointer;transition:all .15s}"
        "button:hover{background:#0c0;box-shadow:0 0 8px #0f0}"
        "button.danger{background:#500;border-color:#f00;color:#f00}"
        "button.danger:hover{background:#800;box-shadow:0 0 8px #f00}"
        "button.mini{width:auto;padding:4px 10px;font-size:12px;display:inline-block}"
        ".form-group{margin-bottom:10px}"
        "label{display:block;color:#0a0;font-size:13px;margin-bottom:3px}"
        ".hint{font-size:11px;color:#060;margin-top:2px}"
        ".wifi-item{display:flex;justify-content:space-between;align-items:center;padding:8px;border:1px solid #060;border-radius:4px;margin-bottom:6px;background:#080808}"
        ".wifi-name{color:#0f0;font-weight:bold}"
        ".album-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:10px}"
        ".album-item{border:1px solid #060;border-radius:4px;padding:4px;text-align:center;font-size:11px;background:#080808}"
        ".album-item .del{color:#f00;cursor:pointer;font-size:12px;display:block;margin-top:3px}"
        ".progress-bar{width:100%;height:20px;background:#222;border:1px solid #0f0;border-radius:10px;overflow:hidden;margin:10px 0}"
        ".progress-fill{height:100%;background:#0f0;transition:width .3s}"
        ".status-badge{display:inline-block;padding:2px 8px;border-radius:10px;font-size:12px}"
        ".ok{background:#060;color:#0f0}"
        ".err{background:#600;color:#f00}"
        ".dkm{padding:6px 8px;margin-bottom:5px;border-left:3px solid #0f0;background:#080808;color:#0f0;font-size:13px;word-break:break-all}"
        "hr{border:none;border-top:1px solid #060;margin:12px 0}"
        "</style>"
        "<script>"
        "function switchTab(id){document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));"
        "document.querySelectorAll('.tab-content').forEach(c=>c.classList.remove('active'));"
        "document.getElementById(id).classList.add('active');"
        "document.querySelector('[data-tab=\"'+id+'\"]').classList.add('active');"
        "try{sessionStorage.setItem('mxtab',id);}catch(e){}"
        "if(id==='tab-danmaku')pollDanmaku();}"
        "function confirmDel(msg){return confirm(msg);}"
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
        "var b=document.getElementById('st_bat');if(b)b.textContent=d.bat+'%';"
        "var dt=document.getElementById('st_date');if(dt)dt.textContent=d.date+' '+d.week;"
        "var tm=document.getElementById('st_time');if(tm)tm.textContent=d.time;"
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
        "<div class='row'><span class='label'>充电状态</span><span class='value'><span class='status-badge ok'>USB供电</span></span></div>"
        "</div>"
        "<div class='card'><div class='card-title'>🕐 时间同步</div>"
        "<div class='row'><span class='label'>NTP服务器</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>当前日期</span><span class='value' id='st_date'>%s %s</span></div>"
        "<div class='row'><span class='label'>当前时间</span><span class='value' style='font-size:24px' id='st_time'>%s</span></div>"
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
        "<div style='font-size:12px;color:#0a0;margin-bottom:5px' id='st_store'>本机存储用量 %d%%（已用 %d KB / 共 %d KB，剩余 %d KB）</div>"
        "<div class='progress-bar' title='已用 %d%% / 空闲 %d%%'>"
        "<div class='progress-fill' id='st_bar' style='width:%d%%;background:linear-gradient(90deg,#0f0 0%%,#0a0 %d%%,#300 %d%%,#500 100%%'></div>"
        "</div>"
        "<div style='display:flex;justify-content:space-between;font-size:11px;color:#060'>"
        "<span>■ 已占用 %d%%</span><span>□ 未占用 %d%%</span>"
        "</div>"
        "</div>"
        "<p style='text-align:center;margin-top:15px;font-size:12px;color:#060'>状态页每 5 秒自动局部刷新（仅更新约 200B 数据）；后台时暂停刷新</p>"
        "</div>",
        (current_wifi_idx >= 0 && current_wifi_idx < saved_wifi_count) ? saved_ssid_list[current_wifi_idx] : "未保存",
        conn_str, battery_buf, ntp_server_addr, date_buf, weekday_buf, time_buf,
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
    // ★ 这两个缓冲原先在 httpd 任务栈上，与下面的 album_list_html / fnames
    //   合计约 5.5KB，而 httpd 任务栈只有 8KB，已逼近极限。栈溢出不会立刻 panic，
    //   只会在栈尾静默破坏相邻内存（表现为随机的响应异常），因此全部移入静态区。
    //   注意 static 不会自动清零，每次进入必须手动置首字节为 0。
    //   容量同时扩大：5 个 WiFi 项每项最坏 347 字节（两处 SSID 回显 32+32）
    //   × 5 项 + 结束符 = 1736 字节，原 1024 必被 strncat 截断导致列表显示不全，故扩到 2048。
    static char wifi_list_html[2048];
    static char tmp[512];
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
        // NTP设置
        "<div class='card'><div class='card-title'>🕐 NTP时间服务器</div>"
        "<form action='/save_settings' method='POST'>"
        "<div class='form-group'><label>NTP服务器地址</label>"
        "<input type='text' name='ntp' value='%s'>"
        "<div class='hint'>推荐: ntp.ntsc.ac.cn (国家授时中心)</div></div>"
        "</div>"
        // 开机音乐
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
        // 相册设置
        "<div class='card'><div class='card-title'>🖼️ 相册设置</div>"
        "<div class='form-group'><label>自动播放间隔 (秒)</label>"
        "<input type='number' name='album_interval' value='%d' min='3' max='60'>"
        "<div class='hint'>默认每10秒切换一张图片</div></div>"
        "<button type='submit'>💾 保存所有设置</button>"
        "</form>"
        "</div>"
        "</div>",
        wifi_list_html, ntp_server_addr,
        boot_music_exists ? "<div class='status-badge ok'>当前已上传自定义开机音乐</div>" : "<div class='status-badge err'>使用默认开机音乐</div>",
        screensaver_theme == 0 ? "selected" : "",
        screensaver_theme == 1 ? "selected" : "",
        saver_timeout_sec, album_interval_sec
    );
    httpd_resp_sendstr_chunk(req, chunk);

    // ====== 标签页3: 个人信息 ======
    snprintf(chunk, sizeof(chunk),
        "<div id='tab-profile' class='tab-content'>"
        "<div class='card'><div class='card-title'>👤 设备显示名称</div>"
        "<form action='/save_name' method='POST'>"
        "<div class='form-group'><label>显示名称（将在主界面显示）</label>"
        "<input type='text' name='name' value='%s' maxlength='16' required>"
        "<button type='submit'>💾 保存名称</button>"
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
        "</div>",
        current_custom_name,
        avatar_exists ? "<div class='status-badge ok'>✅ 已上传自定义头像</div>" : "<div class='status-badge err'>❌ 未上传头像，使用默认</div>"
    );
    httpd_resp_sendstr_chunk(req, chunk);

    // ====== 标签页5: 抖音弹幕 ======
    {
        static char dk_url_esc[DANMAKU_URL_MAX * 6 + 8];
        html_attr_escape(danmaku_get_url(), dk_url_esc, sizeof(dk_url_esc));
        snprintf(chunk, sizeof(chunk),
            "<div id='tab-danmaku' class='tab-content'>"
            "<div class='card'><div class='card-title'>🎤 抖音弹幕接收</div>"
            "<form action='/danmaku_save' method='POST'>"
            "<div class='form-group'><label>WebSocket 服务器地址</label>"
            "<input type='text' name='url' value='%s' placeholder='ws://192.168.1.100:8080/danmaku'>"
            "<div class='hint'>支持 ws:// 与 wss:// 两种协议；填写后点保存并连接即开始接收。留空保存则停止接收。</div>"
            "</div>"
            "<button type='submit'>💾 保存并连接</button>"
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
            dk_url_esc, danmaku_get_state_str(), danmaku_get_total());
        httpd_resp_sendstr_chunk(req, chunk);
    }

    // ====== 标签页4: 相册 ======
    int album_cnt = get_album_file_count();
    // ★ 相册列表：每项 148B 固定文本 + 两处文件名回显 %s。文件名最长 63B
    //   （get_album_filenames 按 63 截断），故单项最坏 148+63+63 = 274B；
    //   30 项 + 包裹标签 + 结束符 = 24 + 30×274 + 6 + 1 = 8251B，故取 8448（余 197B）。
    //   注意：按程序生成的 img_%03d.jpg（11B）算只需 5131B，但 SPIFFS 目录里
    //   一旦存在长名文件就会逼近上限，因此按最坏值配缓冲。
    static char album_list_html[8448];
    static char fnames[30][64];
    album_list_html[0] = 0;
    if (album_cnt > 0) {
        int cnt;
        get_album_filenames(fnames, &cnt);
        strncat(album_list_html, "<div class='album-grid'>", sizeof(album_list_html) - strlen(album_list_html) - 1);
        for (int i = 0; i < cnt; i++) {
            snprintf(tmp, sizeof(tmp),
                "<div class='album-item'>📷 %s<br>"
                "<a class='del' href='/album_delete?name=%s' onclick='return confirmDel(\"确定删除这张图片?\")'>删除</a>"
                "</div>", fnames[i], fnames[i]);
            strncat(album_list_html, tmp, sizeof(album_list_html)-strlen(album_list_html)-1);
        }
        strncat(album_list_html, "</div>", sizeof(album_list_html) - strlen(album_list_html) - 1);
    } else {
        strcpy(album_list_html, "<p style='color:#060;text-align:center;padding:20px'>相册为空，请上传图片</p>");
    }

    snprintf(chunk, sizeof(chunk),
        "<div id='tab-album' class='tab-content'>"
        "<div class='card'><div class='card-title'>🖼️ 相册管理</div>"
        "<div class='progress-bar'><div class='progress-fill' style='width:%d%%'></div></div>"
        "<p style='text-align:center'>已存 %d / 30 张图片（%d%%）</p>"
        "<hr>"
        "<label>上传新图片（最大分辨率240×320）</label>"
        "<div class='hint'>图片会在浏览器端自动等比缩放并压缩为 JPEG 后上传（约 15~25 KB / 张）</div>"
        "<form onsubmit='return uploadAlbum(event)'>"
        "<input type='file' id='albumFile' name='photo' accept='image/*'>"
        "<button type='submit'>📤 上传到相册</button>"
        "<div class='hint' id='albumStatus'></div>"
        "</form>"
        "<hr>"
        "<div style='margin-bottom:6px;font-weight:bold;color:#0f0'>已上传的图片</div>",
        album_cnt * 100 / MAX_ALBUM_IMAGES, album_cnt, album_cnt * 100 / MAX_ALBUM_IMAGES
    );
    httpd_resp_sendstr_chunk(req, chunk);
    // 相册列表（最多 30 张 × 170 字节 ≈ 5KB，单独一chunk，避免占满主 chunk 缓冲区）
    httpd_resp_sendstr_chunk(req, album_list_html);
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
    // 静态部分 952B + 最坏 msg/extra 384B ≈ 1336B；原 1024 会在保存 WiFi 时静默截断掉页面尾部的返回按钮
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='5;url=/'>"
        "<style>*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:#0a0a0a;color:#0f0;text-align:center;padding:60px 20px;font-family:'Consolas','Courier New',monospace}"
        "h2{font-size:24px;margin-bottom:20px;text-shadow:0 0 10px #0f0}"
        ".btn{display:inline-block;margin-top:30px;padding:12px 32px;background:#0f0;color:#000;"
        "font-weight:bold;font-size:16px;text-decoration:none;border-radius:6px;"
        "box-shadow:0 0 15px #0f0;transition:all .2s}"
        ".btn:hover{background:#0c0;box-shadow:0 0 25px #0f0}"
        ".tip{margin-top:20px;color:#060;font-size:14px;line-height:1.6}"
        "</style></head><body>"
        "<h2>✅ %s</h2>"
        "<div style='max-width:400px;margin:0 auto;line-height:1.8;font-size:15px'>%s</div>"
        "<div class='tip'>⏳ 设备可能需要 3~5 秒完成网络切换，请稍候...<br>5 秒后将自动返回管理页面</div>"
        "<a href='/' class='btn'>🔙 立即手动返回首页</a>"
        "</body></html>",
        msg, extra ? extra : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static void send_err_page(httpd_req_t* req, const char* msg) {
    // 静态部分 706B + 最长 msg 59B ≈ 766B；原 512 触发 -Werror=format-truncation 编译失败
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='5;url=/'>"
        "<style>*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:#0a0a0a;color:#f00;text-align:center;padding:60px 20px;font-family:'Consolas','Courier New',monospace}"
        "h2{font-size:24px;margin-bottom:20px;text-shadow:0 0 10px #f00}"
        ".btn{display:inline-block;margin-top:30px;padding:12px 32px;background:#f00;color:#000;"
        "font-weight:bold;font-size:16px;text-decoration:none;border-radius:6px;"
        "box-shadow:0 0 15px #f00}"
        ".tip{margin-top:20px;color:#600;font-size:14px}"
        "</style></head><body>"
        "<h2>❌ %s</h2>"
        "<div class='tip'>5 秒后将自动返回管理页面</div>"
        "<a href='/' class='btn'>🔙 返回首页</a>"
        "</body></html>", msg);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------
// POST: 保存通用设置（名称+NTP+屏保+相册间隔）
// ---------------------------------------------------------
static esp_err_t save_settings_handler(httpd_req_t *req) {
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret <= 0) return ESP_FAIL;
    char ntp[64]={0}, name[32]={0};
    int theme = screensaver_theme, timeout = saver_timeout_sec, interval = album_interval_sec;
    char *p = strtok(buf, "&");
    while (p) {
        if (strncmp(p,"ntp=",4)==0) url_decode_bounded(ntp, sizeof(ntp), p+4);
        else if (strncmp(p,"name=",5)==0) url_decode_bounded(name, sizeof(name), p+5);
        else if (strncmp(p,"saver_theme=",12)==0) theme = atoi(p+12);
        else if (strncmp(p,"saver_timeout=",14)==0) timeout = atoi(p+14);
        else if (strncmp(p,"album_interval=",15)==0) interval = atoi(p+15);
        p = strtok(NULL, "&");
    }
    if (timeout < 10) timeout = 10;
    if (interval < 3) interval = 3;
    save_config_to_nvs((strlen(name)>0)?name:NULL, (strlen(ntp)>0)?ntp:NULL, theme, timeout, interval);
    if (strlen(ntp) > 0) sync_time_manual();
    send_ok_page(req, "设置保存成功", "<p>新配置已生效</p>");
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
// POST: 添加新WiFi
// ---------------------------------------------------------
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
    char extra[512];
    snprintf(extra, sizeof(extra),
             "<p>已保存WiFi: <b>%s</b></p>"
             "<p style='margin-top:12px;color:#0a0'>📶 设备正在连接路由器，AP热点可能会因信道同步导致短暂断连（1~3秒），请稍候。<br>"
             "若手机自动断开热点，请重新连接 <b>%s</b> 后访问 <b>192.168.4.1</b>。</p>",
             ssid, ap_ssid_name);
    send_ok_page(req, "WiFi保存成功", extra);

    // 等响应发送完再切 WiFi（300ms 足够让 httpd 把响应缓冲发往对端，比之前200ms多留时间）
    vTaskDelay(pdMS_TO_TICKS(300));
    wifi_config_t cfg = {0};
    strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
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
    char buf[HTTP_RECV_BUF_SIZE];
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
                if (hs_pos >= sizeof(header_search)-1) hs_pos = 0;
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
        truncate(ctx->target_path, total_written - 60);
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
    if (url[0]) send_ok_page(req, "弹幕地址已保存", "<p>已在后台开始连接服务器</p>");
    else        send_ok_page(req, "弹幕接收已关闭", "");
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
    if (ti.tm_year > (2024 - 1900)) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        static const char *wd[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        weekday = wd[ti.tm_wday];
    }

    // 最坏长度：固定字段约 45B + time 8 + date 10 + week 9 + 5 个数字各 ≤11B ≈ 140B
    // 512B 缓冲区有充足余量，不会触发 -Wformat-truncation。
    static char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "{\"bat\":%d,\"time\":\"%s\",\"date\":\"%s\",\"week\":\"%s\","
                     "\"pct\":%d,\"used\":%d,\"total\":%d,\"free\":%d,\"freepct\":%d}",
                     battery_percent, time_buf, date_buf, weekday,
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
#define HTTPD_TASK_STACK 4096       // 4KB：handler 里所有大缓冲已 static，足够
#define HTTPD_MAX_SOCKETS  2        // 单用户2个槽：1主响应 + 1浏览器预连；多了占内存
#define HTTPD_REQUIRED_FREE (22 * 1024)  // 启动前至少留 22KB 空闲（画布从38KB缩到17KB后够用）

static httpd_handle_t s_server = NULL;

static void start_web_server(void) {
    if (s_server) return; // 已启动就别再启

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = HTTPD_TASK_STACK;
    config.max_uri_handlers  = 24;     // 预留几个位置给 captive portal 快速响应
    config.max_open_sockets  = HTTPD_MAX_SOCKETS;
    config.send_wait_timeout = 30;     // 30秒发送超时：弱网/小内存下给足发送时间
    config.recv_wait_timeout = 5;
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
        { "/danmaku_save",       HTTP_POST, danmaku_save_handler,       NULL },
        { "/danmaku_disconnect", HTTP_POST, danmaku_disconnect_handler, NULL },
        { "/danmaku_status",     HTTP_GET,  danmaku_status_handler,     NULL },
        { "/status_data",        HTTP_GET,  status_data_handler,        NULL },
    };
    for (int i = 0; i < (int)(sizeof(uris)/sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "Web server started, register %d endpoints (空闲内存 %d KB)",
             (int)(sizeof(uris)/sizeof(uris[0])),
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
    xTaskCreate(ntp_auto_sync_task, "ntp_sync", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "AP started: [%s], visit http://192.168.4.1", ap_ssid);
}
