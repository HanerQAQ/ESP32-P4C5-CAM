/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/apps/netbiosns.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "network_service.h"

#define EXAMPLE_MDNS_INSTANCE               CONFIG_EXAMPLE_MDNS_INSTANCE
#define EXAMPLE_MDNS_HOST_NAME              CONFIG_EXAMPLE_MDNS_HOST_NAME

#define EXAMPLE_WIFI_AP_SSID                CONFIG_EXAMPLE_WIFI_AP_SSID
#define EXAMPLE_WIFI_AP_PASSWORD            CONFIG_EXAMPLE_WIFI_AP_PASSWORD
#define EXAMPLE_WIFI_AP_CHANNEL             CONFIG_EXAMPLE_WIFI_AP_CHANNEL
#define EXAMPLE_WIFI_AP_MAX_CONNECTIONS     CONFIG_EXAMPLE_WIFI_AP_MAX_CONNECTIONS

#if CONFIG_EXAMPLE_WIFI_AP_BAND_5G_ONLY
#define EXAMPLE_WIFI_AP_AUTO_FALLBACK_CHANNEL 36
#else
#define EXAMPLE_WIFI_AP_AUTO_FALLBACK_CHANNEL 6
#endif

static const uint8_t WIFI_AP_2G_CANDIDATE_CHANNELS[] = {
    1, 6, 11,
};

#if (CONFIG_SOC_WIFI_SUPPORT_5G || CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G) && CONFIG_EXAMPLE_WIFI_AP_BAND_5G_ONLY
/* Non-DFS channels provide wide client compatibility and fast AP startup. */
static const uint8_t WIFI_AP_5G_CANDIDATE_CHANNELS[] = {
    36, 40, 44, 48,
};
#endif

static const char *TAG = "network_service";

static void wifi_station_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "Station " MACSTR " connected", MAC2STR(event->mac));
        return;
    }

    if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "Station " MACSTR " disconnected", MAC2STR(event->mac));
    }
}

static void initialise_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(EXAMPLE_MDNS_HOST_NAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(EXAMPLE_MDNS_INSTANCE));

    mdns_txt_item_t serviceTxtData[] = {
        {"board", CONFIG_IDF_TARGET},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

#if CONFIG_SOC_WIFI_SUPPORT_5G || CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G
static const char *wifi_band_mode_to_str(wifi_band_mode_t band_mode)
{
    switch (band_mode) {
    case WIFI_BAND_MODE_AUTO:
        return "auto";
    case WIFI_BAND_MODE_5G_ONLY:
        return "5GHz only";
    case WIFI_BAND_MODE_2G_ONLY:
        return "2.4GHz only";
    default:
        return "unknown";
    }
}

static wifi_band_mode_t wifi_get_configured_band_mode(void)
{
#if CONFIG_EXAMPLE_WIFI_AP_BAND_5G_ONLY
    return WIFI_BAND_MODE_5G_ONLY;
#else
    return WIFI_BAND_MODE_2G_ONLY;
#endif
}

static esp_err_t wifi_configure_ap_radio(void)
{
    wifi_band_mode_t band_mode = wifi_get_configured_band_mode();
    ESP_RETURN_ON_ERROR(esp_wifi_set_band_mode(band_mode), TAG, "Failed to set Wi-Fi band mode");

    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocols(WIFI_IF_AP, &protocols), TAG, "Failed to set Wi-Fi protocols");

    wifi_bandwidths_t bandwidths = {
        .ghz_2g = WIFI_BW20,
        .ghz_5g = WIFI_BW20,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidths(WIFI_IF_AP, &bandwidths), TAG, "Failed to set Wi-Fi bandwidths");

    ESP_LOGI(TAG, "Wi-Fi AP band mode:%s protocols:2.4G b/g/n/ax, 5G a/n/ac/ax bandwidth:20MHz",
             wifi_band_mode_to_str(band_mode));
    return ESP_OK;
}
#else

static void wifi_set_ap_bandwidth_40mhz(void)
{
    esp_err_t ret = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW40);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set AP bandwidth to 40MHz: %s", esp_err_to_name(ret));
    }
}
#endif

static uint8_t wifi_select_best_ap_channel(uint8_t default_channel)
{
    esp_err_t ret;
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = NULL;
    const uint8_t *candidate_channels = WIFI_AP_2G_CANDIDATE_CHANNELS;
    size_t candidate_count = sizeof(WIFI_AP_2G_CANDIDATE_CHANNELS);

#if (CONFIG_SOC_WIFI_SUPPORT_5G || CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G) && CONFIG_EXAMPLE_WIFI_AP_BAND_5G_ONLY
    candidate_channels = WIFI_AP_5G_CANDIDATE_CHANNELS;
    candidate_count = sizeof(WIFI_AP_5G_CANDIDATE_CHANNELS);
#endif

    int channel_scores[candidate_count];
    memset(channel_scores, 0, sizeof(channel_scores));

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to switch to STA for channel scan: %s", esp_err_to_name(ret));
        return default_channel;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start Wi-Fi for channel scan: %s", esp_err_to_name(ret));
        return default_channel;
    }

#if CONFIG_SOC_WIFI_SUPPORT_5G || CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G
    ret = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set auto band mode for channel scan: %s", esp_err_to_name(ret));
    }
