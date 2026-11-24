#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip4_addr.h"
#include "dhcpserver/dhcpserver.h"
#include "cJSON.h"
#include "app_mywifi.h"

static const char *TAG = "camera_wifi";

#define WIFI_STORE_NAMESPACE "wifi_store"
#define KEY_STA_SSID         "sta_ssid"
#define KEY_STA_PASSWD       "sta_passwd"

/* Fast user feedback: one retry (1 s apart), then report failure and fall back
 * to the AP; unattended recovery continues every 15 s for a while, then stops
 * so a wrong password cannot keep the radio hopping forever. */
#define STA_FAST_RETRY_LIMIT        1
#define STA_RETRY_INTERVAL_MS       1000
#define STA_SLOW_RETRY_INTERVAL_MS  15000
#define STA_SLOW_RETRY_LIMIT        5
/* Cross-band APSTA stalls the client's link for ~5 s and its TCP stack then
 * backs off for another ~10 s, so the provisioning page needs a 30 s window
 * to fetch the STA IP and show the success dialog before the AP disappears. */
#define AP_STOP_GRACE_MS            30000
#define AP_STOP_GRACE_SEC           (AP_STOP_GRACE_MS / 1000)

static bool s_sta_connected = false;
static bool s_sta_connecting = false;
static bool s_ap_active = false;
static volatile uint8_t s_prepare_channel = 0;
static bool s_ap_stop_on_sta_up = false;
static bool s_creds_dirty = false;
static bool s_manual_disconnect = false;
static bool s_switching_creds = false;
static int64_t s_switching_since_us = 0;
static bool s_sta_pinned = false;
static bool s_fast_retry_exhausted = false;
static char s_sta_ip[32] = "";
static char s_sta_ssid[33] = "";
static char s_pending_ssid[33] = "";
static char s_pending_passwd[65] = "";
static uint8_t s_pending_bssid[6] = { 0 };
static uint8_t s_pending_channel = 0;
static bool s_pending_bssid_set = false;
static int  s_disconnect_reason = 0;
static int  s_retry_attempts = 0;
static int  s_slow_retry_count = 0;
static int64_t s_retry_at_us = 0;
static int64_t s_ap_stop_at_us = 0;
static int64_t s_ap_close_at_us = 0;
static int64_t s_connect_watchdog_at_us = 0;
static int64_t s_status_print_at_us = 0;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static char s_ap_ssid[33] = AP_SSID_PREFIX;
static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t s_wifi_lock = NULL;
static SemaphoreHandle_t s_scan_req = NULL;
static SemaphoreHandle_t s_scan_cache_lock = NULL;
static char *s_scan_cache = NULL;
static volatile bool s_scan_running = false;

/* A wedged C5 returns ESP_ERR_WIFI_SSID in ~20 ms on every connect; three in a
 * row trigger a recovery reboot into AP provisioning. The flag must survive
 * esp_restart(), so keep it in RTC no-init memory with a magic guard. */
#define FORCE_AP_MAGIC 0x41505F52u
RTC_NOINIT_ATTR static uint32_t s_rtc_force_ap;
static int s_instant_ssid_fail = 0;

static void wifi_note_connect_result(esp_err_t err, int64_t elapsed_us)
{
    if (err == ESP_ERR_WIFI_SSID && elapsed_us < 100000) {
        if (++s_instant_ssid_fail >= 3) {
            s_instant_ssid_fail = 0;
            ESP_LOGE(TAG, "C5 STA config wedged (3x instant ESP_ERR_WIFI_SSID); rebooting into AP provisioning");
            s_rtc_force_ap = FORCE_AP_MAGIC;
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        }
    } else {
        s_instant_ssid_fail = 0;
    }
}

#define DEVICE_CFG_NAMESPACE        "device_cfg"
#define KEY_DEVICE_HOSTNAME         "device_hostname"
#define DEVICE_HOSTNAME_DEFAULT     "P4C5CAM"
#define DEVICE_HOSTNAME_MAX_LEN     32

static char s_device_hostname[DEVICE_HOSTNAME_MAX_LEN + 1] = DEVICE_HOSTNAME_DEFAULT;

const char *app_mywifi_get_device_hostname(void)
{
    return s_device_hostname;
}

static bool device_hostname_is_valid(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > DEVICE_HOSTNAME_MAX_LEN) return false;
    for (const char *p = name; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return false;
    }
    return true;
}

