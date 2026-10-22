#include <string.h>
#include <ctype.h>
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
#include "lvgl.h"
#include "esp_http_server.h"
#include "esp_sntp.h"

#define TAG "MATRIX_WEB"

#define NVS_NAMESPACE "matrix_cfg"
#define NVS_KEY_SSID  "wifi_ssid"
#define NVS_KEY_PASS  "wifi_pass"
#define NVS_KEY_NAME  "custom_name"
#define NVS_KEY_NTP   "ntp_server"

static char current_custom_name[32] = "Clock";
static char saved_wifi_ssid[32] = "None";
static char ntp_server_addr[64] = "ntp.ntsc.ac.cn";
static bool wifi_connected_status = false;

static void initialize_sntp(void);
static void sync_time_manual(void);

// ---------------------------------------------------------
// 对外公开的时间获取接口
// ---------------------------------------------------------
bool get_matrix_time_str(char *buf, size_t len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2024 - 1900)) {
        snprintf(buf, len, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
    }
    snprintf(buf, len, "00:00:00");
    return false;
}

bool get_matrix_date_str(char *buf, size_t len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2024 - 1900)) {
        snprintf(buf, len, "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        return true;
    }
    snprintf(buf, len, "1970-01-01");
    return false;
}

const char* get_matrix_weekday_str(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2024 - 1900)) {
        const char* weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        if (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) {
            return weekdays[timeinfo.tm_wday];
        }
    }
    return "星期--";
}

// ---------------------------------------------------------
// Wi-Fi 与系统内部逻辑
// ---------------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_connected_status = true;
        ESP_LOGI(TAG, "Wi-Fi Connected successfully.");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected_status = false;
        ESP_LOGI(TAG, "Wi-Fi Disconnected.");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP, initializing SNTP time synchronization...");
        initialize_sntp();
    }
}

static void load_config_from_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(current_custom_name);
        nvs_get_str(handle, NVS_KEY_NAME, current_custom_name, &size);
        
        size = sizeof(saved_wifi_ssid);
        if (nvs_get_str(handle, NVS_KEY_SSID, saved_wifi_ssid, &size) != ESP_OK) {
            strcpy(saved_wifi_ssid, "None");
        }

        size = sizeof(ntp_server_addr);
        if (nvs_get_str(handle, NVS_KEY_NTP, ntp_server_addr, &size) != ESP_OK) {
            strcpy(ntp_server_addr, "ntp.ntsc.ac.cn");
        }
        nvs_close(handle);
    }
}

static void save_config_to_nvs(const char* ssid, const char* pass, const char* name, const char* ntp) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (ssid && strlen(ssid) > 0) {
            nvs_set_str(handle, NVS_KEY_SSID, ssid);
            strcpy(saved_wifi_ssid, ssid);
        }
        if (pass && strlen(pass) > 0) {
            nvs_set_str(handle, NVS_KEY_PASS, pass);
        }
        if (name && strlen(name) > 0) {
            nvs_set_str(handle, NVS_KEY_NAME, name);
            strcpy(current_custom_name, name);
        }
        if (ntp && strlen(ntp) > 0) {
            nvs_set_str(handle, NVS_KEY_NTP, ntp);
            strcpy(ntp_server_addr, ntp);
        }
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP with server: %s", ntp_server_addr);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntp_server_addr);
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_init();
}

static void sync_time_manual(void) {
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    initialize_sntp();
    ESP_LOGI(TAG, "Manual time synchronization requested.");
}

static void ntp_auto_sync_task(void *pvParameters) {
    bool synced_today = false;
    while (1) {
        time_t now = 0;
        struct tm timeinfo = { 0 };
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year > (2024 - 1900)) {
            if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) {
                if (!synced_today) {
                    ESP_LOGI(TAG, "Daily 00:00 automatic time sync triggered.");
                    sync_time_manual();
                    synced_today = true;
                }
            } else {
                synced_today = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000)); 
    }
}

