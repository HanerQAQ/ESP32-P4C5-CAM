/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "dns_server.h"

#define DNS_PORT               53
#define DNS_MAX_PACKET         512
#define DNS_MAX_QUESTIONS      8
#define DNS_TTL_SEC            60

#define DNS_FLAG_QR            0x8000
#define DNS_FLAG_OPCODE_MASK   0x7800
#define DNS_FLAG_AA            0x0400
#define DNS_FLAG_RD            0x0100
#define DNS_FLAG_RA            0x0080
#define DNS_RCODE_NXDOMAIN     3

#define DNS_TYPE_A             1
#define DNS_CLASS_IN           1

static const char *TAG = "dns_redirect";

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((__packed__)) {
    uint16_t name_ptr;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

struct dns_server_handle {
    volatile bool started;
    TaskHandle_t task;
    int sock;
    uint32_t ap_ip;
};

static uint16_t read_u16(const uint8_t *buf, size_t off)
{
    return ((uint16_t)buf[off] << 8) | buf[off + 1];
}

/* Read a DNS name, following compression pointers. *off is advanced past the
 * name as it appears in the original packet, even when a pointer was followed. */
static bool read_dns_name(const uint8_t *buf, size_t len, size_t *off,
                          char *name, size_t name_size)
{
    size_t p = *off;
    size_t out = 0;
    size_t after_name = 0;
    bool jumped = false;
    bool terminated = false;
    int hops = 0;

    while (p < len) {
        uint8_t label_len = buf[p];
        if (label_len == 0) {
            p++;
            if (!jumped) after_name = p;
            terminated = true;
            break;
        }
        if ((label_len & 0xC0) == 0xC0) {
            if (p + 1 >= len) return false;
            uint16_t ptr = ((uint16_t)(label_len & 0x3F) << 8) | buf[p + 1];
            if (!jumped) after_name = p + 2;
            if (ptr >= len || ++hops > 8) return false;
            p = ptr;
            jumped = true;
            continue;
        }
        if (label_len & 0xC0) return false;
        if (p + 1 + label_len > len) return false;
        if (out + label_len + 1 >= name_size) return false;
        memcpy(name + out, buf + p + 1, label_len);
        out += label_len;
        name[out++] = '.';
        p += 1 + label_len;
    }

    if (!terminated) return false;
    if (out > 0) out--;
    name[out] = '\0';
    *off = jumped ? after_name : p;
    return true;
}

static void handle_dns_query(dns_server_handle_t h, const uint8_t *req, int req_len,
                             const struct sockaddr *src, socklen_t src_len)
{
    if (req_len < (int)sizeof(dns_header_t)) return;

    dns_header_t req_hdr;
    memcpy(&req_hdr, req, sizeof(req_hdr));
    uint16_t req_flags = ntohs(req_hdr.flags);
    if (req_flags & DNS_FLAG_QR) return;
    if (req_flags & DNS_FLAG_OPCODE_MASK) return;

    uint16_t qd_count = ntohs(req_hdr.qd_count);
    if (qd_count > DNS_MAX_QUESTIONS) return;

    struct question {
        size_t name_off;
        uint16_t type;
        uint16_t class;
    } questions[DNS_MAX_QUESTIONS];

    size_t off = sizeof(dns_header_t);
    bool canary = false;

    for (uint16_t i = 0; i < qd_count; i++) {
        questions[i].name_off = off;
        char name[256];
        if (!read_dns_name(req, req_len, &off, name, sizeof(name))) return;
        if (off + 4 > (size_t)req_len) return;
        questions[i].type = read_u16(req, off);
        questions[i].class = read_u16(req, off + 2);
        off += 4;
        if (strcasecmp(name, "use-application-dns.net") == 0) {
            canary = true;
        }
    }

    size_t question_end = off;
    if (question_end > DNS_MAX_PACKET) return;

    uint8_t resp[DNS_MAX_PACKET];
    memcpy(resp, req, question_end);

    dns_header_t *resp_hdr = (dns_header_t *)resp;
    bool nxdomain = canary && qd_count == 1;
    resp_hdr->flags = htons(DNS_FLAG_QR | DNS_FLAG_AA | DNS_FLAG_RA |
                            (req_flags & DNS_FLAG_RD) |
                            (nxdomain ? DNS_RCODE_NXDOMAIN : 0));
    resp_hdr->qd_count = htons(qd_count);
    resp_hdr->an_count = 0;
    resp_hdr->ns_count = 0;
    resp_hdr->ar_count = 0;

    size_t answer_off = question_end;
    uint16_t answer_count = 0;

    if (!nxdomain) {
        for (uint16_t i = 0; i < qd_count; i++) {
            if (questions[i].class != DNS_CLASS_IN || questions[i].type != DNS_TYPE_A) {
                continue; /* AAAA and other types are valid NODATA responses. */
            }
            if (answer_off + sizeof(dns_answer_t) > sizeof(resp)) {
                break;
            }
            dns_answer_t *answer = (dns_answer_t *)(resp + answer_off);
            answer->name_ptr = htons(0xC000 | (uint16_t)questions[i].name_off);
            answer->type = htons(DNS_TYPE_A);
            answer->class = htons(DNS_CLASS_IN);
            answer->ttl = htonl(DNS_TTL_SEC);
            answer->addr_len = htons(sizeof(answer->ip_addr));
            answer->ip_addr = h->ap_ip;
            answer_off += sizeof(dns_answer_t);
            answer_count++;
        }
    }

    resp_hdr->an_count = htons(answer_count);

    int sent = sendto(h->sock, resp, answer_off, 0, src, src_len);
    if (sent < 0) {
        ESP_LOGD(TAG, "sendto failed: errno %d", errno);
    }
}

static void dns_server_task(void *pv)
{
    dns_server_handle_t h = (dns_server_handle_t)pv;
    uint8_t rx[DNS_MAX_PACKET];

    while (h->started) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        struct timeval timeout = { .tv_sec = 0, .tv_usec = 500000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        struct sockaddr_in bind_addr = { 0 };
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(DNS_PORT);
        bind_addr.sin_addr.s_addr = h->ap_ip ? h->ap_ip : htonl(INADDR_ANY);

        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGE(TAG, "bind failed: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        h->sock = sock;
        char addr_str[16];
        inet_ntoa_r(bind_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "DNS server listening on %s:%d", addr_str, DNS_PORT);

        while (h->started) {
            struct sockaddr_storage source;
            socklen_t source_len = sizeof(source);
            int len = recvfrom(sock, rx, sizeof(rx), 0,
                               (struct sockaddr *)&source, &source_len);
            if (len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                ESP_LOGW(TAG, "recvfrom failed: errno %d", errno);
                break;
            }
            handle_dns_query(h, rx, len, (struct sockaddr *)&source, source_len);
        }

        close(sock);
        h->sock = -1;
        if (h->started) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    h->task = NULL;
    vTaskDelete(NULL);
}

dns_server_handle_t start_dns_server(dns_server_config_t *config)
{
    if (!config) return NULL;

    dns_server_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle) return NULL;

    handle->sock = -1;
    handle->started = true;

    for (int i = 0; i < config->num_of_entries; i++) {
        if (config->item[i].if_key) {
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey(config->item[i].if_key);
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr) {
                    handle->ap_ip = ip_info.ip.addr;
                    break;
                }
            }
        } else if (config->item[i].ip.addr) {
            handle->ap_ip = config->item[i].ip.addr;
            break;
        }
    }

    if (!handle->ap_ip) {
        ESP_LOGW(TAG, "AP IP not available yet; DNS answers will be 0.0.0.0");
    }

    if (xTaskCreate(dns_server_task, "dns_server", 4096, handle, 5, &handle->task) != pdPASS) {
        free(handle);
        return NULL;
    }
    return handle;
}

void stop_dns_server(dns_server_handle_t handle)
{
    if (!handle) return;

    handle->started = false;
    if (handle->sock >= 0) {
        shutdown(handle->sock, SHUT_RDWR);
    }

    for (int i = 0; i < 10 && handle->task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (handle->task) {
        vTaskDelete(handle->task);
    }
    free(handle);
}