static void load_device_hostname(void)
{
    nvs_handle_t handle;
    if (nvs_open(DEVICE_CFG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;

    char stored[DEVICE_HOSTNAME_MAX_LEN + 1] = { 0 };
    size_t len = sizeof(stored);
    if (nvs_get_str(handle, KEY_DEVICE_HOSTNAME, stored, &len) == ESP_OK &&
        device_hostname_is_valid(stored)) {
        snprintf(s_device_hostname, sizeof(s_device_hostname), "%s", stored);
    }
    nvs_close(handle);
}

#define EV_GOT_IP      BIT0
#define EV_STA_DOWN    BIT1
#define EV_CONNECT_REQ BIT2
#define EV_RESTORE_AP  BIT3
#define EV_ENTER_PROV  BIT4
#define EV_PREPARE_AP  BIT5

bool app_mywifi_is_sta_connected(void)
{
    return s_sta_connected;
}

bool app_mywifi_is_sta_connecting(void)
{
    /* Past the fast-retry budget the AP is up for re-provisioning; report the
     * failure instead of an endless "connecting" state. Internal slow retries
     * continue in the background. */
    return s_sta_connecting && !s_fast_retry_exhausted;
}

bool app_mywifi_is_ap_active(void)
{
    return s_ap_active;
}

const char* app_mywifi_get_sta_ip(void)
{
    return s_sta_ip;
}

const char* app_mywifi_get_sta_ssid(void)
{
    return s_sta_ssid;
}

const char* app_mywifi_get_ap_ip(void)
{
    return AP_IP_DEFAULT;
}

const char* app_mywifi_get_ap_ssid(void)
{
    return s_ap_ssid;
}

int app_mywifi_get_disconnect_reason(void)
{
    return s_disconnect_reason;
}

const char* app_mywifi_get_disconnect_message(void)
{
    if (s_sta_connected) {
        return "Wi-Fi已连接 / Wi-Fi connected";
    }
    if (app_mywifi_is_sta_connecting()) {
        if (s_disconnect_reason == 15 || s_disconnect_reason == 202 || s_disconnect_reason == 204) {
            return "正在重试：密码错误或握手超时 / Retrying: wrong password";
        } else if (s_disconnect_reason == 201) {
            return "正在重试：未找到热点 / Retrying: AP not found";
        } else if (s_disconnect_reason > 0) {
            return "正在重试连接 / Retrying...";
        }
        return "正在连接路由器... / Connecting...";
    }
    if (s_disconnect_reason == 15 || s_disconnect_reason == 202 || s_disconnect_reason == 204) {
        return "密码错误或握手超时 / Wrong password or handshake timeout";
    } else if (s_disconnect_reason == 201) {
        return "未找到指定Wi-Fi热点 / AP not found";
    } else if (s_disconnect_reason > 0) {
        return "Wi-Fi连接断开或认证失败 / Disconnected or auth failed";
    }
    return s_ap_active ? "AP待配网模式 / AP provisioning mode" : "未连接 / Not connected";
}

esp_err_t app_mywifi_save_sta_creds(const char *ssid, const char *passwd)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, KEY_STA_SSID, ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_STA_PASSWD, passwd ? passwd : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved Wi-Fi credentials to NVS: SSID='%s'", ssid ? ssid : "");
    return err;
}

esp_err_t app_mywifi_load_sta_creds(char *ssid, size_t ssid_len, char *passwd, size_t passwd_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_str(handle, KEY_STA_SSID, ssid, &ssid_len);
    if (err == ESP_OK && passwd && passwd_len > 0) {
        err = nvs_get_str(handle, KEY_STA_PASSWD, passwd, &passwd_len);
    }
    nvs_close(handle);
    return err;
}

static void wifi_clear_driver_sta_config(void)
{
    wifi_config_t sta_cfg = { 0 };
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
}

esp_err_t app_mywifi_clear_sta_creds(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, KEY_STA_SSID);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_erase_key(handle, KEY_STA_PASSWD);
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return err;

    bool sta_was_active = s_sta_connected || s_sta_connecting;
    s_manual_disconnect = true;
    s_switching_creds = false;
    s_switching_since_us = 0;
    s_sta_connected = false;
    s_sta_connecting = false;
    s_retry_attempts = 0;
    s_slow_retry_count = 0;
    s_retry_at_us = 0;
    s_ap_stop_at_us = 0;
    s_connect_watchdog_at_us = 0;
    s_creds_dirty = false;
    s_sta_pinned = false;
    s_fast_retry_exhausted = false;
    s_sta_ip[0] = '\0';
    s_sta_ssid[0] = '\0';
    s_disconnect_reason = 0;
    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    memset(s_pending_passwd, 0, sizeof(s_pending_passwd));
    memset(s_pending_bssid, 0, sizeof(s_pending_bssid));
    s_pending_channel = 0;
    s_pending_bssid_set = false;

    /* Reset the driver-side STA config as well, otherwise the old SSID stays in RAM. */
    if (sta_was_active) {
        s_switching_creds = true;
        s_switching_since_us = esp_timer_get_time();
        esp_wifi_disconnect();
    }
    wifi_clear_driver_sta_config();

    if (s_events) {
        xEventGroupSetBits(s_events, EV_RESTORE_AP);
    }
    ESP_LOGI(TAG, "Cleared Wi-Fi credentials; returning to AP provisioning mode");
    return ESP_OK;
}

esp_err_t app_mywifi_restore_ap(void)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    xEventGroupSetBits(s_events, EV_RESTORE_AP);
    return ESP_OK;
}

esp_err_t app_mywifi_force_recovery_reboot(void)
{
    ESP_LOGW(TAG, "Forcing recovery reboot into AP provisioning");
    s_rtc_force_ap = FORCE_AP_MAGIC;
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
    return ESP_OK;
}

/* Called when the user picks a 2.4 GHz SSID in the provisioning page. Moving
 * the AP now hides the SoftAP restart inside the password-typing window, so the
 * submit itself does not drop the provisioning client. */
esp_err_t app_mywifi_prepare_ap_channel(uint8_t channel)
{
    if (channel < 1 || channel > 14) return ESP_ERR_INVALID_ARG;
    if (!s_events) return ESP_ERR_INVALID_STATE;
    s_prepare_channel = channel;
    xEventGroupSetBits(s_events, EV_PREPARE_AP);
    return ESP_OK;
}

esp_err_t app_mywifi_enter_provisioning(void)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    xEventGroupSetBits(s_events, EV_ENTER_PROV);
    return ESP_OK;
}

static void wifi_enable_dual_band(void)
{
#if CONFIG_SOC_WIFI_SUPPORT_5G || CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G
    esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Dual-band mode unavailable: %s", esp_err_to_name(err));
    }
#endif
}

static esp_err_t ensure_ap_netif(void)
{
    if (s_ap_netif) return ESP_OK;
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return ESP_FAIL;

    esp_netif_ip_info_t ap_ip_info;
    IP4_ADDR(&ap_ip_info.ip, 192, 168, 5, 1);
    IP4_ADDR(&ap_ip_info.gw, 192, 168, 5, 1);
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ap_ip_info);

    /* Start the pool above .2 to avoid stale leases colliding with a second
     * adapter on the same PC during test/bench switching. */
    dhcps_lease_t lease = { .enable = true };
    IP4_ADDR(&lease.start_ip, 192, 168, 5, 10);
    IP4_ADDR(&lease.end_ip, 192, 168, 5, 100);
    esp_err_t err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_REQUESTED_IP_ADDRESS,
                                           &lease, sizeof(lease));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set DHCP lease pool: %s", esp_err_to_name(err));
    }

    /* Explicitly disable the DHCP DNS option: no local resolver/hijack. */
    uint8_t offer_dns = 0;
    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns, sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable DHCP DNS offer: %s", esp_err_to_name(err));
    }

    esp_netif_dhcps_start(s_ap_netif);
    ESP_LOGI(TAG, "Captive portal disabled (no DNS hijack, option 114 or HTTP redirects)");
    return ESP_OK;
}

