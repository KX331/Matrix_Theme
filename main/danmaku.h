#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单条弹幕最大字节数（UTF-8，约 60 个汉字） */
#define DANMAKU_MSG_MAX   192
/** 内部缓存的最大弹幕条数 */
#define DANMAKU_QUEUE_LEN 20
/** WebSocket 地址最大长度 */
#define DANMAKU_URL_MAX   192

/** 弹幕连接状态 */
typedef enum {
    DANMAKU_STATE_IDLE = 0,    /**< 未配置地址，接收功能关闭 */
    DANMAKU_STATE_CONNECTING,  /**< 正在连接 / 握手中 */
    DANMAKU_STATE_CONNECTED,   /**< 已连接，等待消息 */
    DANMAKU_STATE_OFFLINE,     /**< 已断开，等待重连 */
    DANMAKU_STATE_ERROR,       /**< 地址非法或连接失败 */
} danmaku_state_t;

/** 初始化弹幕模块并启动接收任务（自动读取 NVS 中保存的地址） */
void danmaku_init(void);

/**
 * @brief 保存新的 WebSocket 地址并立即重连
 * @param url ws:// 或 wss:// 开头的完整地址；传空串表示关闭接收
 * @return 地址格式合法返回 true，非法返回 false（此时不保存）
 */
bool danmaku_set_url(const char* url);

/** 当前保存的地址（未配置时为空串） */
const char* danmaku_get_url(void);

/**
 * @brief 弹幕接收总开关（默认关闭）
 *
 * 关闭时不解析地址、不建立任何连接、不产生网络请求；
 * 打开后才会按 danmaku_get_url() 返回的地址开始接收。
 * 开关状态持久化在 NVS，重启后保持。
 */
bool danmaku_get_enabled(void);
void danmaku_set_enabled(bool on);

/** 是否把弹幕滚动显示到设备屏幕 */
bool danmaku_get_onscreen(void);
void danmaku_set_onscreen(bool on);

/** 当前连接状态 */
danmaku_state_t danmaku_get_state(void);
/** 连接状态的中文描述，可直接显示 */
const char* danmaku_get_state_str(void);
/** 累计收到的弹幕条数 */
unsigned danmaku_get_total(void);

/**
 * @brief 取出最早的一条尚未显示的弹幕，供 LVGL 任务调用
 * @return 取到返回 true 并写入 out；没有待显示弹幕返回 false
 */
bool danmaku_poll(char* out, size_t out_len);

/**
 * @brief 按序号读取历史弹幕（不消费），供网页端展示
 * @param seq 从 0 开始的消息序号，应满足 danmaku_get_total()-DANMAKU_QUEUE_LEN <= seq < danmaku_get_total()
 */
bool danmaku_get_by_seq(unsigned seq, char* out, size_t out_len);

/** 清空已缓存的消息 */
void danmaku_clear(void);

#ifdef __cplusplus
}
#endif
