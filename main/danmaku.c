#include "danmaku.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_tls.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char* TAG = "DANMAKU";

/* NVS 键（与 gui_main_web.c 共用 matrix_cfg 命名空间） */
#define DK_NVS_NS    "matrix_cfg"
#define DK_KEY_URL   "danmaku_url"
#define DK_KEY_ON    "danmaku_on"
#define DK_KEY_EN    "danmaku_enabled"   /* 弹幕接收总开关，默认关闭 */

#define DK_RECV_BUF  2048            /* 单帧接收缓冲 */
#define DK_TASK_STACK 6144
#define DK_HANDSHAKE_TIMEOUT_S 8     /* 连接/握手超时 */
#define DK_RECV_TIMEOUT_S      1     /* 每次 recv 超时，便于检查地址变更 */
#define DK_IDLE_LIMIT          90    /* 连续无数据多少秒后视为死连接 */
#define DK_RETRY_MS            5000  /* 断线重连间隔 */

/* ==================================================================
 * 一、极简 SHA-1（RFC 3174）与 Base64
 *     已用 RFC 6455 标准握手向量离线比对验证
 * ================================================================== */
typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;
} sha1_ctx_t;

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_ctx_t* c, const uint8_t* p) {
    uint32_t w[80];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | ((~b) & d);         k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_ctx_t* c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->total = 0; c->buflen = 0;
}