static void disable_ap_dhcp_dns(void)
{
    if (!s_ap_netif) return;
    esp_netif_dhcps_stop(s_ap_netif);
    uint8_t offer_dns = 0;
    esp_err_t err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable DHCP DNS offer: %s", esp_err_to_name(err));
    }
    esp_netif_dhcps_start(s_ap_netif);
}

static void ensure_ap_ssid(void)
{
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X%02X",
                 AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", AP_SSID_PREFIX);
        ESP_LOGW(TAG, "Failed to read AP MAC; using fallback SSID '%s'", s_ap_ssid);
    }
}

static esp_err_t configure_ap(uint8_t channel)
{
    wifi_config_t ap_config = { 0 };
    size_t ssid_len = strnlen(s_ap_ssid, sizeof(ap_config.ap.ssid));
    memcpy(ap_config.ap.ssid, s_ap_ssid, ssid_len);
    ap_config.ap.ssid_len = ssid_len;
    ap_config.ap.channel = channel ? channel : AP_CHANNEL_DEFAULT;
    ap_config.ap.max_connection = 4;
    ap_config.ap.beacon_interval = 100;
    ap_config.ap.dtim_period = 2;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static esp_err_t wifi_apply_ap_2g(void)
{
    /* Keep the provisioning AP on 2.4 GHz. It can then stay up while the STA
     * connects to a 5 GHz router (cross-band APSTA), so the provisioning page
     * survives the whole verification and can show the STA IP. */
    wifi_enable_dual_band();

    wifi_bandwidths_t bandwidths = {
        .ghz_2g = WIFI_BW20,
        .ghz_5g = WIFI_BW20,
    };
    esp_err_t err = esp_wifi_set_bandwidths(WIFI_IF_AP, &bandwidths);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set AP 20 MHz bandwidth: %s", esp_err_to_name(err));
    }

    /* Keep 11n on 2.4 GHz for broad client compatibility. */
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC,
    };
    err = esp_wifi_set_protocols(WIFI_IF_AP, &protocols);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set AP protocols: %s", esp_err_to_name(err));
    }

    err = configure_ap(AP_CHANNEL_2G_DEFAULT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "2.4G AP config failed: %s", esp_err_to_name(err));
    }

    uint8_t channel = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&channel, &second) == ESP_OK) {
        ESP_LOGI(TAG, "AP 2.4G requested ch %d, active ch %u (%s)",
                 AP_CHANNEL_2G_DEFAULT, channel, channel > 14 ? "5G" : "2.4G");
        if (channel > 14) {
            ESP_LOGW(TAG, "SoftAP did not enter 2.4G (active ch %u)", channel);
        }
    }
    return err;
}

static void wifi_wait_sta_link_down(void)
{
    for (int i = 0; i < 30; ++i) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* Apply the 2.4G AP only after the STA link is really down. Without the wait the
 * C5 keeps the AP on the STA band (observed 5G ch36 after provision_ap). */
static esp_err_t wifi_apply_ap_2g_stable(void)
{
    wifi_wait_sta_link_down();
    wifi_enable_dual_band();
    esp_err_t ch_err = esp_wifi_set_channel(AP_CHANNEL_2G_DEFAULT, WIFI_SECOND_CHAN_NONE);
    if (ch_err != ESP_OK) {
        ESP_LOGW(TAG, "Forcing 2.4G channel failed: %s", esp_err_to_name(ch_err));
    }
    esp_err_t err = wifi_apply_ap_2g();

    uint8_t channel = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&channel, &second) == ESP_OK && channel > 14) {
        ESP_LOGW(TAG, "AP still on 5G ch %u after config; forcing 2.4G ch %d",
                 channel, AP_CHANNEL_2G_DEFAULT);
        if (esp_wifi_set_channel(AP_CHANNEL_2G_DEFAULT, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            configure_ap(AP_CHANNEL_2G_DEFAULT);
        }
    }
    return err;
}

static void sta_config_from_pending(wifi_config_t *cfg, bool pin, const uint8_t *bssid, uint8_t channel)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->sta.ssid, s_pending_ssid, strnlen(s_pending_ssid, sizeof(cfg->sta.ssid)));
    memcpy(cfg->sta.password, s_pending_passwd, strnlen(s_pending_passwd, sizeof(cfg->sta.password)));
    /* Same selection policy as capture_stream_test's example_connect(). */
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg->sta.threshold.rssi = -127;
    if (pin && bssid) {
        memcpy(cfg->sta.bssid, bssid, sizeof(cfg->sta.bssid));
        cfg->sta.bssid_set = 1;
        cfg->sta.channel = channel;
    }
}

static esp_err_t apply_sta_config(bool pin, const uint8_t *bssid, uint8_t channel)
{
    wifi_config_t cfg;
    sta_config_from_pending(&cfg, pin, bssid, channel);
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static bool wifi_pick_target_bssid(void)
{
    if (!s_pending_ssid[0] || !s_wifi_lock) return false;
    if (xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(15000)) != pdTRUE) return false;

    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)s_pending_ssid,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 30,
        .scan_time.active.max = 60,
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        xSemaphoreGive(s_wifi_lock);
        return false;
    }

    uint16_t count = 12;
    wifi_ap_record_t records[12];
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        xSemaphoreGive(s_wifi_lock);
        return false;
    }

    int best = -1;
    for (int i = 0; i < count; ++i) {
        if (records[i].ssid[0] == '\0') continue;
        uint8_t ch = records[i].primary;
        bool ap_capable_5g = (ch >= 36 && ch <= 48) || ch >= 149;
        if (ch > 14 && !ap_capable_5g) continue; /* DFS AP not supported */
        if (best < 0 || records[i].rssi > records[best].rssi) best = i;
    }
    if (best < 0) {
        xSemaphoreGive(s_wifi_lock);
        return false;
    }

    memcpy(s_pending_bssid, records[best].bssid, sizeof(s_pending_bssid));
    s_pending_channel = records[best].primary;
    s_pending_bssid_set = true;
    ESP_LOGI(TAG, "Auto-selected BSSID " MACSTR " on channel %u (%s)",
             MAC2STR(s_pending_bssid), s_pending_channel,
             s_pending_channel > 14 ? "5G" : "2.4G");
    xSemaphoreGive(s_wifi_lock);
    return true;
}

