#include "scans/wifi/govee_scan.h"

#include "core/glog.h"
#include "core/utils.h"
#include "managers/wifi_manager.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define GOVEE_DISCOVERY_ADDRESS "239.255.255.250"
#define GOVEE_DISCOVERY_PORT 4002
#define GOVEE_CONTROL_PORT 4003
#define GOVEE_SCAN_DURATION_US (3000000LL)
#define GOVEE_SCAN_TASK_STACK 4096

static govee_device_t *g_devices;
static volatile int g_device_count;
static volatile bool g_scan_running;
static volatile bool g_scan_done;
static volatile bool g_scan_cancelled;

static const char *json_string(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static void add_scan_result(const char *response, const struct sockaddr_in *from) {
    cJSON *root = cJSON_Parse(response);
    if (!root) return;

    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
    const cJSON *data = msg ? cJSON_GetObjectItemCaseSensitive(msg, "data") : NULL;
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return;
    }

    const char *ip = json_string(data, "ip");
    char source_ip[16] = {0};
    if (!ip[0] && from && inet_ntoa_r(from->sin_addr, source_ip, sizeof(source_ip))) {
        ip = source_ip;
    }
    if (!ip[0]) {
        cJSON_Delete(root);
        return;
    }

    for (int i = 0; i < g_device_count; i++) {
        if (strcmp(g_devices[i].ip, ip) == 0) {
            cJSON_Delete(root);
            return;
        }
    }
    if (g_device_count >= GOVEE_SCAN_MAX_RESULTS) {
        cJSON_Delete(root);
        return;
    }

    govee_device_t *device = &g_devices[g_device_count++];
    snprintf(device->ip, sizeof(device->ip), "%s", ip);
    snprintf(device->device, sizeof(device->device), "%s", json_string(data, "device"));
    snprintf(device->sku, sizeof(device->sku), "%s", json_string(data, "sku"));
    snprintf(device->version, sizeof(device->version), "%s", json_string(data, "wifiVersionSoft"));
    glog("Govee: %s  %s  %s\n", device->ip,
         device->sku[0] ? device->sku : "Unknown model", device->device);
    cJSON_Delete(root);
}

static void govee_scan_task(void *arg) {
    (void)arg;
    static const char scan_message[] =
        "{\"msg\":{\"cmd\":\"scan\",\"data\":{\"account_topic\":\"reserve\"}}}";

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        glog("Govee scan socket failed: errno %d\n", errno);
        g_scan_running = false;
        g_scan_done = true;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_port = htons(GOVEE_DISCOVERY_PORT);
    target.sin_addr.s_addr = inet_addr(GOVEE_DISCOVERY_ADDRESS);
    if (sendto(sock, scan_message, sizeof(scan_message) - 1, 0,
               (struct sockaddr *)&target, sizeof(target)) < 0) {
        glog("Govee scan send failed: errno %d\n", errno);
        close(sock);
        g_scan_running = false;
        g_scan_done = true;
        vTaskDelete(NULL);
        return;
    }

    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int64_t deadline = esp_timer_get_time() + GOVEE_SCAN_DURATION_US;
    char response[512];
    while (!g_scan_cancelled && esp_timer_get_time() < deadline) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        int received = recvfrom(sock, response, sizeof(response) - 1, 0,
                                (struct sockaddr *)&from, &from_len);
        if (received > 0) {
            response[received] = '\0';
            add_scan_result(response, &from);
        }
    }

    close(sock);
    g_scan_running = false;
    g_scan_done = true;
    vTaskDelete(NULL);
}

void govee_scan_clear_results(void) {
    free(g_devices);
    g_devices = NULL;
    g_device_count = 0;
}

esp_err_t govee_scan_start_async(void) {
    if (g_scan_running) return ESP_ERR_INVALID_STATE;
    if (!is_wifi_sta_connected()) {
        glog("Connect to WiFi before scanning for Govee devices\n");
        return ESP_ERR_INVALID_STATE;
    }

    govee_scan_clear_results();
    g_devices = calloc(GOVEE_SCAN_MAX_RESULTS, sizeof(*g_devices));
    if (!g_devices) {
        glog("Govee scan result allocation failed\n");
        g_scan_done = true;
        return ESP_ERR_NO_MEM;
    }
    g_scan_cancelled = false;
    g_scan_done = false;
    g_scan_running = true;
    if (xTaskCreate(govee_scan_task, "govee_scan", GOVEE_SCAN_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        free(g_devices);
        g_devices = NULL;
        g_scan_running = false;
        g_scan_done = true;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool govee_scan_check_done(void) {
    return g_scan_done;
}

bool govee_scan_is_running(void) {
    return g_scan_running;
}

void govee_scan_cancel(void) {
    g_scan_cancelled = true;
}

int govee_scan_get_count(void) {
    return g_device_count;
}

const govee_device_t *govee_scan_get_device(int index) {
    return g_devices && index >= 0 && index < g_device_count ? &g_devices[index] : NULL;
}

static esp_err_t send_control_message(const char *ip, const char *message) {
    if (!ip || !message) return ESP_ERR_INVALID_ARG;

    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_port = htons(GOVEE_CONTROL_PORT);
    target.sin_addr.s_addr = inet_addr(ip);
    if (target.sin_addr.s_addr == INADDR_NONE) return ESP_ERR_INVALID_ARG;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return ESP_FAIL;
    int sent = sendto(sock, message, strlen(message), 0, (struct sockaddr *)&target, sizeof(target));
    close(sock);
    return sent < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t govee_set_power(const char *ip, bool on) {
    char message[80];
    snprintf(message, sizeof(message), "{\"msg\":{\"cmd\":\"turn\",\"data\":{\"value\":%d}}}", on ? 1 : 0);
    return send_control_message(ip, message);
}

esp_err_t govee_set_brightness(const char *ip, uint8_t brightness) {
    char message[88];
    snprintf(message, sizeof(message), "{\"msg\":{\"cmd\":\"brightness\",\"data\":{\"value\":%u}}}", brightness);
    return send_control_message(ip, message);
}

esp_err_t govee_set_color(const char *ip, uint8_t red, uint8_t green, uint8_t blue) {
    char message[144];
    snprintf(message, sizeof(message),
             "{\"msg\":{\"cmd\":\"colorwc\",\"data\":{\"color\":{\"r\":%u,\"g\":%u,\"b\":%u},\"colorTemInKelvin\":0}}}",
             red, green, blue);
    return send_control_message(ip, message);
}

esp_err_t govee_request_status(const char *ip) {
    return send_control_message(ip, "{\"msg\":{\"cmd\":\"devStatus\",\"data\":{}}}");
}