static void sha1_update(sha1_ctx_t* c, const uint8_t* p, size_t len) {
    c->total += len;
    while (len) {
        size_t n = 64 - c->buflen;
        if (n > len) n = len;
        memcpy(c->buf + c->buflen, p, n);
        c->buflen += n; p += n; len -= n;
        if (c->buflen == 64) { sha1_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha1_final(sha1_ctx_t* c, uint8_t out[20]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80, zero = 0;
    int i;
    sha1_update(c, &pad, 1);
    while (c->buflen != 56) sha1_update(c, &zero, 1);
    uint8_t lenb[8];
    for (i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, lenb, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

static int b64_encode(const uint8_t* in, size_t inlen, char* out, size_t outlen) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((inlen + 2) / 3) * 4 + 1;
    size_t i = 0, o = 0;
    if (outlen < need) return -1;
    while (i + 2 < inlen) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
        i += 3;
    }
    if (inlen - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (inlen - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
    return (int)o;
}

/* ==================================================================
 * 二、模块状态
 * ================================================================== */
typedef struct {
    char text[DANMAKU_MSG_MAX];
} dk_item_t;

static SemaphoreHandle_t s_mtx = NULL;
static char  s_url[DANMAKU_URL_MAX] = {0};
static int   s_state = DANMAKU_STATE_IDLE;
static bool  s_onscreen = true;
static bool  s_enabled = false;   /* 弹幕接收总开关：默认关闭，需在网页端手动打开 */
static volatile bool s_url_dirty = false;

/* 环形历史（供网页查看） + 消费型待显示队列（供屏幕滚动） */
static dk_item_t s_hist[DANMAKU_QUEUE_LEN];
static int       s_hist_pos = 0;
static unsigned  s_total = 0;
static unsigned  s_shown = 0;

/* 分片帧累积缓冲（仅接收任务使用） */
static uint8_t s_frame_acc[DK_RECV_BUF];

static void dk_set_state(int st) { s_state = st; }

static const char* dk_state_text(int st) {
    switch (st) {
        case DANMAKU_STATE_IDLE:       return "未启用";
        case DANMAKU_STATE_CONNECTING: return "连接中";
        case DANMAKU_STATE_CONNECTED:  return "已连接";
        case DANMAKU_STATE_OFFLINE:    return "已断开，等待重连";
        case DANMAKU_STATE_ERROR:      return "连接失败";
        default:                       return "未知";
    }
}

static void dk_push(const char* text) {
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strncpy(s_hist[s_hist_pos].text, text, DANMAKU_MSG_MAX - 1);
    s_hist[s_hist_pos].text[DANMAKU_MSG_MAX - 1] = 0;
    s_hist_pos = (s_hist_pos + 1) % DANMAKU_QUEUE_LEN;
    s_total++;
    xSemaphoreGive(s_mtx);
}

/* ==================================================================
 * 三、URL 解析
 * ================================================================== */
typedef struct {
    bool tls;
    char host[128];
    int  port;
    char path[128];
} ws_url_t;

static bool parse_ws_url(const char* url, ws_url_t* out) {
    memset(out, 0, sizeof(*out));
    const char* p = url;
    if (strncmp(p, "ws://", 5) == 0)       { out->tls = false; p += 5; }
    else if (strncmp(p, "wss://", 6) == 0) { out->tls = true;  p += 6; }
    else return false;

    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    size_t host_len;

    if (colon && (!slash || colon < slash)) {
        host_len = (size_t)(colon - p);
        const char* q = colon + 1;
        int port = 0;
        while (*q >= '0' && *q <= '9') { port = port * 10 + (*q - '0'); q++; }
        if (port <= 0 || port > 65535) return false;
        out->port = port;
    } else {
        host_len = slash ? (size_t)(slash - p) : strlen(p);
        out->port = out->tls ? 443 : 80;
    }
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, p, host_len);
    out->host[host_len] = 0;

    if (slash) snprintf(out->path, sizeof(out->path), "%s", slash);
    else       snprintf(out->path, sizeof(out->path), "/");
    return true;
}

/* ==================================================================
 * 四、连接抽象（ws 用裸 socket，wss 用 esp_tls）
 * ================================================================== */
typedef struct {
    bool       tls;
    int        fd;
    esp_tls_t* tls_h;
} ws_conn_t;

static int ws_write(ws_conn_t* c, const void* buf, size_t len) {
    if (c->tls) return esp_tls_conn_write(c->tls_h, buf, len);
    return send(c->fd, buf, len, 0);
}

/* 返回 >0 读到字节数；0 超时；-1 出错；-2 对端关闭 */
static int ws_read(ws_conn_t* c, void* buf, size_t len) {
    if (c->tls) {
        int r = esp_tls_conn_read(c->tls_h, buf, len);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
        if (r == 0) return -2;
        return r;
    }
    int r = recv(c->fd, buf, len, 0);
    if (r == 0) return -2;
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return r;
}

static int ws_write_all(ws_conn_t* c, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < len) {
        int r = ws_write(c, p + sent, len - sent);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return (int)sent;
}

static void ws_close(ws_conn_t* c) {
    if (c->tls && c->tls_h) {
        esp_tls_conn_destroy(c->tls_h);
        c->tls_h = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static bool ws_open(ws_conn_t* c, const ws_url_t* u) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;

    if (u->tls) {
        esp_tls_cfg_t cfg = {0};
        cfg.skip_common_name = true;  // 跳过证书 CN 校验（自签名证书场景）；正规部署请用 cert_bundle
        c->tls = true;
        c->tls_h = esp_tls_init();
        if (!c->tls_h) return false;
        int r = esp_tls_conn_new_sync(u->host, (int)strlen(u->host), u->port, &cfg, c->tls_h);
        if (r != 1) {
            ESP_LOGD(TAG, "TLS 连接失败: %d", r);
            ws_close(c);
            return false;
        }
        return true;
    }

    struct addrinfo hints;
    struct addrinfo* res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", u->port);
    if (getaddrinfo(u->host, portstr, &hints, &res) != 0 || !res) {
        ESP_LOGD(TAG, "域名解析失败: %s", u->host);
        return false;
    }
    c->fd = socket(res->ai_family, res->ai_socktype, 0);
    if (c->fd < 0) { freeaddrinfo(res); return false; }

    struct timeval tv;
    tv.tv_sec = DK_HANDSHAKE_TIMEOUT_S; tv.tv_usec = 0;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int rc = connect(c->fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        ESP_LOGD(TAG, "TCP 连接失败: errno=%d", errno);
        ws_close(c);
        return false;
    }
    /* 连接后改用短超时，便于周期性检查地址是否被改动 */
    tv.tv_sec = DK_RECV_TIMEOUT_S; tv.tv_usec = 0;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

/* ==================================================================
 * 五、WebSocket 握手（RFC 6455）
 * ================================================================== */
/* 在 HTTP 响应头中按名称取值（头名大小写不敏感） */
static bool ws_header_value(const char* resp, const char* name,
                            char* out, size_t out_len) {
    size_t nl = strlen(name);
    const char* p = resp;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char* v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t i = 0;
            while (v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < out_len) {
                out[i] = v[i];
                i++;
            }
            out[i] = 0;
            return true;
        }
    }
    return false;
}

/* 按 RFC 6455 计算期望的 Sec-WebSocket-Accept 值 */
static bool ws_expected_accept(const char* key, char* out, size_t out_len) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char cat[80];
    int n = snprintf(cat, sizeof(cat), "%s%s", key, GUID);
    if (n <= 0 || n >= (int)sizeof(cat)) return false;
    uint8_t dg[20];
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t*)cat, (size_t)n);
    sha1_final(&ctx, dg);
    return b64_encode(dg, 20, out, out_len) > 0;
}

static bool ws_handshake(ws_conn_t* c, const ws_url_t* u) {
    uint8_t nonce[16];
    int i;
    for (i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(nonce + i, &r, 4);
    }
    char key[32];
    if (b64_encode(nonce, 16, key, sizeof(key)) < 0) return false;

    char req[640];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        u->path, u->host, u->port, key);
    if (n <= 0 || n >= (int)sizeof(req)) return false;
    if (ws_write_all(c, req, (size_t)n) != n) return false;

    char resp[512];
    int got = 0, idle = 0;
    resp[0] = 0;
    while (got < (int)sizeof(resp) - 1) {
        int r = ws_read(c, resp + got, sizeof(resp) - 1 - (size_t)got);
        if (r == 0) {
            if (++idle > DK_HANDSHAKE_TIMEOUT_S * 20) return false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (r < 0) return false;
        got += r;
        resp[got] = 0;
        if (strstr(resp, "\r\n\r\n")) break;
    }
    if (!strstr(resp, "\r\n\r\n")) return false;
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 &&
        strncmp(resp, "HTTP/1.0 101", 12) != 0) {
        ESP_LOGD(TAG, "握手被服务器拒绝: %.48s", resp);
        return false;
    }

    /* 校验 Sec-WebSocket-Accept，确认对端确实是合规的 WebSocket 服务 */
    char expect[40];
    if (!ws_expected_accept(key, expect, sizeof(expect))) return false;
    char accept_val[64];
    if (!ws_header_value(resp, "Sec-WebSocket-Accept", accept_val, sizeof(accept_val))) {
        ESP_LOGD(TAG, "响应缺少 Sec-WebSocket-Accept 头，放弃连接");
        return false;
    }
    if (strcmp(accept_val, expect) != 0) {
        ESP_LOGD(TAG, "握手校验失败: 期望 %s，收到 %s", expect, accept_val);
        return false;
    }
    return true;
}