static esp_err_t wifi_start_ap(bool stop_on_sta_up)
{
    (void)stop_on_sta_up;
    if (s_ap_active) return ESP_OK;
    ESP_RETURN_ON_ERROR(ensure_ap_netif(), TAG, "Failed to create AP netif");

    /* Option C: the 5 GHz provisioning AP and the STA never run together.
     * Disconnect the STA and keep it idle; the AP is closed before connect. */
    s_manual_disconnect = true;
    s_sta_connecting = false;
    s_fast_retry_exhausted = false;
    s_retry_attempts = 0;
    s_retry_at_us = 0;
    s_ap_stop_at_us = 0;
    s_ap_close_at_us = 0;
    s_connect_watchdog_at_us = 0;
    s_disconnect_reason = 0;
    bool was_linked = s_sta_connected;
    s_sta_connected = false;
    s_sta_ip[0] = '\0';
    s_sta_ssid[0] = '\0';
    s_pending_bssid_set = false;
    s_pending_channel = 0;
    if (was_linked) {
        s_switching_creds = true;
        s_switching_since_us = esp_timer_get_time();
        esp_wifi_disconnect();
    }
    wifi_clear_driver_sta_config();

    wifi_mode_t mode = WIFI_MODE_APSTA; /* STA idle for scanning; AP on 2.4G */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "Failed to switch to APSTA mode");
    esp_err_t err = wifi_apply_ap_2g_stable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start 2.4G SoftAP: %s", esp_err_to_name(err));
        return err;
    }

    s_ap_active = true;
    s_ap_stop_on_sta_up = false;
    ESP_LOGI(TAG, "SoftAP enabled: SSID='%s', IP='%s', 2.4G ch %d (stays up during verify; closes %d s after success)",
             s_ap_ssid, AP_IP_DEFAULT, AP_CHANNEL_2G_DEFAULT, AP_STOP_GRACE_SEC);
    return ESP_OK;
}

static void wifi_stop_ap(void)
{
    if (!s_ap_active) return;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to switch to STA-only mode: %s", esp_err_to_name(err));
    }
    s_ap_active = false;
    s_ap_stop_on_sta_up = false;
    s_ap_stop_at_us = 0;
    s_ap_close_at_us = 0;
    /* Drop the APSTA pin; the next reconnect uses a full scan like the reference. */
    s_sta_pinned = false;

    ESP_LOGI(TAG, "SoftAP closed, STA-only steady state (long-press BOOT to reopen AP)");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                disable_ap_dhcp_dns();
                ESP_LOGI(TAG, "SoftAP started: SSID='%s', IP='%s'", s_ap_ssid, AP_IP_DEFAULT);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                /* Association can eat most of the 8 s connect budget; give DHCP
                 * its own window instead of forcing a pointless retry. */
                if (s_sta_connecting && !s_sta_connected) {
                    s_connect_watchdog_at_us = esp_timer_get_time() + 6000000;
                }
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Station " MACSTR " joined SoftAP, AID=%d", MAC2STR(event->mac), event->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Station " MACSTR " left SoftAP, AID=%d", MAC2STR(event->mac), event->aid);
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
                if (event->reason == WIFI_REASON_ROAMING) {
                    break;
                }
                bool was_connected = s_sta_connected;
                s_sta_connected = false;
                s_sta_ip[0] = '\0';
                if (s_switching_creds) {
                    s_switching_creds = false;
                    s_switching_since_us = 0;
                    ESP_LOGI(TAG, "Ignoring disconnect (reason=%d) while switching credentials", event->reason);
                    break;
                }
                if (s_manual_disconnect) {
                    ESP_LOGI(TAG, "STA disconnected (manual), reconnect disabled");
                    break;
                }
                s_disconnect_reason = event->reason;
                if (was_connected) s_retry_attempts = 0;
                if (s_pending_ssid[0] && s_events) {
                    xEventGroupSetBits(s_events, EV_STA_DOWN);
                }
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (s_sta_netif && event->esp_netif != s_sta_netif) {
            return;
        }
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_sta_connecting = false;
        s_switching_creds = false;
        s_switching_since_us = 0;
        s_connect_watchdog_at_us = 0;
        if (s_events) {
            xEventGroupSetBits(s_events, EV_GOT_IP);
        }
    }
}

static void wifi_handle_got_ip(void)
{
    s_sta_connected = true;
    s_sta_connecting = false;
    s_fast_retry_exhausted = false;
    s_retry_attempts = 0;
    s_slow_retry_count = 0;
    s_retry_at_us = 0;
    s_disconnect_reason = 0;
    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", s_pending_ssid);

    if (s_creds_dirty) {
        if (app_mywifi_save_sta_creds(s_pending_ssid, s_pending_passwd) == ESP_OK) {
            s_creds_dirty = false;
        }
    }
    if (s_ap_active && s_ap_stop_on_sta_up) {
        s_ap_stop_at_us = esp_timer_get_time() + (int64_t)AP_STOP_GRACE_MS * 1000;
    }

    printf("\r\n=======================================================\r\n"
           " [WiFi STA Connected Successfully!]\r\n"
           " Connected Router SSID: %s\r\n"
           " STA IP Address:        %s\r\n"
           " Camera Web URL:        http://%s/\r\n"
           " Web Stream URL:        http://%s:81/stream\r\n"
           " Web Snapshot URL:      http://%s/capture\r\n"
           " SoftAP:                %s\r\n"
           "=======================================================\r\n\r\n",
           s_sta_ssid, s_sta_ip, s_sta_ip, s_sta_ip, s_sta_ip,
           !s_ap_active ? "off (long-press BOOT to reopen)" :
           s_ap_stop_on_sta_up ? "closing in 30 s, STA-only steady state" :
           "http://" AP_IP_DEFAULT " (kept open)");
}