// ---------------------------------------------------------
// 分段发送 HTML（彻底避免栈内存溢出崩溃）
// ---------------------------------------------------------
static esp_err_t index_get_handler(httpd_req_t *req) {
    const char *status_str = wifi_connected_status ? "<span style='color:#0f0;'>Connected</span>" : "<span style='color:red;'>Disconnected / Connecting</span>";

    char time_buf[32];
    char date_buf[32];
    const char* weekday_buf = get_matrix_weekday_str();

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year > (2024 - 1900)) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    } else {
        strcpy(time_buf, "Not Synchronized");
        strcpy(date_buf, "------.--.--");
    }

    // 分段发送头部和样式
    httpd_resp_sendstr_chunk(req, 
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Matrix Clock Config</title>"
        "<style>body{font-family:Arial;background:#111;color:#0f0;text-align:center;padding:20px;}"
        "input,button{padding:10px;margin:8px;border-radius:5px;border:1px solid #0f0;background:#000;color:#0f0;width:250px;}"
        "button{background:#0f0;color:#000;font-weight:bold;cursor:pointer;}"
        "fieldset{border:1px solid #0f0;display:inline-block;padding:20px;border-radius:8px;margin-top:10px;text-align:left;}"
        "table{color:#0f0;width:100%;margin-top:10px;border-collapse:collapse;}"
        "th,td{border:1px solid #0f0;padding:8px;text-align:center;}"
        "</style></head>"
        "<body><h1>Matrix Clock Web Config</h1>"
    );

    // 发送状态表格
    char chunk[512];
    snprintf(chunk, sizeof(chunk),
        "<fieldset><legend><b>System & Wi-Fi Status</b></legend>"
        "<table><tr><th>Item</th><th>Status / Value</th></tr>"
        "<tr><td>Saved SSID</td><td>%s</td></tr>"
        "<tr><td>Connection</td><td>%s</td></tr>"
        "<tr><td>Current Date</td><td>%s (%s)</td></tr>"
        "<tr><td>Current Time (24H)</td><td>%s</td></tr>"
        "</table></fieldset><br>",
        saved_wifi_ssid, status_str, date_buf, weekday_buf, time_buf
    );
    httpd_resp_sendstr_chunk(req, chunk);

    // 发送设置表单
    snprintf(chunk, sizeof(chunk),
        "<fieldset><legend><b>Device & NTP Settings</b></legend>"
        "<form action='/save' method='POST'>"
        "<label>Wi-Fi SSID:</label><br><input type='text' name='ssid' placeholder='Enter Router SSID'><br>"
        "<label>Wi-Fi Password:</label><br><input type='password' name='pass' placeholder='Enter Router Password'><br>"
        "<label>Device Name:</label><br><input type='text' name='name' value='%s'><br>"
        "<label>NTP Server:</label><br><input type='text' name='ntp' value='%s'><br>"
        "<button type='submit'>Save Settings & Connect</button>"
        "</form>"
        "<br><form action='/sync' method='POST'>"
        "<button type='submit' style='background:#005500;color:#0f0;'>Manual Sync Time Now</button>"
        "</form></fieldset><br>",
        current_custom_name, ntp_server_addr
    );
    httpd_resp_sendstr_chunk(req, chunk);

    // 发送图片上传表单并结束
    httpd_resp_sendstr_chunk(req,
        "<fieldset><legend><b>Upload 80x80 Image</b></legend>"
        "<form action='/upload' method='POST' enctype='multipart/form-data'>"
        "<input type='file' name='image' accept='image/*'><br>"
        "<button type='submit'>Upload Image</button>"
        "</form></fieldset></body></html>"
    );

    httpd_resp_sendstr_chunk(req, NULL); // 结束传输
    return ESP_OK;
}