/* ==================================================================
 * 六、帧读取
 * ================================================================== */
#define DK_OP_CONT  0x0
#define DK_OP_TEXT  0x1
#define DK_OP_BIN   0x2
#define DK_OP_CLOSE 0x8
#define DK_OP_PING  0x9
#define DK_OP_PONG  0xA

static int ws_read_exact(ws_conn_t* c, uint8_t* buf, size_t n) {
    size_t got = 0;
    int idle = 0;
    while (got < n) {
        if (s_url_dirty) return -1;
        int r = ws_read(c, buf + got, n - got);
        if (r > 0) { got += (size_t)r; idle = 0; continue; }
        if (r == 0) {
            if (++idle > DK_IDLE_LIMIT) return -1;
            continue;
        }
        return -1;
    }
    return (int)got;
}

/**
 * @brief 读取一条完整消息
 * @return 1 取到消息（写入 out）；0 收到控制帧但无消息；-1 连接异常
 */
static int ws_read_message(ws_conn_t* c, char* out, size_t out_len) {
    size_t acc_len = 0;

    for (;;) {
        uint8_t bh[2];
        if (ws_read_exact(c, bh, 2) != 2) return -1;

        int fin = bh[0] & 0x80;
        int op  = bh[0] & 0x0F;
        uint64_t plen = (uint64_t)(bh[1] & 0x7F);

        if (plen == 126) {
            uint8_t e[2];
            if (ws_read_exact(c, e, 2) != 2) return -1;
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127) {
            uint8_t e[8];
            if (ws_read_exact(c, e, 8) != 8) return -1;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | e[i];
        }

        uint8_t mask[4] = {0, 0, 0, 0};
        bool masked = (bh[1] & 0x80) != 0;
        if (masked && ws_read_exact(c, mask, 4) != 4) return -1;

        if (plen > sizeof(s_frame_acc)) {
            ESP_LOGD(TAG, "单帧过大(%u 字节)，丢弃", (unsigned)plen);
            return -1;
        }

        /* 读 payload 并按需解除掩码 */
        if (masked) {
            uint8_t tmp[256];
            uint64_t off = 0;
            while (off < plen) {
                size_t chunk = (size_t)(plen - off);
                if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
                if (ws_read_exact(c, tmp, chunk) != (int)chunk) return -1;
                for (size_t i = 0; i < chunk; i++)
                    s_frame_acc[off + i] = tmp[i] ^ mask[(off + i) & 3];
                off += chunk;
            }
        } else {
            if (plen > 0 && ws_read_exact(c, s_frame_acc, (size_t)plen) != (int)plen) return -1;
        }

        if (op == DK_OP_PING) {
            /* 回 Pong，payload 原样带回 */
            uint8_t hdr[2];
            hdr[0] = 0x8A;
            hdr[1] = (uint8_t)((plen < 126) ? plen : 126);
            ws_write_all(c, hdr, 2);
            if (plen >= 126) {
                uint8_t e[2] = { (uint8_t)(plen >> 8), (uint8_t)plen };
                ws_write_all(c, e, 2);
            }
            if (plen > 0) ws_write_all(c, s_frame_acc, (size_t)plen);
            continue;
        }
        if (op == DK_OP_PONG) continue;
        if (op == DK_OP_CLOSE) return -1;
        if (op == DK_OP_BIN) continue;   /* 二进制帧忽略 */

        if (op == DK_OP_TEXT || op == DK_OP_CONT) {
            size_t room = sizeof(s_frame_acc) - 1 - acc_len;
            size_t take = ((size_t)plen < room) ? (size_t)plen : room;
            if (take > 0) {
                memmove(s_frame_acc + acc_len, s_frame_acc, take);
                acc_len += take;
            }
            if (fin) {
                size_t n = acc_len;
                if (n >= out_len) n = out_len - 1;
                memcpy(out, s_frame_acc, n);
                out[n] = 0;
                return 1;
            }
        }
    }
}