static void wifi_schedule_retry(int64_t delay_ms)
{
    s_retry_at_us = esp_timer_get_time() + delay_ms * 1000;
}

/* Cross-band APSTA can associate without ever getting an IP on the hosted C5;
 * force a failure event instead of hanging forever on one attempt. */
static void wifi_arm_connect_watchdog(void)
{
    s_connect_watchdog_at_us = esp_timer_get_time() + 8000000;
}

static void wifi_handle_sta_down(void)
{
    if (s_manual_disconnect) return;
    s_connect_watchdog_at_us = 0;
    s_sta_connecting = true;
    s_sta_connected = false;
    /* A pending "close AP after success" must not fire while the link is down. */
    s_ap_stop_at_us = 0;

    if (s_retry_attempts < STA_FAST_RETRY_LIMIT) {
        s_retry_attempts++;
        s_fast_retry_exhausted = false;
        wifi_schedule_retry(STA_RETRY_INTERVAL_MS);
        ESP_LOGW(TAG, "STA disconnected (reason=%d), fast retry %d/%d in %d ms",
                 s_disconnect_reason, s_retry_attempts, STA_FAST_RETRY_LIMIT, STA_RETRY_INTERVAL_MS);
        return;
    }

    s_fast_retry_exhausted = true;
    if (!s_ap_active) {
        ESP_LOGW(TAG, "STA retries exhausted (reason=%d), reopening SoftAP provisioning",
                 s_disconnect_reason);
        esp_err_t err = wifi_start_ap(true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SoftAP fallback failed: %s", esp_err_to_name(err));
        }
    }
    /* A pinned BSSID can disappear (mesh node roam or a stale scan entry) and
     * then every retry fails with NO_AP_FOUND without ever scanning again.
     * Slow retries drop the pin and let the C5 pick any node of the SSID. */
    s_sta_pinned = false;
    if (s_pending_bssid_set) {
        s_pending_bssid_set = false;
        s_pending_channel = 0;
        ESP_LOGW(TAG, "Slow retry drops the pinned BSSID and uses a full scan");
    }
    if (s_slow_retry_count >= STA_SLOW_RETRY_LIMIT) {
        s_sta_connecting = false;
        ESP_LOGW(TAG, "Automatic retries exhausted (reason=%d); waiting for a new provisioning request",
                 s_disconnect_reason);
        return;
    }
    s_slow_retry_count++;
    wifi_schedule_retry(STA_SLOW_RETRY_INTERVAL_MS);
    ESP_LOGW(TAG, "Slow reconnect to '%s' every %d s (%d/%d)", s_pending_ssid,
             STA_SLOW_RETRY_INTERVAL_MS / 1000, s_slow_retry_count, STA_SLOW_RETRY_LIMIT);
}

static void wifi_retry_tick(void)
{
    s_retry_at_us = 0;
    if (!s_sta_connecting || s_manual_disconnect || !s_pending_ssid[0]) return;

    /* Keep a user-selected BSSID/channel pinned across retries; otherwise use
     * full-channel automatic selection. */
    if (s_pending_bssid_set) {
        apply_sta_config(true, s_pending_bssid, s_pending_channel);
    } else {
        apply_sta_config(false, NULL, 0);
    }

    ESP_LOGI(TAG, "Reconnecting to '%s'%s...", s_pending_ssid,
             s_pending_bssid_set ? " (pinned BSSID/channel)" : "");
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_wifi_connect();
    wifi_note_connect_result(err, esp_timer_get_time() - t0);
    if (err == ESP_OK) {
        wifi_arm_connect_watchdog();
    } else {
        ESP_LOGW(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        wifi_schedule_retry(s_retry_attempts < STA_FAST_RETRY_LIMIT ?
                            STA_RETRY_INTERVAL_MS : STA_SLOW_RETRY_INTERVAL_MS);
    }
}

static void wifi_handle_restore_ap(void)
{
    esp_err_t err = wifi_start_ap(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP restore failed: %s", esp_err_to_name(err));
    }
}

static void wifi_handle_prepare_ap_channel(void)
{
    uint8_t channel = s_prepare_channel;
    s_prepare_channel = 0;
    if (!s_ap_active || channel < 1 || channel > 14) return;
    if (s_sta_connected || s_sta_connecting) return; /* do not disturb a running connect */

    wifi_config_t ap_cfg = { 0 };
    if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) return;
    if (ap_cfg.ap.channel == channel) return;

    ap_cfg.ap.channel = channel;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to pre-align AP to channel %u: %s", channel, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "AP pre-aligned to 2.4G ch %u for the selected SSID", channel);
}

static void wifi_handle_enter_provisioning(void)
{
    s_manual_disconnect = true;
    s_sta_connecting = false;
    s_fast_retry_exhausted = false;
    s_retry_attempts = 0;
    s_retry_at_us = 0;
    s_ap_stop_at_us = 0;
    s_ap_close_at_us = 0;
    s_connect_watchdog_at_us = 0;
    s_creds_dirty = false;
    s_sta_pinned = false;
    s_disconnect_reason = 0;
    bool was_linked = s_sta_connected;
    s_sta_connected = false;
    s_sta_ip[0] = '\0';
    s_sta_ssid[0] = '\0';

    /* Keep the NVS record for the one-click reconnect card, but clear the
     * runtime copy so the STA interface stays idle until the user re-provisions. */
    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    memset(s_pending_passwd, 0, sizeof(s_pending_passwd));
    memset(s_pending_bssid, 0, sizeof(s_pending_bssid));
    s_pending_channel = 0;
    s_pending_bssid_set = false;

    if (was_linked) {
        s_switching_creds = true;
        s_switching_since_us = esp_timer_get_time();
        esp_wifi_disconnect();
    }
    wifi_clear_driver_sta_config();

    esp_err_t err = ensure_ap_netif();
    /* Keep the STA interface enabled but idle for scanning. The AP is fixed to
     * 2.4 GHz so it can stay up while the STA connects to either band. */
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = wifi_apply_ap_2g_stable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enter AP provisioning: %s", esp_err_to_name(err));
        return;
    }
    s_ap_active = true;
    s_ap_stop_on_sta_up = false;
    ESP_LOGI(TAG, "AP provisioning mode: 2.4G AP, STA disconnected (idle for scanning), saved credentials kept for one-click reconnect");
}

