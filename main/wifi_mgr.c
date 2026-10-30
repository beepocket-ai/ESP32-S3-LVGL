/*
 * WiFi 配网管理（参考 ESP-IDF 的 softap / provisioning 示例思路）
 *
 * 流程：
 *   1. 上电从 NVS 读取已保存的 SSID/密码；
 *   2. 有凭据 -> STA 连接路由器，拿到 IP 后启动 SNTP 对时；
 *   3. 无凭据，或连续连接失败 -> 开 SoftAP 热点（"ESP-Clock-xxxx"），
 *      手机连上后浏览器打开 http://192.168.4.1 填写 WiFi 名称/密码，
 *      保存进 NVS 并重启，重启后即以 STA 模式联网对时。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "wifi_mgr.h"

#define NVS_NS          "clock_wifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

#define AP_SSID_PREFIX  "ESP-Clock"
#define AP_IP_STR       "192.168.4.1"
#define NTP_SERVER      "ntp.aliyun.com"
#define TZ_STR          "CST-8"
#define RETRY_MAX       6

#define BOOT_BTN_GPIO       0       // 开发板 BOOT 键（GPIO0，按下为低）
#define BOOT_HOLD_RESET_MS  5000    // 按住 5 秒清除 WiFi 配置

static const char *TAG = "wifi_mgr";

static volatile wifi_mgr_state_t s_state;
static volatile bool s_time_synced;
static volatile bool s_softap_started;
static bool s_sntp_started;
static int s_retry;
static char s_ap_ssid[32] = AP_SSID_PREFIX;
static httpd_handle_t s_server;

/* ---------------- NVS 凭据 ---------------- */
static bool load_cred(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char tmp[65] = { 0 };
    size_t len = sizeof(tmp);
    bool ok = false;
    if (nvs_get_str(h, NVS_KEY_SSID, tmp, &len) == ESP_OK && strlen(tmp) > 0) {
        strlcpy(ssid, tmp, ssid_len);
        len = sizeof(tmp);
        tmp[0] = 0;
        if (nvs_get_str(h, NVS_KEY_PASS, tmp, &len) != ESP_OK) {
            tmp[0] = 0;
        }
        strlcpy(pass, tmp, pass_len);
        ok = true;
    }
    nvs_close(h);
    return ok;
}

static bool save_cred(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(h, NVS_KEY_SSID, ssid) == ESP_OK &&
              nvs_set_str(h, NVS_KEY_PASS, pass) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ---------------- 网页配网 ---------------- */
static const char *CONFIG_PAGE =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Clock - WiFi</title><style>"
    "body{font-family:-apple-system,Segoe UI,sans-serif;background:#0b1014;color:#e6edf3;margin:0;padding:24px}"
    "h1{font-size:20px;margin:0 0 6px}label{display:block;margin:16px 0 6px;color:#8a98a8;font-size:14px}"
    "input{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;border:1px solid #2a3542;"
    "background:#121a22;color:#e6edf3;font-size:16px}"
    "button{margin-top:22px;width:100%;padding:13px;border:0;border-radius:8px;background:#35d0ff;"
    "color:#04121a;font-size:16px;font-weight:600}"
    "p{color:#8a98a8;font-size:13px;line-height:1.6}"
    "</style></head><body><h1>WiFi 配置</h1>"
    "<p>输入要连接的路由器信息，保存后设备会重启并联网对时。</p>"
    "<form action='/save' method='get'>"
    "<label>WiFi 名称 (SSID)</label><input name='ssid' maxlength='32' required autocomplete='off'>"
    "<label>密码</label><input name='pass' type='password' maxlength='64' autocomplete='off'>"
    "<button type='submit'>保存并重启</button></form>"
    "<p>提示：若屏幕出现 CONNECTING 后又回到 SETUP，说明密码不对，重新连热点再配置即可。</p>"
    "</body></html>";

static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != 0 && j + 1 < dst_len; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            c = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        dst[j++] = c;
    }
    dst[j] = 0;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, CONFIG_PAGE, HTTPD_RESP_USE_STRLEN);
}

/* 任意未匹配路径（手机系统的联网探测请求等）都 302 跳转到配置页 */
static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP_STR "/");
    return httpd_resp_send(req, NULL, 0);
}

/* 简易 DNS 服务器：把收到的 A 查询全部解析为 192.168.4.1，用于触发系统 Captive Portal 弹窗 */
static uint8_t s_dns_rx[512];
static uint8_t s_dns_tx[600];

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in bind_addr = { 0 };
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(53);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "dns hijack on udp:53");

    uint8_t *buf = s_dns_rx;
    uint8_t *resp = s_dns_tx;
    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(s_dns_rx), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12 || (buf[2] & 0x80) != 0) {   // 忽略过短报文和响应报文
            continue;
        }
        int q_end = 12;
        while (q_end < n && buf[q_end] != 0) {
            q_end++;
        }
        q_end += 5;                             // 结束符 + qtype + qclass
        if (q_end > n) {
            continue;
        }

        memcpy(resp, buf, q_end);
        resp[2] = 0x81;                         // 标准响应 + RD
        resp[3] = 0x80;                         // RA
        resp[6] = 0x00;
        resp[7] = 0x01;                         // ANCOUNT = 1
        resp[8] = resp[9] = resp[10] = resp[11] = 0;

        int p = q_end;
        resp[p++] = 0xC0;                       // 指向问题中的域名
        resp[p++] = 0x0C;
        resp[p++] = 0x00;
        resp[p++] = 0x01;                       // TYPE A
        resp[p++] = 0x00;
        resp[p++] = 0x01;                       // CLASS IN
        resp[p++] = 0x00;
        resp[p++] = 0x00;
        resp[p++] = 0x00;
        resp[p++] = 0x3C;                       // TTL 60s
        resp[p++] = 0x00;
        resp[p++] = 0x04;                       // RDLENGTH 4
        resp[p++] = 192;
        resp[p++] = 168;
        resp[p++] = 4;
        resp[p++] = 1;

        sendto(sock, resp, p, 0, (struct sockaddr *)&from, from_len);
    }
}