/* ==================================================================
 * 七、消息文本提取（兼容纯文本与常见 JSON 弹幕格式）
 * ================================================================== */
static int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex4(const char* s, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/** 把 JSON 字符串体（不含首尾引号）还原为 UTF-8 文本 */
static size_t json_unescape(const char* s, size_t slen, char* out, size_t out_len) {
    size_t i = 0, o = 0;
    while (i < slen && o + 4 < out_len) {
        char c = s[i];
        if (c != '\\') { out[o++] = c; i++; continue; }
        if (i + 1 >= slen) break;
        char e = s[i + 1];
        i += 2;
        switch (e) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case '/': out[o++] = '/';  break;
            case '\\': out[o++] = '\\'; break;
            case '"': out[o++] = '"';  break;
            case 'u': {
                uint32_t cp = 0;
                if (i + 4 > slen || hex4(s + i, &cp) != 0) break;
                i += 4;
                /* UTF-16 代理对 */
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= slen &&
                    s[i] == '\\' && s[i + 1] == 'u') {
                    uint32_t low = 0;
                    if (hex4(s + i + 2, &low) == 0 && low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                o += (size_t)utf8_encode(cp, out + o);
                break;
            }
            default: out[o++] = e; break;
        }
    }
    out[o] = 0;
    return o;
}

/** 在 JSON 中查找 "key":"value"，成功则还原 value 并返回 true */
static bool json_get_str(const char* s, const char* key, char* out, size_t out_len) {
    char pat[32];
    int pl = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pl <= 0 || pl >= (int)sizeof(pat)) return false;

    const char* p = strstr(s, pat);
    if (!p) return false;
    p += pl;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;

    const char* st = p;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '"') break;
        p++;
    }
    json_unescape(st, (size_t)(p - st), out, out_len);
    return out[0] != 0;
}

static void json_find_any(const char* s, const char* const* keys, int n,
                          char* out, size_t out_len) {
    for (int i = 0; i < n; i++) {
        if (json_get_str(s, keys[i], out, out_len)) return;
    }
}