static void wifi_begin_sta_connect(void)
{
    /* The provisioning AP is already off; restore auto band mode so the STA can
     * connect to the user-selected 2.4 GHz or 5 GHz network. */
    wifi_enable_dual_band();
    s_manual_disconnect = false;
    s_sta_connecting = true;
    s_fast_retry_exhausted = false;
    s_retry_attempts = 0;
    s_slow_retry_count = 0;
    s_disconnect_reason = 0;
    s_sta_ip[0] = '\0';
    s_sta_ssid[0] = '\0';
    s_sta_pinned = false;
    s_retry_at_us = 0;
    s_ap_stop_at_us = 0;
    s_ap_close_at_us = 0;
    s_connect_watchdog_at_us = 0;
    s_ap_stop_on_sta_up = false;

    if (s_sta_connected) {
        s_switching_creds = true;
        s_switching_since_us = esp_timer_get_time();
        esp_wifi_disconnect();
        s_sta_connected = false;
    }

    if (s_pending_bssid_set) {
        esp_err_t err = apply_sta_config(true, s_pending_bssid, s_pending_channel);
        if (err == ESP_OK) {
            s_sta_pinned = true;
            ESP_LOGI(TAG, "Connecting to selected BSSID " MACSTR " on channel %u (%s)",
                     MAC2STR(s_pending_bssid), s_pending_channel,
                     s_pending_channel > 14 ? "5G" : "2.4G");
        } else {
            ESP_LOGW(TAG, "Selected BSSID apply failed: %s; using automatic selection",
                     esp_err_to_name(err));
            apply_sta_config(false, NULL, 0);
        }
    } else {
        apply_sta_config(false, NULL, 0);
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_wifi_connect();
    wifi_note_connect_result(err, esp_timer_get_time() - t0);
    if (err == ESP_OK) {
        wifi_arm_connect_watchdog();
    } else {
        ESP_LOGW(TAG, "Connect request failed: %s", esp_err_to_name(err));
        wifi_schedule_retry(STA_RETRY_INTERVAL_MS);
    }
}

static void wifi_handle_connect_request(void)
{
    if (s_ap_active) {
        s_manual_disconnect = false;
        s_sta_connecting = true;
        s_fast_retry_exhausted = false;
        s_retry_attempts = 0;
        s_slow_retry_count = 0;
        s_disconnect_reason = 0;
        s_sta_ip[0] = '\0';
        s_sta_ssid[0] = '\0';
        s_retry_at_us = 0;
        s_ap_stop_at_us = 0;
        s_ap_close_at_us = 0;
        s_connect_watchdog_at_us = 0;
        s_ap_stop_on_sta_up = true;

        /* Re-provisioning while already connected: drop the old link first so
         * the status API never reports "connected" with an empty IP. */
        if (s_sta_connected) {
            s_switching_creds = true;
            s_switching_since_us = esp_timer_get_time();
            esp_wifi_disconnect();
        }
        s_sta_connected = false;

        if (!s_pending_bssid_set && !wifi_pick_target_bssid()) {
            /* Keep the AP up: a full-scan STA connect works with the AP active. */
            ESP_LOGW(TAG, "No target BSSID found; keeping 2.4G AP up for a full-scan STA connect");
            wifi_begin_sta_connect();
            s_ap_stop_on_sta_up = true; /* wifi_begin_sta_connect() resets it */
            return;
        }

        uint8_t channel = s_pending_channel;
        wifi_enable_dual_band();

        if (channel <= 14) {
            /* Same band: the AP and STA must share one channel on the C5. */
            wifi_config_t ap_cfg = { 0 };
            if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK &&
                ap_cfg.ap.channel != channel) {
                ap_cfg.ap.channel = channel;
                esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
                if (cfg_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to move AP to channel %u: %s; keeping AP on current channel",
                             channel, esp_err_to_name(cfg_err));
                } else {
                    ESP_LOGI(TAG, "AP moved to 2.4G ch %u; waiting 2.5 s for provisioning clients", channel);
                    vTaskDelay(pdMS_TO_TICKS(2500));
                }
            }
        } else {
            /* 5 GHz / DFS target: keep the 2.4G AP up and use cross-band APSTA. */
            ESP_LOGI(TAG, "Target ch %u is 5G; keeping the 2.4G AP up (cross-band APSTA)", channel);
        }

        esp_err_t err = apply_sta_config(true, s_pending_bssid, s_pending_channel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Selected BSSID apply failed while AP active: %s; retrying without pin",
                     esp_err_to_name(err));
            err = apply_sta_config(false, NULL, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "STA config apply failed: %s; AP stays up", esp_err_to_name(err));
                s_sta_connecting = false;
                return;
            }
            s_sta_pinned = false;
        } else {
            s_sta_pinned = true;
        }

        ESP_LOGI(TAG, "Connecting STA to '%s'%s ch %u (%s) while 2.4G AP stays up; AP closes %d s after success",
                 s_pending_ssid, s_sta_pinned ? " (pinned BSSID)" : "",
                 s_pending_channel, s_pending_channel > 14 ? "5G" : "2.4G", AP_STOP_GRACE_SEC);

        int64_t t0 = esp_timer_get_time();
        err = esp_wifi_connect();
        wifi_note_connect_result(err, esp_timer_get_time() - t0);
        if (err == ESP_OK) {
            wifi_arm_connect_watchdog();
        } else {
            ESP_LOGW(TAG, "Connect request failed: %s", esp_err_to_name(err));
            wifi_schedule_retry(STA_RETRY_INTERVAL_MS);
        }
        return;
    }

    wifi_begin_sta_connect();
}

