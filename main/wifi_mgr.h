#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_AP_CONFIG = 0,   /* 未配网/连接失败：已开配置热点，等待手机网页配置 */
    WIFI_MGR_CONNECTING,      /* 已保存凭据，正在连接路由器 */
    WIFI_MGR_CONNECTED,       /* 已连接并拿到 IP（NTP 可能还在同步） */
} wifi_mgr_state_t;

/* 启动 WiFi 管理：有 NVS 凭据则连接路由器，否则开 "ESP-Clock-xxxx" 配置热点 + 网页配网 */
void wifi_mgr_start(void);

/* 清除已保存的 WiFi 凭据并重启（回到配网模式）；由 BOOT 长按 5 秒触发 */
void wifi_mgr_clear_config(void);

wifi_mgr_state_t wifi_mgr_state(void);
bool wifi_mgr_time_synced(void);
void wifi_mgr_get_ap_ssid(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