static void dk_extract_text(const char* in, char* out, size_t out_len) {
    if (!in || out_len == 0) { if (out && out_len) out[0] = 0; return; }

    char content[DANMAKU_MSG_MAX] = {0};
    char nick[64] = {0};
    bool has = false;

    if (in[0] == '{') {
        static const char* const ckeys[] = { "content", "danmaku", "text", "msg", "message" };
        static const char* const nkeys[] = { "nickname", "nick", "uname", "user_name", "username" };
        json_find_any(in, ckeys, 5, content, sizeof(content));
        has = content[0] != 0;
        if (has) json_find_any(in, nkeys, 5, nick, sizeof(nick));
    }

    /* 手动拼接，避免 snprintf 触发格式截断告警 */
    size_t pos = 0;
    if (has) {
        if (nick[0]) {
            size_t n = strlen(nick);
            if (n > out_len - 4) n = out_len - 4;
            memcpy(out, nick, n);
            pos = n;
            out[pos++] = ':';
            out[pos++] = ' ';
        }
        size_t avail = (out_len > pos + 1) ? (out_len - pos - 1) : 0;
        size_t cn = strlen(content);
        if (cn > avail) cn = avail;
        memcpy(out + pos, content, cn);
        out[pos + cn] = 0;
        return;
    }

    /* 纯文本：去掉首尾空白后原样输出 */
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') in++;
    size_t n = strlen(in);
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t' ||
                     in[n - 1] == '\r' || in[n - 1] == '\n')) n--;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

/* ==================================================================
 * 八、接收任务
 * ================================================================== */