static void wifi_print_periodic_status(void)
{
    if (s_ap_active) {
        printf("[WiFi 30s] AP mode | SSID: %s | AP IP: %s", s_ap_ssid, AP_IP_DEFAULT);
        if (s_sta_connected) {
            printf(" | STA: %s | STA IP: %s", s_sta_ssid, s_sta_ip);
        }
        printf("\r\n");
    } else if (s_sta_connected) {
        printf("[WiFi 30s] STA mode | Router: %s | STA IP: %s\r\n", s_sta_ssid, s_sta_ip);
    } else {
        printf("[WiFi 30s] STA connecting | Router: %s | STA IP: none | %s\r\n",
               s_pending_ssid[0] ? s_pending_ssid : "-",
               app_mywifi_get_disconnect_message());
    }
}

static void wifi_control_task(void *arg)
{
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(s_events,
            EV_GOT_IP | EV_STA_DOWN | EV_CONNECT_REQ | EV_RESTORE_AP | EV_ENTER_PROV | EV_PREPARE_AP,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();

        if (s_status_print_at_us == 0) {
            s_status_print_at_us = now + 30000000LL;
        }
        if (now >= s_status_print_at_us) {
            s_status_print_at_us = now + 30000000LL;
            wifi_print_periodic_status();
        }

        if (bits & EV_ENTER_PROV) wifi_handle_enter_provisioning();
        if (bits & EV_RESTORE_AP) wifi_handle_restore_ap();
        if (bits & EV_PREPARE_AP) wifi_handle_prepare_ap_channel();
        if (bits & EV_CONNECT_REQ) wifi_handle_connect_request();
        if (bits & EV_GOT_IP) wifi_handle_got_ip();
        if (bits & EV_STA_DOWN) wifi_handle_sta_down();

        /* A disconnect-during-switch event may never arrive; never let the flag leak. */
        if (s_switching_creds && s_switching_since_us &&
            now - s_switching_since_us > 2000000) {
            s_switching_creds = false;
            s_switching_since_us = 0;
        }

        if (s_connect_watchdog_at_us && now >= s_connect_watchdog_at_us) {
            s_connect_watchdog_at_us = 0;
            if (s_sta_connecting && !s_sta_connected && !s_manual_disconnect) {
                ESP_LOGW(TAG, "Connect attempt timed out; forcing the next retry");
                s_switching_creds = true;
                s_switching_since_us = now;
                esp_wifi_disconnect();
                s_disconnect_reason = WIFI_REASON_NO_AP_FOUND;
                wifi_handle_sta_down();
            }
        }

        if (s_retry_at_us && now >= s_retry_at_us) {
            wifi_retry_tick();
        }
        if (s_ap_stop_at_us && now >= s_ap_stop_at_us) {
            s_ap_stop_at_us = 0;
            wifi_stop_ap();
        }
        if (s_ap_close_at_us && now >= s_ap_close_at_us) {
            s_ap_close_at_us = 0;
            wifi_stop_ap();
            wifi_begin_sta_connect();
        }
    }
}

esp_err_t app_mywifi_connect_sta_ex(const char *ssid, const char *passwd,
                                    const uint8_t *bssid, uint8_t channel)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    if (passwd && strlen(passwd) > 64) return ESP_ERR_INVALID_ARG;
    if (bssid && (channel == 0 || channel > 177)) return ESP_ERR_INVALID_ARG;
    if (!s_events) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Triggering STA connection to SSID: '%s'%s", ssid,
             bssid ? " (specific BSSID)" : "");
    snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", ssid);
    if (passwd) {
        snprintf(s_pending_passwd, sizeof(s_pending_passwd), "%s", passwd);
    } else {
        s_pending_passwd[0] = '\0';
    }
    if (bssid) {
        memcpy(s_pending_bssid, bssid, sizeof(s_pending_bssid));
        s_pending_channel = channel;
        s_pending_bssid_set = true;
    } else {
        memset(s_pending_bssid, 0, sizeof(s_pending_bssid));
        s_pending_channel = 0;
        s_pending_bssid_set = false;
    }
    s_creds_dirty = true;
    s_manual_disconnect = false;
    xEventGroupSetBits(s_events, EV_CONNECT_REQ);
    return ESP_OK;
}

esp_err_t app_mywifi_connect_sta(const char *ssid, const char *passwd)
{
    return app_mywifi_connect_sta_ex(ssid, passwd, NULL, 0);
}

static void wifi_scan_task(void *arg)
{
    (void)arg;

    while (true) {
        if (xSemaphoreTake(s_scan_req, portMAX_DELAY) != pdTRUE) continue;

        char *json_str = NULL;
        if (s_wifi_lock && xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(15000)) == pdTRUE) {
            wifi_scan_config_t scan_cfg = {
                .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                .show_hidden = true,
                .scan_time.active.min = 30,
                .scan_time.active.max = 60,
            };
            int64_t scan_t0 = esp_timer_get_time();
            esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
            if (ret == ESP_OK) {
                uint16_t ap_count = 0;
                esp_wifi_scan_get_ap_num(&ap_count);
                uint16_t total = ap_count;
                if (ap_count > 64) ap_count = 64;
                wifi_ap_record_t *ap_records = NULL;
                if (ap_count) {
                    ap_records = malloc((size_t)ap_count * sizeof(wifi_ap_record_t));
                }
                if (ap_records &&
                    esp_wifi_scan_get_ap_records(&ap_count, ap_records) == ESP_OK) {
                    cJSON *root = cJSON_CreateArray();
                    int listed = 0;
                    for (int i = 0; i < ap_count; i++) {
                        if (strlen((char*)ap_records[i].ssid) == 0) continue;
                        cJSON *item = cJSON_CreateObject();
                        cJSON_AddStringToObject(item, "ssid", (char*)ap_records[i].ssid);
                        cJSON_AddNumberToObject(item, "rssi", ap_records[i].rssi);
                        cJSON_AddNumberToObject(item, "auth", ap_records[i].authmode);
                        cJSON_AddNumberToObject(item, "channel", ap_records[i].primary);
                        cJSON_AddStringToObject(item, "band", ap_records[i].primary > 14 ? "5G" : "2.4G");
                        char bssid_str[18];
                        snprintf(bssid_str, sizeof(bssid_str), MACSTR, MAC2STR(ap_records[i].bssid));
                        cJSON_AddStringToObject(item, "bssid", bssid_str);
                        cJSON_AddItemToArray(root, item);
                        listed++;
                    }
                    json_str = cJSON_PrintUnformatted(root);
                    cJSON_Delete(root);
                    ESP_LOGI(TAG, "Scan done: %u APs found, %d listed, %lld ms",
                             total, listed, (long long)((esp_timer_get_time() - scan_t0) / 1000));
                }
                free(ap_records);
            } else {
                ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
            }
            xSemaphoreGive(s_wifi_lock);
        }

        if (json_str) {
            if (s_scan_cache_lock && xSemaphoreTake(s_scan_cache_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                free(s_scan_cache);
                s_scan_cache = json_str;
                xSemaphoreGive(s_scan_cache_lock);
            } else {
                free(json_str);
            }
        }
        s_scan_running = false;
    }
}

