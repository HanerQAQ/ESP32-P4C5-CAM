#include "app_myperiph.h"
#include "app_mywifi.h"
#include "camera_service.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "app_myperiph";

#define BOOT_BUTTON_GPIO      GPIO_NUM_35
#define BOOT_ALT_BUTTON_GPIO  GPIO_NUM_26
#define BOOT_LONG_PRESS_MS    1000
#define BOOT_DEBOUNCE_MS      30

typedef struct {
    gpio_num_t pin;
    int raw;
    int stable;
    int64_t edge_ms;
    int64_t pressed_ms;
    bool long_handled;
} boot_btn_t;

static boot_btn_t s_btn_primary = { .pin = BOOT_BUTTON_GPIO, .raw = 1, .stable = 1 };
static boot_btn_t s_btn_alt = { .pin = BOOT_ALT_BUTTON_GPIO, .raw = 1, .stable = 1 };
static volatile int s_boot_level_primary = 1;
static volatile int s_boot_level_alt = 1;
static volatile uint32_t s_boot_press_count = 0;

static void boot_enter_provisioning(const char *why)
{
    ESP_LOGI(TAG, "BOOT %s: disconnecting STA and entering AP provisioning", why);
    esp_err_t err = app_mywifi_enter_provisioning();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Provisioning mode request failed: %s", esp_err_to_name(err));
    }
}

static void print_system_info_banner(void)
{
    uint32_t w = 640, h = 480;
    camera_service_get_resolution(&w, &h);
    float fps = camera_service_get_fps();

    printf("\r\n=======================================================\r\n"
           " [ESP32-P4C5-CAM System Status]\r\n"
           " SoftAP:            %s (SSID: %s, IP: http://%s)\r\n"
           " Config URL:        http://%s/wifi\r\n"
           " STA Wi-Fi State:   %s\r\n"
           " STA Router SSID:   %s\r\n"
           " STA IP Address:    %s\r\n"
           " Camera Web UI:     http://%s/\r\n"
           " MJPEG Stream URL:  http://%s:81/stream\r\n"
           " Snapshot URL:      http://%s/capture\r\n"
           " Camera Resolution: %" PRIu32 "x%" PRIu32 " (%.1f FPS)\r\n"
           "=======================================================\r\n\r\n",
           app_mywifi_is_ap_active() ? "ON" : "OFF (hold BOOT to provision)",
           app_mywifi_get_ap_ssid(),
           app_mywifi_get_ap_ip(),
           app_mywifi_is_ap_active() ? app_mywifi_get_ap_ip() :
           (app_mywifi_is_sta_connected() ? app_mywifi_get_sta_ip() : app_mywifi_get_ap_ip()),
           app_mywifi_get_disconnect_message(),
           app_mywifi_get_sta_ssid(),
           app_mywifi_is_sta_connected() ? app_mywifi_get_sta_ip() : "None",
           app_mywifi_is_sta_connected() ? app_mywifi_get_sta_ip() : app_mywifi_get_ap_ip(),
           app_mywifi_is_sta_connected() ? app_mywifi_get_sta_ip() : app_mywifi_get_ap_ip(),
           app_mywifi_is_sta_connected() ? app_mywifi_get_sta_ip() : app_mywifi_get_ap_ip(),
           w, h, fps);
}

static void boot_button_poll(boot_btn_t *btn)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    int level = gpio_get_level(btn->pin);
    if (btn->pin == BOOT_BUTTON_GPIO) {
        s_boot_level_primary = level;
    } else {
        s_boot_level_alt = level;
    }

    if (level != btn->raw) {
        btn->raw = level;
        btn->edge_ms = now_ms;
    }
    if (btn->stable != btn->raw && now_ms - btn->edge_ms >= BOOT_DEBOUNCE_MS) {
        btn->stable = btn->raw;
        if (btn->stable == 0) {
            btn->pressed_ms = now_ms;
            btn->long_handled = false;
            s_boot_press_count++;
            ESP_LOGI(TAG, "BOOT GPIO%d pressed", btn->pin);
        } else {
            ESP_LOGI(TAG, "BOOT GPIO%d released after %lld ms", btn->pin,
                     (long long)(now_ms - btn->pressed_ms));
            if (!btn->long_handled) {
                printf("key_down!! (short press: status only)\r\n");
                print_system_info_banner();
            }
        }
    }
    if (btn->stable == 0 && !btn->long_handled &&
        now_ms - btn->pressed_ms >= BOOT_LONG_PRESS_MS) {
        btn->long_handled = true;
        printf("key_long_down!! \r\n");
        boot_enter_provisioning("long press");
    }
}