static void danmaku_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "弹幕接收任务已启动");

    int fail_cnt = 0;   /* 连续失败计数：用于日志节流，避免串口被重试信息刷屏 */

    for (;;) {
        char url[DANMAKU_URL_MAX];
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        strncpy(url, s_url, sizeof(url) - 1);
        url[sizeof(url) - 1] = 0;
        xSemaphoreGive(s_mtx);

        if (!s_enabled) {
            /* 总开关关闭：不解析地址、不建连、不产生任何网络请求 */
            dk_set_state(DANMAKU_STATE_IDLE);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (url[0] == 0) {
            dk_set_state(DANMAKU_STATE_IDLE);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ws_url_t u;
        if (!parse_ws_url(url, &u)) {
            ESP_LOGW(TAG, "地址格式非法: %s", url);
            dk_set_state(DANMAKU_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        dk_set_state(DANMAKU_STATE_CONNECTING);
        ESP_LOGI(TAG, "正在连接 %s%s:%d%s", u.tls ? "wss://" : "ws://", u.host, u.port, u.path);

        ws_conn_t c;
        if (!ws_open(&c, &u) || !ws_handshake(&c, &u)) {
            ws_close(&c);
            dk_set_state(DANMAKU_STATE_ERROR);
            /* 日志节流：每 5 次失败才打一行汇总（首次必打），避免刷屏。
               具体失败原因已由底层 ESP_LOGD 记录，需要排查时把日志级别开到 DEBUG 即可。 */
            fail_cnt++;
            if (fail_cnt == 1 || fail_cnt % 5 == 0) {
                ESP_LOGW(TAG, "弹幕服务器连接失败 %d 次（地址 %s），%d 秒后重试",
                         fail_cnt, url, DK_RETRY_MS / 1000);
            }
            for (int i = 0; i < DK_RETRY_MS / 100 && !s_url_dirty; i++) vTaskDelay(pdMS_TO_TICKS(100));
            s_url_dirty = false;
            continue;
        }

        ESP_LOGI(TAG, "WebSocket 已连接，等待弹幕");
        dk_set_state(DANMAKU_STATE_CONNECTED);
        fail_cnt = 0;
        s_url_dirty = false;

        while (!s_url_dirty) {
            char raw[DANMAKU_MSG_MAX];
            int r = ws_read_message(&c, raw, sizeof(raw));
            if (r < 0) break;
            if (r > 0 && raw[0]) {
                char text[DANMAKU_MSG_MAX];
                dk_extract_text(raw, text, sizeof(text));
                if (text[0]) {
                    dk_push(text);
                    ESP_LOGI(TAG, "收到弹幕: %s", text);
                }
            }
        }
        ws_close(&c);

        if (!s_url_dirty) {
            dk_set_state(DANMAKU_STATE_OFFLINE);
            fail_cnt++;
            if (fail_cnt == 1 || fail_cnt % 5 == 0) {
                ESP_LOGW(TAG, "弹幕连接断开 %d 次，%d 秒后重连", fail_cnt, DK_RETRY_MS / 1000);
            }
            for (int i = 0; i < DK_RETRY_MS / 100 && !s_url_dirty; i++) vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_url_dirty = false;
    }
}

/* ==================================================================
 * 九、NVS 读写
 * ================================================================== */
static void dk_load_nvs(void) {
    nvs_handle_t h;
    s_url[0] = 0;
    s_enabled = false;              /* 读不到开关记录时也保持"默认关闭" */
    if (nvs_open(DK_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_url);
        if (nvs_get_str(h, DK_KEY_URL, s_url, &len) != ESP_OK) s_url[0] = 0;
        int8_t on = 1;
        if (nvs_get_i8(h, DK_KEY_ON, &on) == ESP_OK) s_onscreen = (on != 0);
        int8_t en = 0;
        if (nvs_get_i8(h, DK_KEY_EN, &en) == ESP_OK) s_enabled = (en != 0);
        nvs_close(h);
    }
}

static void dk_save_enabled_nvs(bool en) {
    nvs_handle_t h;
    if (nvs_open(DK_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i8(h, DK_KEY_EN, en ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void dk_save_url_nvs(const char* url) {
    nvs_handle_t h;
    if (nvs_open(DK_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, DK_KEY_URL, url);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void dk_save_on_nvs(bool on) {
    nvs_handle_t h;
    if (nvs_open(DK_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i8(h, DK_KEY_ON, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ==================================================================
 * 十、对外接口
 * ================================================================== */
void danmaku_init(void) {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    dk_load_nvs();
    if (s_url[0]) ESP_LOGI(TAG, "已加载弹幕地址: %s", s_url);
    xTaskCreate(danmaku_task, "danmaku", DK_TASK_STACK, NULL, 4, NULL);
}

bool danmaku_set_url(const char* url) {
    if (url && url[0]) {
        ws_url_t u;
        if (!parse_ws_url(url, &u)) return false;
    }
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strncpy(s_url, url ? url : "", sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = 0;
    xSemaphoreGive(s_mtx);

    dk_save_url_nvs(s_url);
    s_url_dirty = true;   /* 通知接收任务立即按新地址重连 */
    ESP_LOGI(TAG, "弹幕地址已更新: %s (总开关 %s)", s_url[0] ? s_url : "(空)",
             s_enabled ? "开" : "关");
    return true;
}

/* ---- 弹幕接收总开关：默认关闭，只有打开后才建立 WebSocket 连接 ---- */
bool danmaku_get_enabled(void) { return s_enabled; }

void danmaku_set_enabled(bool on) {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_enabled = on;
    if (s_mtx) xSemaphoreGive(s_mtx);
    dk_save_enabled_nvs(on);
    s_url_dirty = true;   /* 让接收任务立刻按新的开关状态生效（打开即连 / 关闭即断） */
    ESP_LOGI(TAG, "弹幕接收总开关: %s", on ? "打开" : "关闭");
}

const char* danmaku_get_url(void) { return s_url; }
bool danmaku_get_onscreen(void)   { return s_onscreen; }

void danmaku_set_onscreen(bool on) {
    s_onscreen = on;
    dk_save_on_nvs(on);
}

danmaku_state_t danmaku_get_state(void) { return (danmaku_state_t)s_state; }
const char* danmaku_get_state_str(void) { return dk_state_text(s_state); }
unsigned danmaku_get_total(void)        { return s_total; }

bool danmaku_poll(char* out, size_t out_len) {
    bool ok = false;
    if (!s_mtx || !out || out_len == 0) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_shown < s_total) {
        if (s_total - s_shown > (unsigned)DANMAKU_QUEUE_LEN)
            s_shown = s_total - DANMAKU_QUEUE_LEN;
        strlcpy(out, s_hist[s_shown % DANMAKU_QUEUE_LEN].text, out_len);
        s_shown++;
        ok = true;
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

bool danmaku_get_by_seq(unsigned seq, char* out, size_t out_len) {
    bool ok = false;
    if (!s_mtx || !out || out_len == 0) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (seq < s_total && (s_total - seq) <= (unsigned)DANMAKU_QUEUE_LEN) {
        strlcpy(out, s_hist[seq % DANMAKU_QUEUE_LEN].text, out_len);
        ok = true;
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

void danmaku_clear(void) {
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_total = 0;
    s_shown = 0;
    s_hist_pos = 0;
    memset(s_hist, 0, sizeof(s_hist));
    xSemaphoreGive(s_mtx);
}
