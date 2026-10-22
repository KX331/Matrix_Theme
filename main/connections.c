#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h" // 确保你的项目中引入了 cJSON 库

static const char *TAG_NET = "WebAdmin";

// 全局个性化变量（供 UI 界面线程实时读取）
char g_username[16] = "Morpheus"; // 🚀 修正：声明为可以容纳字符串的16字节数组
uint8_t g_avatar_id = 0;
uint32_t g_screensaver_timeout_sec = 30; // 默认 30 秒超时进入屏保
bool g_config_updated = false;

// 1. 前端 HTML + 自动对时 JavaScript 代码
static const char* html_page = 
"<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><title>AI Passport 配置</title>"
"<style>body{background-color:#111;color:#0f0;font-family:monospace;padding:20px;}input{background:#222;border:1px solid #0f0;color:#fff;padding:5px;width:100%;max-width:200px;margin-bottom:15px;}button{background:#0f0;border:none;color:#000;padding:8px 15px;cursor:pointer;font-weight:bold;}</style></head>"
"<body><h2>AI Passport 个性化设置</h2><form action=\"/save\" method=\"POST\">"
"<label>显示名称:</label><br><input type=\"text\" name=\"username\" maxlength=\"16\" required><br>"
"<label>头像索引(0-3):</label><br><input type=\"number\" name=\"avatar_id\" min=\"0\" max=\"3\" value=\"0\" required><br>"
"<label>屏保启用秒数(0关闭):</label><br><input type=\"number\" name=\"timeout\" min=\"0\" max=\"3600\" value=\"30\" required><br>"
"<button type=\"submit\">保存并应用</button></form><div id=\"sync\" style=\"color:#888;font-size:12px;margin-top:10px;\">正在自动对时...</div>"
"<script>window.addEventListener('DOMContentLoaded',()=>{const now=new Date();const ts=Math.floor(now.getTime()/1000);const offset=now.getTimezoneOffset();"
"fetch('/sync-time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({time:ts,offset:offset})})"
".then(res=>{if(res.ok)document.getElementById('sync').innerText='系统时间已自动同步: '+now.toLocaleString();});});</script></body></html>";

static esp_err_t get_handler(httpd_req_t *req) {
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 2. 接收并保存 Web 个性化表单 (名称/头像/超时)
static esp_err_t post_handler(httpd_req_t *req) {
    char buf[150];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char name_val[32] = {0};
    char avatar_val[10] = {0};
    char timeout_val[10] = {0};

    if (httpd_query_key_value(buf, "username", name_val, sizeof(name_val)) == ESP_OK &&
        httpd_query_key_value(buf, "avatar_id", avatar_val, sizeof(avatar_val)) == ESP_OK &&
        httpd_query_key_value(buf, "timeout", timeout_val, sizeof(timeout_val)) == ESP_OK) {
        
        strncpy(g_username, name_val, sizeof(g_username) - 1);
        g_username[sizeof(g_username) - 1] = '\0'; // 确保结尾对齐
        g_avatar_id = atoi(avatar_val);
        g_screensaver_timeout_sec = atoi(timeout_val);

        nvs_handle_t my_handle;
        if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_str(my_handle, "username", g_username);
            nvs_set_u8(my_handle, "avatar", g_avatar_id);
            nvs_set_u32(my_handle, "timeout", g_screensaver_timeout_sec);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }
        g_config_updated = true; 
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// 3. 异步 JSON 接口：接收客户端时间并动态配置 C 标准库时区
static esp_err_t sync_time_handler(httpd_req_t *req) {
    char content[100];
    int ret = httpd_req_recv(req, content, req->content_len);
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root) {
        cJSON *time_obj = cJSON_GetObjectItem(root, "time");
        cJSON *offset_obj = cJSON_GetObjectItem(root, "offset");
        if (time_obj && offset_obj) {
            struct timeval tv = { .tv_sec = (time_t)time_obj->valuedouble, .tv_usec = 0 };
            settimeofday(&tv, NULL);

            int target_hours = -offset_obj->valueint / 60;
            char tz_env[20];
            snprintf(tz_env, sizeof(tz_env), "GMT-%d", target_hours); 
            setenv("TZ", tz_env, 1);
            tzset();
            ESP_LOGI(TAG_NET, "Time Sync OK. TZ: %s", tz_env);
        }
        cJSON_Delete(root);
    }
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// 4. 初始化 AP 并启动 Web 监听
void start_softap_web_admin(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t size = sizeof(g_username);
        nvs_get_str(my_handle, "username", g_username, &size);
        nvs_get_u8(my_handle, "avatar", &g_avatar_id);
        nvs_get_u32(my_handle, "timeout", &g_screensaver_timeout_sec);
        nvs_close(my_handle);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = { .ssid = "AI-Passport-Setup", .channel = 1, .max_connection = 2, .authmode = WIFI_AUTH_OPEN },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_g = { .uri = "/", .method = HTTP_GET, .handler = get_handler };
        httpd_register_uri_handler(server, &uri_g);
        httpd_uri_t uri_p = { .uri = "/save", .method = HTTP_POST, .handler = post_handler };
        httpd_register_uri_handler(server, &uri_p);
        httpd_uri_t uri_s = { .uri = "/sync-time", .method = HTTP_POST, .handler = sync_time_handler };
        httpd_register_uri_handler(server, &uri_s);
    }
}
