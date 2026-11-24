#include "camera_service.h"
#include "network_service.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "hardware_test.h"

void app_main(void)
{
    hardware_test_init();
    esp_err_t camera_err = camera_service_init();
    if (camera_err != ESP_OK) {
        hardware_test_camera_error();
        ESP_LOGE("APRILTAG_WEB", "Camera initialization failed: %s", esp_err_to_name(camera_err));
    }
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(network_service_start());
    ESP_ERROR_CHECK(camera_service_start());
    ESP_LOGI("APRILTAG_WEB", "Open http://192.168.5.1 after connecting to the board AP");
}