#endif

    wifi_scan_config_t scan_config = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .show_hidden = true,
    };

    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi channel scan failed: %s", esp_err_to_name(ret));
        esp_wifi_stop();
        return default_channel;
    }

    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK || ap_count == 0) {
        ESP_LOGI(TAG, "No AP found during channel scan, keep channel %d", default_channel);
        esp_wifi_stop();
        return default_channel;
    }

    ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        ESP_LOGW(TAG, "No memory for %d AP scan records, keep channel %d", ap_count, default_channel);
        esp_wifi_clear_ap_list();
        esp_wifi_stop();
        return default_channel;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP scan records: %s", esp_err_to_name(ret));
        free(ap_records);
        esp_wifi_stop();
        return default_channel;
    }

    for (int i = 0; i < ap_count; i++) {
        uint8_t ap_channel = ap_records[i].primary;
        int rssi_penalty = 100 + ap_records[i].rssi;

        if (rssi_penalty < 5) {
            rssi_penalty = 5;
        }

        for (size_t j = 0; j < candidate_count; j++) {
            uint8_t candidate = candidate_channels[j];

            if (candidate <= 14 && ap_channel <= 14) {
                int distance = abs((int)candidate - (int)ap_channel);

                if (distance == 0) {
                    channel_scores[j] += rssi_penalty + 30;
                } else if (distance < 5) {
                    channel_scores[j] += (rssi_penalty * (5 - distance)) / 5;
                }
            } else if (candidate == ap_channel) {
                channel_scores[j] += rssi_penalty + 30;
            }
        }
    }

    size_t best_index = 0;
    for (size_t i = 1; i < candidate_count; i++) {
        if (channel_scores[i] < channel_scores[best_index]) {
            best_index = i;
        }
    }

    uint8_t selected_channel = candidate_channels[best_index];
    ESP_LOGI(TAG, "Wi-Fi scan found %d APs, selected channel %d score %d",
             ap_count, selected_channel, channel_scores[best_index]);

    free(ap_records);
    esp_wifi_stop();

    return selected_channel;
}

static void wifi_apply_radio_tuning(void)
{
    esp_err_t ret;

    ret = esp_wifi_set_country_code("CN", false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set Wi-Fi country code CN: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable Wi-Fi power save: %s", esp_err_to_name(ret));
    }
}

static void wifi_init_softap(void)
{
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    esp_netif_ip_info_t ap_ip_info;
    IP4_ADDR(&ap_ip_info.ip, 192, 168, 5, 1);
    IP4_ADDR(&ap_ip_info.gw, 192, 168, 5, 1);
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_apply_radio_tuning();

    uint8_t device_mac[6];
    char ap_ssid[sizeof(((wifi_config_t *)0)->ap.ssid) + 1];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, device_mac));
    snprintf(ap_ssid, sizeof(ap_ssid), "%.24s-%02X%02X%02X",
             EXAMPLE_WIFI_AP_SSID, device_mac[3], device_mac[4], device_mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .channel = EXAMPLE_WIFI_AP_CHANNEL,
            .password = EXAMPLE_WIFI_AP_PASSWORD,
            .max_connection = EXAMPLE_WIFI_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .beacon_interval = 100,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    memcpy(wifi_config.ap.ssid, ap_ssid, strlen(ap_ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);

    if (strlen(EXAMPLE_WIFI_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

#if CONFIG_EXAMPLE_WIFI_AP_AUTO_CHANNEL
    wifi_config.ap.channel = wifi_select_best_ap_channel(EXAMPLE_WIFI_AP_AUTO_FALLBACK_CHANNEL);
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (cfg_err != ESP_OK && wifi_config.ap.channel >= 36) {
        ESP_LOGW(TAG, "Direct 5 GHz AP pre-config returned %s, configuring channel 1 before start", esp_err_to_name(cfg_err));
        uint8_t target_ch = wifi_config.ap.channel;
        wifi_config.ap.channel = 1;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        wifi_config.ap.channel = target_ch;
    }
    ESP_ERROR_CHECK(esp_wifi_start());

#if CONFIG_SOC_WIFI_SUPPORT_5G || CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G
    /* ESP-Hosted requires Wi-Fi to be started before changing band mode. */
    ESP_ERROR_CHECK(wifi_configure_ap_radio());
    if (wifi_config.ap.channel >= 36) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    }
#else
    ESP_LOGW(TAG, "5GHz Wi-Fi is not enabled in sdkconfig; SoftAP will use the configured channel");
    wifi_set_ap_bandwidth_40mhz();
#endif

    esp_err_t inactive_ret = esp_wifi_set_inactive_time(WIFI_IF_AP, 60);
    if (inactive_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set AP inactive time: %s", esp_err_to_name(inactive_ret));
    }

    uint8_t active_channel = 0;
    wifi_second_chan_t active_second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&active_channel, &active_second);

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info));
    char ip_address[16];
    snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Connect to SSID '%s', then open http://%s", wifi_config.ap.ssid, ip_address);
    ESP_LOGI(TAG, "Wi-Fi AP started. SSID:%s password:%s channel:%d (driver channel:%d) band:%s",
             ap_ssid,
             strlen(EXAMPLE_WIFI_AP_PASSWORD) ? EXAMPLE_WIFI_AP_PASSWORD : "<open>",
             wifi_config.ap.channel,
             active_channel,
             active_channel >= 36 ? "5GHz" : "2.4GHz");
    ESP_LOGI(TAG, "Open http://" IPSTR " or http://%s.local",
             IP2STR(&ip_info.ip), EXAMPLE_MDNS_HOST_NAME);
}

esp_err_t network_service_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Failed to initialize TCP/IP stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Failed to create event loop");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                   wifi_station_event_handler, NULL),
                        TAG, "Failed to register Wi-Fi connect handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                   wifi_station_event_handler, NULL),
                        TAG, "Failed to register Wi-Fi disconnect handler");

    wifi_init_softap();
    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(EXAMPLE_MDNS_HOST_NAME);

    return ESP_OK;
}
