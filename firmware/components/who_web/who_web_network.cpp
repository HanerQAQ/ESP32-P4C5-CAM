#include <cstring>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

static const char *TAG = "who_web_net";

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t configure_radio_5g(void)
{
#if (CONFIG_SOC_WIFI_SUPPORT_5G || CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G)
    esp_err_t band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
    if (band_err != ESP_OK && band_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "5 GHz mode failed: %s", esp_err_to_name(band_err));
    }

    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    esp_err_t err = esp_wifi_set_protocols(WIFI_IF_AP, &protocols);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not enable 802.11ax protocols: %s", esp_err_to_name(err));
    }

    wifi_bandwidths_t bandwidths = {
        .ghz_2g = WIFI_BW20,
        .ghz_5g = WIFI_BW20,
    };
    err = esp_wifi_set_bandwidths(WIFI_IF_AP, &bandwidths);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set 20 MHz bandwidth: %s", esp_err_to_name(err));
    }
    return ESP_OK;
#else
    ESP_LOGE(TAG, "This build does not expose 5 GHz Wi-Fi support");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t configure_radio_2g4(void)
{
    esp_err_t err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to force 20 MHz bandwidth: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

extern "C" esp_err_t who_web_network_start(void)
{
    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi AP netif");
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, 192, 168, 5, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 5, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap_netif), TAG, "DHCP server stop failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip_info), TAG, "Static IP setup failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), TAG, "DHCP server start failed");

    ESP_LOGI(TAG, "Internal heap before hosted Wi-Fi: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    ESP_LOGI(TAG, "Internal DMA heap before hosted Wi-Fi: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_cfg), TAG, "esp_wifi_init failed");

    esp_err_t country_err = esp_wifi_set_country_code("CN", false);
    if (country_err != ESP_OK) {
        ESP_LOGW(TAG, "Country code setup failed: %s", esp_err_to_name(country_err));
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    uint8_t mac[6] = {};
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, mac), TAG, "Could not read AP MAC");

    char ssid[sizeof(((wifi_config_t *)0)->ap.ssid) + 1] = {};
    snprintf(ssid, sizeof(ssid), "%.24s-%02X%02X%02X", CONFIG_WHO_WEB_AP_SSID, mac[3], mac[4], mac[5]);

    wifi_config_t default_cfg = {};
    esp_err_t get_err = esp_wifi_get_config(WIFI_IF_AP, &default_cfg);
    ESP_LOGI(TAG, "Default AP cfg: err=%s ssid='%.32s' ch=%d auth=%d max_conn=%d cipher=%d pmf_req=%d",
             esp_err_to_name(get_err), default_cfg.ap.ssid, default_cfg.ap.channel,
             default_cfg.ap.authmode, default_cfg.ap.max_connection,
             default_cfg.ap.pairwise_cipher, default_cfg.ap.pmf_cfg.required);

    wifi_config_t wifi_config = default_cfg;
    wifi_config.ap.channel = CONFIG_WHO_WEB_AP_CHANNEL;
    wifi_config.ap.max_connection = CONFIG_WHO_WEB_AP_MAX_CONNECTIONS;
    wifi_config.ap.beacon_interval = 100;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    memset(wifi_config.ap.password, 0, sizeof(wifi_config.ap.password));

    const char *password = CONFIG_WHO_WEB_AP_PASSWORD;
    const size_t password_len = strlen(password);
    if (password_len >= 8 && password_len <= 63) {
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        memcpy(wifi_config.ap.password, password, password_len);
    }

    memcpy(wifi_config.ap.ssid, ssid, strlen(ssid));
    wifi_config.ap.ssid[strlen(ssid)] = '\0';
    wifi_config.ap.ssid_len = strlen(ssid);

    ESP_LOGI(TAG, "Setting AP cfg: ssid='%.32s' len=%d ch=%d auth=%d max_conn=%d cipher=%d",
             wifi_config.ap.ssid, wifi_config.ap.ssid_len, wifi_config.ap.channel,
             wifi_config.ap.authmode, wifi_config.ap.max_connection, wifi_config.ap.pairwise_cipher);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "AP mode setup failed");

    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (cfg_err != ESP_OK && wifi_config.ap.channel >= 36) {
        ESP_LOGW(TAG, "Pre-config for 5 GHz channel %d failed (%s), temporarily using channel 1 before start",
                 wifi_config.ap.channel, esp_err_to_name(cfg_err));
        uint8_t target_ch = wifi_config.ap.channel;
        wifi_config.ap.channel = 1;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "Fallback AP config failed");
        wifi_config.ap.channel = target_ch;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");

    if (wifi_config.ap.channel >= 36) {
        configure_radio_5g();
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "Setting AP 5 GHz config failed");
    } else {
        configure_radio_2g4();
    }

    esp_err_t inactive_err = esp_wifi_set_inactive_time(WIFI_IF_AP, 60);
    if (inactive_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set inactive time: %s", esp_err_to_name(inactive_err));
    }

    uint8_t active_channel = 0;
    wifi_second_chan_t active_second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&active_channel, &active_second);

    ESP_LOGI(TAG, "SoftAP SSID:%s password:%s channel:%d (driver channel:%d, band:%s)",
             ssid, password_len >= 8 ? password : "<open>",
             wifi_config.ap.channel, active_channel,
             active_channel >= 36 ? "5GHz" : "2.4GHz");
    ESP_LOGI(TAG, "Connect to the AP and open http://192.168.5.1");
    return ESP_OK;
}