void app_mywifi_scan_trigger(void)
{
    if (!s_scan_req || s_scan_running) return;
    s_scan_running = true;
    xSemaphoreGive(s_scan_req);
}

bool app_mywifi_scan_running(void)
{
    return s_scan_running;
}

char* app_mywifi_scan_json(void)
{
    char *out = NULL;
    if (s_scan_cache_lock && xSemaphoreTake(s_scan_cache_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        out = strdup(s_scan_cache ? s_scan_cache : "[]");
        xSemaphoreGive(s_scan_cache_lock);
    }
    return out ? out : strdup("[]");
}

void app_mywifi_main(void)
{
    load_device_hostname();
    bool force_ap = (s_rtc_force_ap == FORCE_AP_MAGIC);
    s_rtc_force_ap = 0;
    if (force_ap) {
        ESP_LOGW(TAG, "Recovery boot: forcing AP provisioning (saved credentials kept)");
    }
    s_events = xEventGroupCreate();
    s_wifi_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_wifi_lock) {
        ESP_LOGE(TAG, "Failed to allocate Wi-Fi synchronization primitives");
        return;
    }

    s_scan_req = xSemaphoreCreateBinary();
    s_scan_cache_lock = xSemaphoreCreateMutex();
    if (s_scan_req && s_scan_cache_lock) {
        if (xTaskCreate(wifi_scan_task, "wifi_scan", 6144, NULL, 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create Wi-Fi scan task");
        }
    }

    ESP_ERROR_CHECK(ensure_ap_netif());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    assert(s_sta_netif);
    esp_netif_set_hostname(s_ap_netif, s_device_hostname);
    esp_netif_set_hostname(s_sta_netif, s_device_hostname);
    ESP_LOGI(TAG, "Device hostname: %s", s_device_hostname);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ensure_ap_ssid();
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("CN", false));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    char saved_ssid[33] = { 0 };
    char saved_passwd[65] = { 0 };
    if (!force_ap && app_mywifi_load_sta_creds(saved_ssid, sizeof(saved_ssid),
                                               saved_passwd, sizeof(saved_passwd)) == ESP_OK && saved_ssid[0]) {
        snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", saved_ssid);
        snprintf(s_pending_passwd, sizeof(s_pending_passwd), "%s", saved_passwd);
    }

    if (s_pending_ssid[0]) {
        /* Reference steady state: pure STA, SoftAP stays closed. */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(apply_sta_config(false, NULL, 0));
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_enable_dual_band();
        s_sta_connecting = true;
        ESP_LOGI(TAG, "Boot STA connect to saved SSID '%s'", s_pending_ssid);
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_wifi_connect();
        wifi_note_connect_result(err, esp_timer_get_time() - t0);
        if (err == ESP_OK) {
            wifi_arm_connect_watchdog();
        } else {
            ESP_LOGW(TAG, "Boot connect failed: %s", esp_err_to_name(err));
            s_ap_stop_on_sta_up = true;
            wifi_start_ap(true);
            wifi_schedule_retry(STA_RETRY_INTERVAL_MS);
        }
    } else {
        /* No credentials (factory state): APSTA with the STA idle. AP-only mode
         * cannot scan on the hosted C5, which would leave the provisioning page
         * without a Wi-Fi list. */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        esp_err_t pre_ap_err = configure_ap(1);
        if (pre_ap_err != ESP_OK) {
            ESP_LOGW(TAG, "Pre-start 2.4G AP config failed (%s); trying fallback ch %d before dual-band init",
                     esp_err_to_name(pre_ap_err), AP_CHANNEL_DEFAULT);
            esp_err_t fb_err = configure_ap(AP_CHANNEL_DEFAULT);
            if (fb_err != ESP_OK) {
                ESP_LOGW(TAG, "Pre-start fallback AP config returned %s; continuing to esp_wifi_start()",
                         esp_err_to_name(fb_err));
            }
        }
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_enable_dual_band();
        s_manual_disconnect = true;
        esp_err_t ap_err = wifi_apply_ap_2g_stable();
        if (ap_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure 2.4G provisioning AP: %s", esp_err_to_name(ap_err));
        }
        s_ap_active = true;
        s_ap_stop_on_sta_up = false;
        s_ap_close_at_us = 0;
        ESP_LOGI(TAG, "%s; 2.4G SoftAP provisioning ready (APSTA, STA idle)",
                 force_ap ? "Recovery boot: forced AP provisioning" : "No saved credentials");
    }

    BaseType_t task_ok = xTaskCreate(wifi_control_task, "wifi_ctrl", 4096, NULL, 4, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi control task");
    }

    printf("\r\n=======================================================\r\n"
           " [WiFi Ready] %s\r\n"
           " AP SSID:             %s\r\n"
           " AP Web URL:          http://%s\r\n"
           " Config URL:          http://%s/wifi\r\n"
           " Device Hostname:     %s (DHCP)\r\n"
           " State:               %s\r\n"
           "=======================================================\r\n\r\n",
           s_pending_ssid[0] ? "STA-first (reference parity)" : "AP provisioning",
           s_ap_ssid, AP_IP_DEFAULT, AP_IP_DEFAULT,
           app_mywifi_get_device_hostname(),
           s_pending_ssid[0] ? "Connecting to saved router in background..." :
                               "Waiting for Wi-Fi provisioning...");
}