// URL 解码
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            if (a >= 'a') a -= 32;
            else if (a >= 'A') a -= 7;
            else a -= '0';
            
            if (b >= 'a') b -= 32;
            else if (b >= 'A') b -= 7;
            else b -= '0';
            
            *dst = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst = ' ';
            src++;
        } else {
            *dst = *src;
            src++;
        }
        dst++;
    }
    *dst = '\0';
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[384] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;

    char ssid[32] = {0}, pass[32] = {0}, name[32] = {0}, ntp[64] = {0};
    
    char *p = strtok(buf, "&");
    while (p != NULL) {
        if (strncmp(p, "ssid=", 5) == 0) {
            url_decode(ssid, p + 5);
        } else if (strncmp(p, "pass=", 5) == 0) {
            url_decode(pass, p + 5);
        } else if (strncmp(p, "name=", 5) == 0) {
            url_decode(name, p + 5);
        } else if (strncmp(p, "ntp=", 4) == 0) {
            url_decode(ntp, p + 4);
        }
        p = strtok(NULL, "&");
    }

    save_config_to_nvs(ssid, pass, name, ntp);

    const char *resp_html = 
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='3;url=/'></head>"
        "<body style='background:#111;color:#0f0;text-align:center;padding-top:100px;font-family:Arial;'>"
        "<h2>Settings Saved Successfully!</h2>"
        "<p>Applying new configuration and syncing time...</p>"
        "<p>Returning to configuration page in 3 seconds...</p>"
        "</body></html>";

    httpd_resp_send(req, resp_html, HTTPD_RESP_USE_STRLEN);
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (strlen(ssid) > 0) {
        wifi_config_t wifi_sta_config = {0};
        strlcpy((char *)wifi_sta_config.sta.ssid, ssid, sizeof(wifi_sta_config.sta.ssid));
        strlcpy((char *)wifi_sta_config.sta.password, pass, sizeof(wifi_sta_config.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config);
        esp_wifi_connect();
    }
    
    sync_time_manual();
    return ESP_OK;
}

static esp_err_t sync_post_handler(httpd_req_t *req) {
    sync_time_manual();

    const char *resp_html = 
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='2;url=/'></head>"
        "<body style='background:#111;color:#0f0;text-align:center;padding-top:100px;font-family:Arial;'>"
        "<h2>Time Synchronization Triggered!</h2>"
        "<p>Attempting to sync with: %s</p>"
        "<p>Returning in 2 seconds...</p>"
        "</body></html>";
    
    char resp_buf[512];
    snprintf(resp_buf, sizeof(resp_buf), resp_html, ntp_server_addr);
    httpd_resp_send(req, resp_buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t upload_post_handler(httpd_req_t *req) {
    char content[1024];
    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;

    while (cur_len < total_len) {
        if ((received = httpd_req_recv(req, content, MIN(total_len - cur_len, sizeof(content)))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        cur_len += received;
    }

    ESP_LOGI(TAG, "Received image upload, total size: %d bytes", total_len);
    httpd_resp_send(req, "<h3>Image Uploaded Successfully!</h3><a href='/'>Back</a>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_web_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL };
        httpd_uri_t sync_uri = { .uri = "/sync", .method = HTTP_POST, .handler = sync_post_handler, .user_ctx = NULL };
        httpd_uri_t upload_uri = { .uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL };
        
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &save_uri);
        httpd_register_uri_handler(server, &sync_uri);
        httpd_register_uri_handler(server, &upload_uri);
    }
}

void wifi_init_softap_sta(void) {
    load_config_from_nvs();

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_init();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    nvs_handle_t handle;
    char saved_pass[32] = {0};
    size_t size = sizeof(saved_pass);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_str(handle, NVS_KEY_PASS, saved_pass, &size);
        nvs_close(handle);
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    char ap_ssid[64];
    snprintf(ap_ssid, sizeof(ap_ssid), "Matrix_%s_%02X%02X", current_custom_name, mac[4], mac[5]);

    wifi_config_t wifi_ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    strlcpy((char *)wifi_ap_config.ap.ssid, ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    if (strcmp(saved_wifi_ssid, "None") != 0 && strlen(saved_wifi_ssid) > 0) {
        wifi_config_t wifi_sta_config = {0};
        strlcpy((char *)wifi_sta_config.sta.ssid, saved_wifi_ssid, sizeof(wifi_sta_config.sta.ssid));
        strlcpy((char *)wifi_sta_config.sta.password, saved_pass, sizeof(wifi_sta_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
        ESP_LOGI(TAG, "Connecting to saved Wi-Fi SSID: %s", saved_wifi_ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (strcmp(saved_wifi_ssid, "None") != 0 && strlen(saved_wifi_ssid) > 0) {
        esp_wifi_connect();
    }

    start_web_server();

    xTaskCreate(ntp_auto_sync_task, "ntp_auto_sync", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "Wi-Fi AP started. Hotspot: [%s], Connect & visit http://192.168.4.1", ap_ssid);
}