static void boot_button_task(void *arg)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO) | (1ULL << BOOT_ALT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&btn_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOOT GPIO config failed: %s", esp_err_to_name(err));
    }

    s_boot_level_primary = gpio_get_level(BOOT_BUTTON_GPIO);
    s_boot_level_alt = gpio_get_level(BOOT_ALT_BUTTON_GPIO);
    ESP_LOGI(TAG, "BOOT inputs: GPIO%d=%d, GPIO%d=%d (active low); short press=status, 1s hold=provisioning",
             BOOT_BUTTON_GPIO, s_boot_level_primary, BOOT_ALT_BUTTON_GPIO, s_boot_level_alt);

    while (1) {
        boot_button_poll(&s_btn_primary);
        boot_button_poll(&s_btn_alt);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void process_uart_command(char *cmd)
{
    // Trim whitespace and newlines
    while (*cmd == ' ' || *cmd == '\t' || *cmd == '\r' || *cmd == '\n') cmd++;
    int len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t' || cmd[len - 1] == '\r' || cmd[len - 1] == '\n')) {
        cmd[--len] = '\0';
    }
    if (len == 0) return;

    if (strcasecmp(cmd, "sta_ip") == 0) {
        if (app_mywifi_is_sta_connected()) {
            printf("sta_ip:%s\r\n", app_mywifi_get_sta_ip());
        } else {
            printf("sta_ip:null\r\n");
        }
    } else if (strcasecmp(cmd, "ap_ip") == 0) {
        printf("ap_ip:%s\r\n", app_mywifi_get_ap_ip());
    } else if (strcasecmp(cmd, "status") == 0) {
        uint32_t w = 640, h = 480;
        camera_service_get_resolution(&w, &h);
        float fps = camera_service_get_fps();
        printf("{\"board\":\"ESP32-P4\",\"sta_connected\":%s,\"sta_ip\":\"%s\",\"ap_ip\":\"%s\","
               "\"w\":%" PRIu32 ",\"h\":%" PRIu32 ",\"fps\":%.1f}\r\n",
               app_mywifi_is_sta_connected() ? "true" : "false",
               app_mywifi_get_sta_ip(),
               app_mywifi_get_ap_ip(),
               w, h, fps);
    } else if (strcasecmp(cmd, "reset_wifi") == 0) {
        printf("Clearing Wi-Fi credentials from NVS and restarting...\r\n");
        app_mywifi_clear_sta_creds();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (strcasecmp(cmd, "restore_ap") == 0) {
        printf("Restoring 2.4G SoftAP (STA will be disconnected)...\r\n");
        app_mywifi_restore_ap();
    } else if (strcasecmp(cmd, "provision_ap") == 0) {
        printf("Entering provisioning mode (STA disconnected, saved creds kept)...\r\n");
        app_mywifi_enter_provisioning();
    } else if (strcasecmp(cmd, "recover") == 0) {
        printf("Recovery reboot: forcing AP provisioning...\r\n");
        app_mywifi_force_recovery_reboot();
    } else if (strcasecmp(cmd, "boot") == 0) {
        printf("boot_gpio:%d level:%d alt_gpio:%d level:%d presses:%u\r\n",
               BOOT_BUTTON_GPIO, s_boot_level_primary,
               BOOT_ALT_BUTTON_GPIO, s_boot_level_alt,
               (unsigned)s_boot_press_count);
    } else if (strcasecmp(cmd, "reboot") == 0) {
        printf("Rebooting device...\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (strcasecmp(cmd, "help") == 0) {
        printf("\r\nAvailable UART Commands:\r\n"
               "  sta_ip     - Query Station IP address\r\n"
               "  ap_ip      - Query SoftAP IP address\r\n"
               "  status     - Query current JSON status\r\n"
               "  reset_wifi - Clear saved Wi-Fi and reboot to AP\r\n"
               "  restore_ap - Disconnect STA and open 2.4G SoftAP\r\n"
               "  provision_ap - Disconnect STA and enter AP provisioning\r\n"
               "  recover    - Recovery reboot into AP provisioning\r\n"
               "  boot       - Show BOOT input levels / press count\r\n"
               "  reboot     - Restart board\r\n"
               "  help       - Show this command list\r\n\r\n");
    } else {
        printf("Unknown command: '%s'. Type 'help' for command list.\r\n", cmd);
    }
}

static void uart_cmd_task(void *arg)
{
    char line_buf[128];
    int line_idx = 0;

    // Use non-blocking mode on stdin
    int flags = fcntl(fileno(stdin), F_GETFL);
    fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);

    while (1) {
        int c = fgetc(stdin);
        if (c != EOF) {
            if (c == '\r' || c == '\n') {
                if (line_idx > 0) {
                    line_buf[line_idx] = '\0';
                    process_uart_command(line_buf);
                    line_idx = 0;
                }
            } else if (line_idx < sizeof(line_buf) - 1) {
                line_buf[line_idx++] = (char)c;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

esp_err_t app_myperiph_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // Drain USB RX in the ISR even while the command task handles a request.
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usj_ret = usb_serial_jtag_driver_install(&usj_cfg);
    if (usj_ret == ESP_OK || usj_ret == ESP_ERR_INVALID_STATE) {
        usb_serial_jtag_vfs_use_driver();
    } else {
        ESP_LOGW(TAG, "USB Serial JTAG driver install failed: %s", esp_err_to_name(usj_ret));
    }
#endif

    BaseType_t ret1 = xTaskCreate(boot_button_task, "boot_btn_task", 3072, NULL, 4, NULL);
    BaseType_t ret2 = xTaskCreate(uart_cmd_task, "uart_cmd_task", 3072, NULL, 4, NULL);

    if (ret1 != pdPASS || ret2 != pdPASS) {
        ESP_LOGE(TAG, "Failed to create peripheral task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Hardware Peripherals initialized (BOOT GPIO35 + USB Serial CLI)");
    return ESP_OK;
}