/* 让 DHCP 下发的 DNS 指向本机，配合 dns_task 实现强制门户 */
static void setup_captive_dns(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap == NULL) {
        return;
    }
    esp_netif_dhcps_stop(ap);
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    esp_err_t err = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(ap);
    ESP_LOGI(TAG, "captive dns -> %s (%s)", AP_IP_STR, esp_err_to_name(err));
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char query[256] = { 0 };
    char raw[96] = { 0 };
    char ssid[40] = { 0 };
    char pass[72] = { 0 };

    if (httpd_req_get_url_query_len(req) > 0) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            if (httpd_query_key_value(query, "ssid", raw, sizeof(raw)) == ESP_OK) {
                url_decode(raw, ssid, sizeof(ssid));
            }
            if (httpd_query_key_value(query, "pass", raw, sizeof(raw)) == ESP_OK) {
                url_decode(raw, pass, sizeof(pass));
            }
        }
    }

    if (strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing ssid");
    }

    ESP_LOGI(TAG, "config received, ssid='%s'", ssid);
    if (!save_cred(ssid, pass)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "save failed");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body style='background:#0b1014;color:#e6edf3;"
                             "font-family:sans-serif;padding:24px'>已保存，设备重启中...</body></html>");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // 允许 "/*" 通配，兜底跳转到配置页
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http server start failed: %s", esp_err_to_name(err));
        return;
    }
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_GET, .handler = save_handler };
    httpd_uri_t uri_any = { .uri = "/*", .method = HTTP_GET, .handler = redirect_handler };
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_save);
    httpd_register_uri_handler(s_server, &uri_any);
    ESP_LOGI(TAG, "config web: http://%s", AP_IP_STR);
}

/* ---------------- SNTP ---------------- */
static void sntp_task(void *arg)
{
    (void)arg;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    if (esp_netif_sntp_init(&cfg) == ESP_OK &&
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) == ESP_OK) {
        setenv("TZ", TZ_STR, 1);
        tzset();
        s_time_synced = true;
        ESP_LOGI(TAG, "NTP synced");
    } else {
        ESP_LOGW(TAG, "NTP sync failed");
    }
    vTaskDelete(NULL);
}

static void start_sntp_once(void)
{
    if (!s_sntp_started) {
        s_sntp_started = true;
        xTaskCreate(sntp_task, "sntp", 4096, NULL, 4, NULL);
    }
}

/* ---------------- 热点配置模式 ---------------- */
static void softap_task(void *arg)
{
    (void)arg;

    esp_wifi_stop();                       // 未启动时会返回 NOT_STARTED，忽略
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    setup_captive_dns();

    start_httpd();
    xTaskCreate(dns_task, "dns", 4096, NULL, 3, NULL);
    s_state = WIFI_MGR_AP_CONFIG;
    ESP_LOGI(TAG, "AP ready: ssid='%s'  open http://%s", s_ap_ssid, AP_IP_STR);
    vTaskDelete(NULL);
}

/* ---------------- WiFi 事件 ---------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_state = WIFI_MGR_CONNECTING;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected, reason=%d", ev->reason);
        if (!s_softap_started && ++s_retry > RETRY_MAX) {
            s_softap_started = true;
            ESP_LOGW(TAG, "connect failed, switch to config AP");
            xTaskCreate(softap_task, "softap", 4096, NULL, 5, NULL);
        } else if (!s_softap_started) {
            s_state = WIFI_MGR_CONNECTING;
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_state = WIFI_MGR_CONNECTED;
        start_sntp_once();
    }
}

/* ---------------- 长按 BOOT 重置配网 ---------------- */
static void erase_cred(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void boot_key_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
            held_ms += 20;
            if (held_ms % 1000 == 0) {
                ESP_LOGW(TAG, "BOOT held %d/%d s, keep holding to reset wifi",
                         held_ms / 1000, BOOT_HOLD_RESET_MS / 1000);
            }
            if (held_ms >= BOOT_HOLD_RESET_MS) {
                ESP_LOGW(TAG, "BOOT held %d s: clear wifi config and restart",
                         BOOT_HOLD_RESET_MS / 1000);
                erase_cred();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
    }
}

/* ---------------- 对外接口 ---------------- */
void wifi_mgr_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // 热点名加 MAC 后缀，避免周围多个设备重名
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", AP_SSID_PREFIX, (unsigned)mac[4], (unsigned)mac[5]);

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (load_cred(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "connect to '%s'", ssid);
        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = strlen(pass) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        s_state = WIFI_MGR_CONNECTING;
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        ESP_LOGW(TAG, "no wifi config, start config AP");
        s_state = WIFI_MGR_AP_CONFIG;
        xTaskCreate(softap_task, "softap", 4096, NULL, 5, NULL);
    }

    xTaskCreate(boot_key_task, "boot_key", 3072, NULL, 4, NULL);
}

wifi_mgr_state_t wifi_mgr_state(void)
{
    return s_state;
}

bool wifi_mgr_time_synced(void)
{
    return s_time_synced;
}

void wifi_mgr_get_ap_ssid(char *buf, size_t len)
{
    strlcpy(buf, s_ap_ssid, len);
}
