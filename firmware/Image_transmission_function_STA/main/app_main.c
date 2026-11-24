#include "camera_service.h"
#include "network_service.h"
#include "app_mywifi.h"
#include "app_myi2c.h"
#include "app_myperiph.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "MAIN_STA";

void app_main(void)
{
    esp_err_t camera_err = camera_service_init();
    if (camera_err != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed: %s", esp_err_to_name(camera_err));
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "Initializing peripherals (BOOT GPIO35 & USB Serial CLI)...");
    ESP_ERROR_CHECK(app_myperiph_init());
    printf("ESP32-P4C5-CAM Board Ver:AI_V1.5.2 (Image Transmission STA I2C:0x52)\r\n");

    /* Initialize 32-byte I2C Slave (0x52) */
    ESP_LOGI(TAG, "Initializing ESP32-P4C5-CAM I2C Slave (0x52, SCL=IO47, SDA=IO46)...");
    ESP_ERROR_CHECK(app_myi2c_init());

    ESP_LOGI(TAG, "Starting Wi-Fi network service (AP+STA mode)...");
    ESP_ERROR_CHECK(network_service_start());

    ESP_LOGI(TAG, "Starting Camera HTTP servers (port 80 & 81)...");
    ESP_ERROR_CHECK(camera_service_start());

    bool sta_connected = app_mywifi_is_sta_connected();
    bool ap_active = app_mywifi_is_ap_active();
    const char *web_host = sta_connected ? app_mywifi_get_sta_ip() : app_mywifi_get_ap_ip();

    ESP_LOGI(TAG, "=======================================================");
    ESP_LOGI(TAG, "  ESP32-P4C5-CAM Image Transmission STA Started  ");
    if (sta_connected) {
        ESP_LOGI(TAG, "  Wi-Fi Mode    : STA connected to '%s' (http://%s)",
                 app_mywifi_get_sta_ssid(), app_mywifi_get_sta_ip());
    } else {
        ESP_LOGI(TAG, "  Wi-Fi Mode    : STA connecting (%s)", app_mywifi_get_disconnect_message());
    }
    if (ap_active) {
        ESP_LOGI(TAG, "  SoftAP        : %s (IP: http://%s, ch %d)",
                 app_mywifi_get_ap_ssid(), app_mywifi_get_ap_ip(), AP_CHANNEL_2G_DEFAULT);
        ESP_LOGI(TAG, "  Wi-Fi Config  : http://%s/wifi", app_mywifi_get_ap_ip());
    } else {
        ESP_LOGI(TAG, "  SoftAP        : closed (long-press BOOT to reopen)");
        ESP_LOGI(TAG, "  Wi-Fi Config  : http://%s/wifi", web_host);
    }
    ESP_LOGI(TAG, "  Camera Web UI : http://%s/ (or /vue, /cam)", web_host);
    ESP_LOGI(TAG, "  MJPEG Stream  : http://%s:81/stream (redirected from :80/stream)", web_host);
    ESP_LOGI(TAG, "  Snapshot JPEG : http://%s/capture (or /api/frame.jpg)", web_host);
    ESP_LOGI(TAG, "  I2C Slave     : 0x52 (SCL=GPIO47, SDA=GPIO46)");
    ESP_LOGI(TAG, "  USB Serial CLI: Available (type 'help' for commands)");
    ESP_LOGI(TAG, "  BOOT Button   : Short press = status, 1s hold = disconnect STA + provisioning AP");
    ESP_LOGI(TAG, "=======================================================");
}
