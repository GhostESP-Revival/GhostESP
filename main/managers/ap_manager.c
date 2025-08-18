#include "managers/ap_manager.h"
#include "managers/ghost_esp_site.h"
#include "managers/settings_manager.h"
#include "core/esp_comm_manager.h"
#include <cJSON.h>
#include <core/serial_manager.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <mdns.h>
#include <nvs_flash.h>
#include <stdio.h>
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"


// Forward declarations
static esp_err_t http_get_handler(httpd_req_t *req);
static esp_err_t api_clear_logs_handler(httpd_req_t *req);
static esp_err_t api_settings_handler(httpd_req_t *req);
static esp_err_t api_command_handler(httpd_req_t *req);
static esp_err_t api_settings_get_handler(httpd_req_t *req);
static esp_err_t api_logs_handler(httpd_req_t *req);
static esp_err_t api_esp_comm_status_handler(httpd_req_t *req);
static esp_err_t api_esp_comm_control_handler(httpd_req_t *req);
static esp_err_t api_esp_comm_send_handler(httpd_req_t *req);

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data);

static esp_err_t load_server_config(void);
static esp_err_t start_http_server(void);
static esp_err_t stop_http_server(void);
static void reset_server_config(void);
static bool is_server_running(void);
static bool is_config_loaded(void);
static esp_err_t setup_mdns(void);
static esp_err_t teardown_mdns(void);

#define MAX_LOG_BUFFER_SIZE (8 * 1024)  // 8KB log buffer size
#define LOG_CHUNK_SIZE (MAX_LOG_BUFFER_SIZE / 4)  // Size to remove when buffer is full
#define MAX_FILE_SIZE (5 * 1024 * 1024) // 5 MB
#define AP_MANAGER_BUFFER_SIZE (1024)   // 1 KB buffer size for reading chunks
#define MIN_(a, b) ((a) < (b) ? (a) : (b))
#define SERIAL_BUFFER_SIZE 528          // Size of serial buffer

static char *log_buffer = NULL; // dynamically allocated at runtime
static size_t log_buffer_index = 0;
static SemaphoreHandle_t log_mutex = NULL;

static const char *TAG = "AP_MANAGER";
static httpd_handle_t server = NULL;
static esp_netif_t *netif = NULL;
static bool mdns_freed = false;

static httpd_config_t server_config;
static httpd_uri_t uri_handlers[20];
static int handler_count = 0;
static bool config_loaded = false;

static esp_err_t scan_directory(const char *base_path, cJSON *json_array) {
    DIR *dir = opendir(base_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", base_path);
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Dynamically allocate memory for full_path
        size_t full_path_len = strlen(base_path) + strlen(entry->d_name) + 2; // +2 for '/' and '\0'
        char *full_path = malloc(full_path_len);
        if (!full_path) {
            ESP_LOGE(TAG, "Failed to allocate memory for full path.");
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }

        snprintf(full_path, full_path_len, "%s/%s", base_path, entry->d_name);

        struct stat entry_stat;
        if (stat(full_path, &entry_stat) != 0) {
            ESP_LOGE(TAG, "Failed to stat file: %s", full_path);
            free(full_path);
            continue;
        }

        if (S_ISDIR(entry_stat.st_mode)) {
            // Add folder
            cJSON *folder = cJSON_CreateObject();
            cJSON_AddStringToObject(folder, "name", entry->d_name);
            cJSON_AddStringToObject(folder, "type", "folder");

            // Recursively scan children
            cJSON *children = cJSON_CreateArray();
            if (scan_directory(full_path, children) == ESP_OK) {
                cJSON_AddItemToObject(folder, "children", children);
            } else {
                cJSON_Delete(children);
            }

            cJSON_AddItemToArray(json_array, folder);
        } else if (S_ISREG(entry_stat.st_mode)) {
            // Add file
            cJSON *file = cJSON_CreateObject();
            cJSON_AddStringToObject(file, "name", entry->d_name);
            cJSON_AddStringToObject(file, "type", "file");
            cJSON_AddStringToObject(file, "path", full_path);
            cJSON_AddItemToArray(json_array, file);
        }

        // Free dynamically allocated memory
        free(full_path);
    }

    closedir(dir);
    return ESP_OK;
}

static esp_err_t api_sd_card_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Received request for SD card structure.");

    const char *base_path = "/mnt";

    struct stat st;
    if (stat(base_path, &st) != 0) {
        ESP_LOGE(TAG, "SD card not mounted or inaccessible.");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"SD card not supported or not mounted.\"}");
        return ESP_FAIL;
    }

    cJSON *response_json = cJSON_CreateObject();
    if (!response_json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to create JSON object.\"}");
        return ESP_FAIL;
    }

    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(base_path, &total_bytes, &free_bytes);

    if (ret == ESP_OK) {
        cJSON *storage_info = cJSON_CreateObject();
        cJSON_AddNumberToObject(storage_info, "total", total_bytes);
        cJSON_AddNumberToObject(storage_info, "used", total_bytes - free_bytes);
        cJSON_AddItemToObject(response_json, "storage", storage_info);
    } else {
        ESP_LOGW(TAG, "Could not get FATFS info (%s)", esp_err_to_name(ret));
    }

    cJSON *files_array = cJSON_CreateArray();
    if (scan_directory(base_path, files_array) != ESP_OK) {
        cJSON_Delete(response_json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to scan SD card.\"}");
        return ESP_FAIL;
    }
    cJSON_AddItemToObject(response_json, "files", files_array);

    char *response_string = cJSON_Print(response_json);
    if (!response_string) {
        ESP_LOGE(TAG, "Failed to serialize JSON.");
        cJSON_Delete(response_json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to serialize SD card data.\"}");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_string);

    cJSON_Delete(response_json);
    free(response_string);

    return ESP_OK;
}

static esp_err_t api_sd_card_post_handler(httpd_req_t *req) {
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf));
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive request payload.");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid request payload.\"}");
        return ESP_FAIL;
    }

    // Parse JSON payload
    buf[received] = '\0'; // Null-terminate the received string
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON payload.");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON payload.\"}");
        return ESP_FAIL;
    }

    cJSON *path_item = cJSON_GetObjectItem(json, "path");
    if (!cJSON_IsString(path_item) || !path_item->valuestring) {
        ESP_LOGE(TAG, "Missing or invalid 'path' in request payload.");
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"'path' is required and must be a string.\"}");
        return ESP_FAIL;
    }

    const char *file_path = path_item->valuestring;

    // Open the file
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        cJSON_Delete(json);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\": \"File not found.\"}");
        return ESP_FAIL;
    }

    // Set response headers for chunked transfer
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");

    // Allocate a buffer for sending chunks
    char *chunk_buf = malloc(AP_MANAGER_BUFFER_SIZE);
    if (!chunk_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for chunk buffer.");
        fclose(file);
        cJSON_Delete(json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed.\"}");
        return ESP_FAIL;
    }

    size_t bytes_read;
    esp_err_t ret = ESP_OK;
    while ((bytes_read = fread(chunk_buf, 1, AP_MANAGER_BUFFER_SIZE, file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk_buf, bytes_read) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send file chunk.");
            ret = ESP_FAIL;
            break;
        }
    }

    // Send final, zero-length chunk
    if (ret == ESP_OK) {
        if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send final chunk.");
            ret = ESP_FAIL;
        }
    }

    fclose(file);
    free(chunk_buf);
    cJSON_Delete(json);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "File sent successfully: %s", file_path);
    } else {
        ESP_LOGE(TAG, "File download failed for: %s", file_path);
    }

    return ret;
}

#define MAX_PATH_LENGTH 512

esp_err_t get_query_param(httpd_req_t *req, const char *key, char *value, size_t max_len) {
    size_t query_len = httpd_req_get_url_query_len(req) + 1;

    if (query_len > 1) { // >1 because query string starts with '?'
        char *query = malloc(query_len);
        if (!query) {
            ESP_LOGE(TAG, "Failed to allocate memory for query string.");
            return ESP_ERR_NO_MEM;
        }

        if (httpd_req_get_url_query_str(req, query, query_len) == ESP_OK) {
            char encoded_value[max_len];
            if (httpd_query_key_value(query, key, encoded_value, sizeof(encoded_value)) == ESP_OK) {
                url_decode(value, encoded_value);
                free(query);
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Key '%s' not found in query string.", key);
            }
        } else {
            ESP_LOGE(TAG, "Failed to get query string.");
        }

        free(query);
    } else {
        ESP_LOGE(TAG, "No query string found in the URL.");
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t api_sd_card_delete_file_handler(httpd_req_t *req) {
    char filepath[256 + 1];

    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        char query[query_len];
        httpd_req_get_url_query_str(req, query, query_len);

        char path[256];
        if (httpd_query_key_value(query, "path", path, sizeof(path)) == ESP_OK) {
            snprintf(filepath, sizeof(filepath), "%s", path);
            ESP_LOGI(TAG, "Deleting file: %s", filepath);

            struct _reent r;
            memset(&r, 0, sizeof(struct _reent));
            int res = _unlink_r(&r, filepath);
            if (res == 0) {
                ESP_LOGI(TAG, "File deleted successfully");
                httpd_resp_set_status(req, "200 OK");
                httpd_resp_send(req, "File deleted successfully", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Failed to delete file: %s, errno: %d", filepath, errno);
                httpd_resp_set_status(req, "500 Internal Server Error");
                httpd_resp_send(req, "Failed to delete the file", HTTPD_RESP_USE_STRLEN);
                return ESP_FAIL;
            }
        }
    }

    ESP_LOGE(TAG, "Invalid query parameters");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "Missing or invalid 'path' parameter", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

// Handler for uploading files to SD card
static esp_err_t api_sd_card_upload_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Received file upload request.");

    // Retrieve 'path' query parameter
    char path_param[MAX_PATH_LENGTH] = {0};
    if (get_query_param(req, "path", path_param, sizeof(path_param)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\": \"Missing or invalid 'path' query parameter.\"}");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Upload path: %s", path_param);

    // Buffer for receiving data
    char *buf = malloc(AP_MANAGER_BUFFER_SIZE + 1);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed for buffer.\"}");
        return ESP_FAIL;
    }

    char *file_path = NULL;
    FILE *file = NULL;
    int received;
    int total_received = 0;
    bool headers_parsed = false;
    char *body_start = NULL;

    while ((received = httpd_req_recv(req, buf, AP_MANAGER_BUFFER_SIZE)) > 0) {
        buf[received] = '\0';
        total_received += received;

        if (!headers_parsed) {
            body_start = strstr(buf, "\r\n\r\n");
            if (body_start) {
                headers_parsed = true;
                body_start += 4; // Move past the separator

                // Extract filename from headers
                char *filename_start = strstr(buf, "filename=\"");
                if (filename_start) {
                    filename_start += strlen("filename=\"");
                    char *filename_end = strstr(filename_start, "\"");
                    if (filename_end) {
                        char original_filename[128] = {0};
                        strncpy(original_filename, filename_start, filename_end - filename_start);

                        // Allocate memory for the full file path
                        file_path = malloc(strlen(path_param) + strlen(original_filename) + 2);
                        if (!file_path) {
                            free(buf);
                            httpd_resp_set_status(req, "500 Internal Server Error");
                            httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed for file path.\"}");
                            return ESP_FAIL;
                        }
                        snprintf(file_path, MAX_PATH_LENGTH + 128, "%s/%s", path_param, original_filename);
                        
                        ESP_LOGI(TAG, "Writing to file: %s", file_path);
                        file = fopen(file_path, "wb");
                        if (!file) {
                            free(buf);
                            free(file_path);
                            httpd_resp_set_status(req, "500 Internal Server Error");
                            httpd_resp_sendstr(req, "{\"error\": \"Failed to open file for writing.\"}");
                            return ESP_FAIL;
                        }
                        
                        // Write the first part of the file data
                        size_t data_len = received - (body_start - buf);
                        if (data_len > 0) {
                            fwrite(body_start, 1, data_len, file);
                        }
                    }
                }
            }
        } else if (file) {
            // Write subsequent chunks of file data
            fwrite(buf, 1, received, file);
        }
    }
    
    free(buf);
    if (file) {
        fclose(file);
        
        // Post-process the file to remove the boundary
        file = fopen(file_path, "r+b");
        if (file) {
            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            fseek(file, 0, SEEK_SET);
            
            char *file_buf = malloc(file_size + 1);
            if (file_buf) {
                fread(file_buf, 1, file_size, file);
                file_buf[file_size] = '\0';
                
                char *end_boundary = strstr(file_buf, "\r\n--");
                if (end_boundary) {
                    long new_size = end_boundary - file_buf;
                    rewind(file);
#ifdef _WIN32
                    _chsize(_fileno(file), new_size);
#else
                    ftruncate(fileno(file), new_size);
#endif
                }
                free(file_buf);
            }
            fclose(file);
        }
    }
    free(file_path); // Free the allocated file_path

    if (received < 0) {
        ESP_LOGE(TAG, "Error receiving file data.");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to receive file data.\"}");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "File upload finished, total bytes: %d", total_received);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "File uploaded successfully.");

    return ESP_OK;
}

esp_err_t ap_manager_init(void) {
    esp_err_t ret;
    wifi_mode_t mode;

    // --- Memory check before AP init ---
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_heap < (45 * 1024)) {
        ESP_LOGW(TAG, "WARNING: Less than 45KB of free RAM available (%d bytes). AP may fail to initialize or operate reliably!", (int)free_heap);
        //TERMINAL_VIEW_ADD_TEXT("WARNING: <45KB RAM free (%d bytes). AP may not initialize or operate reliably!\n", (int)free_heap);
    }

    // Check if AP is disabled in settings
    if (!settings_get_ap_enabled(&G_Settings)) {
        printf("Access Point disabled in settings, skipping AP initialization\n");
        
        // Initialize log buffer and mutex even when AP is disabled
        log_buffer = malloc(MAX_LOG_BUFFER_SIZE);
        if(!log_buffer){
            ESP_LOGE(TAG, "failed to alloc log buffer");
            return ESP_ERR_NO_MEM;
        }

        log_mutex = xSemaphoreCreateRecursiveMutex();
        if (!log_mutex) {
            ESP_LOGE(TAG, "Failed to create log mutex");
            free(log_buffer);
            log_buffer = NULL;
            return ESP_FAIL;
        }

        if(log_buffer){
            memset(log_buffer, 0, MAX_LOG_BUFFER_SIZE);
        }
        
        return ESP_OK;
    }

    ret = esp_wifi_get_mode(&mode);
    if (ret == ESP_ERR_WIFI_NOT_INIT) {
        printf("Wi-Fi not initialized, initializing as Access Point...\n");

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            printf("esp_wifi_init failed: %s\n", esp_err_to_name(ret));
            return ret;
        }

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!netif) {
            netif = esp_netif_create_default_wifi_ap();
            if (netif == NULL) {
                printf("Failed to create default Wi-Fi AP\n");
                return ESP_FAIL;
            }
        }
    } else if (ret == ESP_OK) {
        printf("Wi-Fi already initialized, skipping Wi-Fi init.\n");
    } else {
        printf("esp_wifi_get_mode failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        printf("esp_wifi_set_mode failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    const char *ssid = strlen(settings_get_ap_ssid(&G_Settings)) > 0
                           ? settings_get_ap_ssid(&G_Settings)
                           : "GhostNet";

    const char *password = strlen(settings_get_ap_password(&G_Settings)) > 8
                               ? settings_get_ap_password(&G_Settings)
                               : "GhostNet";

    wifi_config_t wifi_config = {
        .ap =
            {
                .channel = 6,
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK,
                .beacon_interval = 100,
            },
    };

    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';

    wifi_config.ap.ssid_len = strlen(ssid);

    strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        printf("esp_wifi_set_config failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        printf("Failed to get the AP network interface\n");
    } else {
        // Stop DHCP server before configuring
        esp_netif_dhcps_stop(ap_netif);

        // Configure IP address
        esp_netif_ip_info_t ip_info;
        ip_info.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);        // IP address (192.168.4.1)
        ip_info.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);        // Gateway (usually same as IP)
        ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0); // Subnet mask
        esp_netif_set_ip_info(ap_netif, &ip_info);

        esp_netif_dhcps_start(ap_netif);
        printf("DHCP server configured successfully.\n");
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        printf("esp_wifi_start failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    printf("Wi-Fi Access Point started with SSID: %s\n", ssid);

    // Register event handlers for Wi-Fi events if not registered already
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &event_handler, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Initialize mDNS
    ret = setup_mdns();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup mDNS");
        return ret;
    }

    // Start HTTP server
    ret = load_server_config();
    if (ret != ESP_OK) {
        printf("Error loading server config\n");
        return ret;
    }

    ret = start_http_server();
    if (ret != ESP_OK) {
        printf("Error starting HTTP server\n");
        return ret;
    }

    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
        printf("ESP32 AP IP Address: \n" IPSTR, IP2STR(&ip_info.ip));
    } else {
        printf("Failed to get IP address\n");
    }

    // Initialize log buffer and mutex
    log_buffer = malloc(MAX_LOG_BUFFER_SIZE);
    if(!log_buffer){
        ESP_LOGE(TAG, "failed to alloc log buffer");
        return ESP_ERR_NO_MEM;
    }

    log_mutex = xSemaphoreCreateMutex();
    if (!log_mutex) {
        ESP_LOGE(TAG, "Failed to create log mutex");
        free(log_buffer);
        log_buffer = NULL;
        return ESP_FAIL;
    }

    if(log_buffer){
        memset(log_buffer, 0, MAX_LOG_BUFFER_SIZE);
    }

    return ESP_OK;
}

// Deinitialize and stop the servers
void ap_manager_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing AP Manager");
    
    stop_http_server();
    reset_server_config();
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    if (netif) {
        esp_netif_destroy(netif);
        netif = NULL;
    }
    
    teardown_mdns();
    
    if(log_buffer){
        free(log_buffer);
        log_buffer = NULL;
    }

    if (log_mutex) {
        vSemaphoreDelete(log_mutex);
        log_mutex = NULL;
    }
    
    ESP_LOGI(TAG, "AP Manager deinitialized successfully");
}

void ap_manager_add_log(const char *log_message) {
    if (!log_message || !log_mutex) return;
    
    size_t message_length = strlen(log_message);
    if (message_length == 0) return;
    
    // Take recursive mutex with timeout
    if (xSemaphoreTakeRecursive(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take log mutex");
        return;
    }
    
    // Check if we need to make space
    if (log_buffer_index + message_length >= MAX_LOG_BUFFER_SIZE) {
        // Find the first newline after LOG_CHUNK_SIZE
        size_t remove_index = LOG_CHUNK_SIZE;
        while (remove_index < log_buffer_index && log_buffer[remove_index] != '\n') {
            remove_index++;
        }
        if (remove_index >= log_buffer_index) {
            remove_index = LOG_CHUNK_SIZE; // Fallback if no newline found
        } else {
            remove_index++; // Include the newline
        }
        
        // Move remaining content to start of buffer
        size_t remaining = log_buffer_index - remove_index;
        if (remaining > 0) {
            memmove(log_buffer, log_buffer + remove_index, remaining);
            log_buffer_index = remaining;
        } else {
            log_buffer_index = 0;
        }
    }
    
    // Add new message
    if (log_buffer_index + message_length < MAX_LOG_BUFFER_SIZE) {
        memcpy(log_buffer + log_buffer_index, log_message, message_length);
        log_buffer_index += message_length;
    }
    
    xSemaphoreGiveRecursive(log_mutex);
}

esp_err_t ap_manager_start_services() {
    esp_err_t ret;

    // if ap is disabled or power saving is on, do not start ap services.
    if (!settings_get_ap_enabled(&G_Settings) || settings_get_power_save_enabled(&G_Settings)) {
        printf("ap services skipped: ap disabled or power saving mode is on\n");
        // make sure services are stopped if they somehow started and conditions changed
        ap_manager_stop_services();
        return ESP_OK;
    }

    // Set Wi-Fi mode to AP
    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        printf("WiFi mode set failed\n");
        return ret;
    }

    // Start Wi-Fi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        printf("WiFi start failed\n");
        return ret;
    }

    // Start mDNS
    ret = setup_mdns();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup mDNS");
        return ret;
    }

    // Start HTTPD server
    ret = load_server_config();
    if (ret != ESP_OK) {
        printf("Error loading server config\n");
        return ret;
    }

    ret = start_http_server();
    if (ret != ESP_OK) {
        printf("Error starting HTTP server\n");
        return ret;
    }

    return ESP_OK;
}

void ap_manager_stop_services() {
    wifi_mode_t wifi_mode;
    esp_err_t err = esp_wifi_get_mode(&wifi_mode);

    {
        esp_err_t err_reg = esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister WIFI_EVENT handler: %s", esp_err_to_name(err_reg));
        }
    }
    {
        esp_err_t err_reg = esp_event_handler_unregister(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister IP_EVENT_AP_STAIPASSIGNED handler: %s", esp_err_to_name(err_reg));
        }
    }
    {
        esp_err_t err_reg = esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister IP_EVENT_STA_GOT_IP handler: %s", esp_err_to_name(err_reg));
        }
    }

    if (err == ESP_OK) {
        if (wifi_mode == WIFI_MODE_AP || wifi_mode == WIFI_MODE_STA ||
            wifi_mode == WIFI_MODE_APSTA) {
            printf("Stopping Wi-Fi...\n");
            ESP_ERROR_CHECK(esp_wifi_stop());
        }
    } else {
        printf("Failed to get Wi-Fi mode, error: %d\n", err);
    }

    esp_err_t ret = stop_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    teardown_mdns();
}

// Handler for GET requests (serves the HTML page)
static esp_err_t http_get_handler(httpd_req_t *req) {
    printf("Received HTTP GET request: %s\n", req->uri);

    if (!settings_get_web_auth_enabled(&G_Settings)) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, (const char *)ghost_site_html,
                               ghost_site_html_size);
    }

    char auth_buffer[128];

    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");

    if (auth_len == 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Protected Area\"");
        httpd_resp_sendstr(req, "Authentication required");
        return ESP_OK;
    }


    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buffer, sizeof(auth_buffer)) == ESP_OK) {
        if (strncmp(auth_buffer, "Basic ", 6) == 0) {
            char *credentials = auth_buffer + 6;
            size_t decoded_len;

            size_t input_len = strlen(credentials);
            size_t decoded_max_len = (input_len * 3) / 4 + 1;
            char *decoded = (char *)malloc(decoded_max_len);

            if (decoded) {
                int ret = mbedtls_base64_decode((unsigned char *)decoded, decoded_max_len,
                                                &decoded_len, (const unsigned char *)credentials,
                                                input_len);
                if (ret == 0) {
                    decoded[decoded_len] = '\0';


                    const FSettings *settings = &G_Settings;
                    const char *expected_username = settings_get_ap_ssid(settings);
                    const char *expected_password = settings_get_ap_password(settings);

                    // use "GhostNet" if settings are empty or invalid (should fix the issue reported about not being able to login)
                    if (expected_username == NULL || strlen(expected_username) == 0) {
                        expected_username = "GhostNet";
                    }
                    if (expected_password == NULL || strlen(expected_password) < 8) {
                        expected_password = "GhostNet";
                    }

                    char expected_creds[128];
                    snprintf(expected_creds, sizeof(expected_creds), "%s:%s",
                             expected_username, expected_password);


                    if (strcmp(decoded, expected_creds) == 0) {
                        httpd_resp_set_type(req, "text/html");
                        return httpd_resp_send(req, (const char *)ghost_site_html,
                                               ghost_site_html_size);
                    }
                }
                free(decoded);
            }
        }
    }


    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Protected Area\"");
    httpd_resp_sendstr(req, "Invalid credentials");
    return ESP_OK;
}

static esp_err_t api_command_handler(httpd_req_t *req) {
    char content[500];
    int ret, command_len;

    command_len = MIN_(req->content_len, sizeof(content) - 1);

    ret = httpd_req_recv(req, content, command_len);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    content[command_len] = '\0';

    cJSON *json = cJSON_Parse(content);
    if (json == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid JSON", strlen("Invalid JSON"));
        return ESP_FAIL;
    }

    cJSON *command_json = cJSON_GetObjectItem(json, "command");
    if (command_json == NULL || !cJSON_IsString(command_json)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Missing or invalid 'command' field",
                        strlen("Missing or invalid 'command' field"));
        cJSON_Delete(json); // Cleanup JSON object
        return ESP_FAIL;
    }

    const char *command = command_json->valuestring;

    // Add command to log buffer
    char cmd_log[512];
    snprintf(cmd_log, sizeof(cmd_log), "> %s\n", command);
    ap_manager_add_log(cmd_log);

    simulateCommand(command);

    httpd_resp_send(req, "Command executed", strlen("Command executed"));

    cJSON_Delete(json);
    return ESP_OK;
}

// handler for getting serial logs
static esp_err_t api_logs_handler(httpd_req_t *req) {
    if (!log_mutex) {
        return httpd_resp_send(req, "", 0);
    }
    
    // Take mutex with timeout
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take log mutex for reading");
        return httpd_resp_send(req, "", 0);
    }
    
    // set response type as text/plain
    httpd_resp_set_type(req, "text/plain");
    
    // Check if we have any logs
    if (log_buffer_index == 0) {
        xSemaphoreGive(log_mutex);
        return httpd_resp_sendstr(req, "");
    }
    
    // send the buffer contents
    esp_err_t err = httpd_resp_send(req, log_buffer, log_buffer_index);
    
    xSemaphoreGive(log_mutex);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send logs: %d", err);
        return err;
    }
    
    return ESP_OK;
}

// Handler for /api/clear_logs (clears the log buffer)
static esp_err_t api_clear_logs_handler(httpd_req_t *req) {
    if (!log_mutex) {
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Log system not initialized\"}", -1);
    }
    
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Failed to acquire lock\"}", -1);
    }
    
    log_buffer_index = 0;
    memset(log_buffer, 0, MAX_LOG_BUFFER_SIZE);
    
    xSemaphoreGive(log_mutex);
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"logs_cleared\"}");
}

// Handler for /api/settings (updates settings based on JSON payload)
static esp_err_t api_settings_handler(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;
    char *buf = malloc(total_len + 1);
    if (!buf) {
        printf("Failed to allocate memory for JSON payload\n");
        return ESP_FAIL;
    }

    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            free(buf);
            printf("Failed to receive JSON payload\n");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0'; // Null-terminate the received data

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        printf("Failed to parse JSON\n");
        return ESP_FAIL;
    }

    // Update settings
    FSettings *settings = &G_Settings;

    // Core settings
    cJSON *broadcast_speed = cJSON_GetObjectItem(root, "broadcast_speed");
    if (broadcast_speed) {
        settings_set_broadcast_speed(settings, broadcast_speed->valueint);
    }

    cJSON *ap_ssid = cJSON_GetObjectItem(root, "ap_ssid");
    if (ap_ssid) {
        settings_set_ap_ssid(settings, ap_ssid->valuestring);
    }

    cJSON *ap_password = cJSON_GetObjectItem(root, "ap_password");
    if (ap_password) {
        settings_set_ap_password(settings, ap_password->valuestring);
    }

    cJSON *rgb_mode = cJSON_GetObjectItem(root, "rainbow_mode");
    if (cJSON_IsBool(rgb_mode)) {
        bool rgb_mode_value = cJSON_IsTrue(rgb_mode);
        printf("Debug: Passed rgb_mode_value = %d to settings_set_rgb_mode()\n", rgb_mode_value);
        settings_set_rgb_mode(settings, (RGBMode)rgb_mode_value);
    } else {
        printf("Error: 'rgb_mode' is not a boolean.\n");
    }

    cJSON *rgb_speed = cJSON_GetObjectItem(root, "rgb_speed");
    if (rgb_speed) {
        settings_set_rgb_speed(settings, rgb_speed->valueint);
    }

    cJSON *channel_delay = cJSON_GetObjectItem(root, "channel_delay");
    if (channel_delay) {
        settings_set_channel_delay(settings, (float)channel_delay->valuedouble);
    }

    // Evil Portal settings
    cJSON *portal_url = cJSON_GetObjectItem(root, "portal_url");
    if (portal_url) {
        settings_set_portal_url(settings, portal_url->valuestring);
    }

    cJSON *portal_ssid = cJSON_GetObjectItem(root, "portal_ssid");
    if (portal_ssid) {
        settings_set_portal_ssid(settings, portal_ssid->valuestring);
    }

    cJSON *portal_password = cJSON_GetObjectItem(root, "portal_password");
    if (portal_password) {
        settings_set_portal_password(settings, portal_password->valuestring);
    }

    cJSON *portal_ap_ssid = cJSON_GetObjectItem(root, "portal_ap_ssid");
    if (portal_ap_ssid) {
        settings_set_portal_ap_ssid(settings, portal_ap_ssid->valuestring);
    }

    cJSON *portal_domain = cJSON_GetObjectItem(root, "portal_domain");
    if (portal_domain) {
        settings_set_portal_domain(settings, portal_domain->valuestring);
    }

    cJSON *portal_offline_mode = cJSON_GetObjectItem(root, "portal_offline_mode");
    if (portal_offline_mode) {
        settings_set_portal_offline_mode(settings, portal_offline_mode->valueint != 0);
    }

    // Power Printer settings
    cJSON *printer_ip = cJSON_GetObjectItem(root, "printer_ip");
    if (printer_ip) {
        settings_set_printer_ip(settings, printer_ip->valuestring);
    }

    cJSON *printer_text = cJSON_GetObjectItem(root, "printer_text");
    if (printer_text) {
        settings_set_printer_text(settings, printer_text->valuestring);
    }

    cJSON *printer_font_size = cJSON_GetObjectItem(root, "printer_font_size");
    if (printer_font_size) {
        printf("PRINTER FONT SIZE %i", printer_font_size->valueint);
        settings_set_printer_font_size(settings, printer_font_size->valueint);
    }

    cJSON *printer_alignment = cJSON_GetObjectItem(root, "printer_alignment");
    if (printer_alignment) {
        printf("printer_alignment %i", printer_alignment->valueint);
        settings_set_printer_alignment(settings, (PrinterAlignment)printer_alignment->valueint);
    }

    cJSON *flappy_ghost_name = cJSON_GetObjectItem(root, "flappy_ghost_name");
    if (flappy_ghost_name) {
        settings_set_flappy_ghost_name(settings, flappy_ghost_name->valuestring);
    }

    cJSON *time_zone_str_name = cJSON_GetObjectItem(root, "timezone_str");
    if (time_zone_str_name) {
        settings_set_timezone_str(settings, time_zone_str_name->valuestring);
    }

    cJSON *hex_accent_color_str = cJSON_GetObjectItem(root, "hex_accent_color");
    if (hex_accent_color_str) {
        settings_set_accent_color_str(settings, hex_accent_color_str->valuestring);
    }

    cJSON *rts_enabled_bool = cJSON_GetObjectItem(root, "rts_enabled");
    if (rts_enabled_bool) {
        settings_set_rts_enabled(settings, rts_enabled_bool->valueint != 0);
    }

    cJSON *web_auth_enabled_bool = cJSON_GetObjectItem(root, "web_auth_enabled");
    if (web_auth_enabled_bool) {
        settings_set_web_auth_enabled(settings, web_auth_enabled_bool->valueint != 0);
    }

    cJSON *ap_enabled_bool = cJSON_GetObjectItem(root, "ap_enabled");
    if (ap_enabled_bool) {
        settings_set_ap_enabled(settings, ap_enabled_bool->valueint != 0);
    }

    cJSON *gps_rx_pin = cJSON_GetObjectItem(root, "gps_rx_pin");
    if (gps_rx_pin) {
        settings_set_gps_rx_pin(settings, gps_rx_pin->valueint);
    }

    // Handle display timeout
    cJSON *display_timeout = cJSON_GetObjectItem(root, "display_timeout");
    if (display_timeout) {
        settings_set_display_timeout(settings, display_timeout->valueint);
        ESP_LOGI(TAG, "Setting display timeout to: %d ms", display_timeout->valueint);
    }
    printf("About to Save Settings\n");

    settings_save(settings);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"settings_updated\"}");

    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t api_settings_get_handler(httpd_req_t *req) {
    FSettings *settings = &G_Settings;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        printf("Failed to create JSON object\n");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "broadcast_speed", settings_get_broadcast_speed(settings));
    cJSON_AddStringToObject(root, "ap_ssid", settings_get_ap_ssid(settings));
    cJSON_AddStringToObject(root, "ap_password", settings_get_ap_password(settings));
    cJSON_AddNumberToObject(root, "rgb_mode", settings_get_rgb_mode(settings));
    cJSON_AddNumberToObject(root, "rgb_speed", settings_get_rgb_speed(settings));
    cJSON_AddNumberToObject(root, "channel_delay", settings_get_channel_delay(settings));

    cJSON_AddStringToObject(root, "portal_url", settings_get_portal_url(settings));
    cJSON_AddStringToObject(root, "portal_ssid", settings_get_portal_ssid(settings));
    cJSON_AddStringToObject(root, "portal_password", settings_get_portal_password(settings));
    cJSON_AddStringToObject(root, "portal_ap_ssid", settings_get_portal_ap_ssid(settings));
    cJSON_AddStringToObject(root, "portal_domain", settings_get_portal_domain(settings));
    cJSON_AddBoolToObject(root, "portal_offline_mode", settings_get_portal_offline_mode(settings));

    cJSON_AddStringToObject(root, "printer_ip", settings_get_printer_ip(settings));
    cJSON_AddStringToObject(root, "printer_text", settings_get_printer_text(settings));
    cJSON_AddNumberToObject(root, "printer_font_size", settings_get_printer_font_size(settings));
    cJSON_AddNumberToObject(root, "printer_alignment", settings_get_printer_alignment(settings));
    cJSON_AddStringToObject(root, "hex_accent_color", settings_get_accent_color_str(settings));
    cJSON_AddStringToObject(root, "timezone_str", settings_get_timezone_str(settings));
    cJSON_AddNumberToObject(root, "gps_rx_pin", settings_get_gps_rx_pin(settings));
    cJSON_AddNumberToObject(root, "display_timeout", settings_get_display_timeout(settings));
    cJSON_AddNumberToObject(root, "rts_enabled_bool", settings_get_rts_enabled(settings));
    cJSON_AddBoolToObject(root, "web_auth_enabled", settings_get_web_auth_enabled(settings));
    cJSON_AddBoolToObject(root, "ap_enabled", settings_get_ap_enabled(settings));

    // Add ESP communication pin settings
    int32_t tx_pin, rx_pin;
    settings_get_esp_comm_pins(settings, &tx_pin, &rx_pin);
    cJSON_AddNumberToObject(root, "esp_comm_tx_pin", tx_pin);
    cJSON_AddNumberToObject(root, "esp_comm_rx_pin", rx_pin);

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        if (ip_info.ip.addr != 0) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(root, "station_ip", ip_str);
        }
    }

    const char *json_response = cJSON_Print(root);
    if (!json_response) {
        cJSON_Delete(root);
        printf("Failed to print JSON object\n");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);

    cJSON_Delete(root);
    free((void *)json_response);

    return ESP_OK;
}

// Handler for ESP communication status
static esp_err_t api_esp_comm_status_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to create JSON object\"}");
        return ESP_FAIL;
    }

    comm_state_t state = esp_comm_manager_get_state();
    const char *state_str = "unknown";
    switch (state) {
        case COMM_STATE_IDLE: state_str = "idle"; break;
        case COMM_STATE_SCANNING: state_str = "scanning"; break;
        case COMM_STATE_HANDSHAKE: state_str = "handshake"; break;
        case COMM_STATE_CONNECTED: state_str = "connected"; break;
        case COMM_STATE_ERROR: state_str = "error"; break;
        default: state_str = "unknown"; break;
    }

    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddBoolToObject(root, "connected", esp_comm_manager_is_connected());
    cJSON_AddBoolToObject(root, "is_remote_command", esp_comm_manager_is_remote_command());

    char *response_string = cJSON_Print(root);
    if (!response_string) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to serialize JSON\"}");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_string);

    cJSON_Delete(root);
    free(response_string);
    return ESP_OK;
}

// Handler for ESP communication control (start discovery, connect, disconnect)
static esp_err_t api_esp_comm_control_handler(httpd_req_t *req) {
    char content[512];
    int ret = httpd_req_recv(req, content, MIN_(req->content_len, sizeof(content) - 1));
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid request payload\"}");
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    cJSON *json = cJSON_Parse(content);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON payload\"}");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Missing or invalid action\"}");
        return ESP_FAIL;
    }

    const char *action_str = action->valuestring;
    bool success = false;
    char response_msg[256] = {0};

    if (strcmp(action_str, "start_discovery") == 0) {
        success = esp_comm_manager_start_discovery();
        snprintf(response_msg, sizeof(response_msg), "Discovery %s", success ? "started" : "failed");
    } else if (strcmp(action_str, "connect") == 0) {
        cJSON *peer_name = cJSON_GetObjectItem(json, "peer_name");
        if (peer_name && cJSON_IsString(peer_name)) {
            success = esp_comm_manager_connect_to_peer(peer_name->valuestring);
            snprintf(response_msg, sizeof(response_msg), "Connection to %s %s", 
                     peer_name->valuestring, success ? "initiated" : "failed");
        } else {
            snprintf(response_msg, sizeof(response_msg), "Missing peer name");
        }
    } else if (strcmp(action_str, "disconnect") == 0) {
        esp_comm_manager_disconnect();
        success = true;
        snprintf(response_msg, sizeof(response_msg), "Disconnected");
    } else {
        snprintf(response_msg, sizeof(response_msg), "Unknown action: %s", action_str);
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    cJSON_AddStringToObject(response, "message", response_msg);

    char *response_string = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_string);

    cJSON_Delete(json);
    cJSON_Delete(response);
    free(response_string);
    return ESP_OK;
}

// Handler for sending ESP communication commands
static esp_err_t api_esp_comm_send_handler(httpd_req_t *req) {
    char content[512];
    int ret = httpd_req_recv(req, content, MIN_(req->content_len, sizeof(content) - 1));
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid request payload\"}");
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    cJSON *json = cJSON_Parse(content);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON payload\"}");
        return ESP_FAIL;
    }

    cJSON *command = cJSON_GetObjectItem(json, "command");
    if (!command || !cJSON_IsString(command)) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Missing or invalid command\"}");
        return ESP_FAIL;
    }

    // Send the full command string as-is (no separate data field needed)
    bool success = esp_comm_manager_send_command(command->valuestring, NULL);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    cJSON_AddStringToObject(response, "message", success ? "Command sent successfully" : "Failed to send command");

    char *response_string = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_string);

    cJSON_Delete(json);
    cJSON_Delete(response);
    free(response_string);
    return ESP_OK;
}

// Event handler for Wi-Fi events
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            printf("AP_manager: AP started\n");
            break;
        case WIFI_EVENT_AP_STOP:
            printf("AP_manager: AP stopped\n");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            printf("AP_manager: Device connected to AP\n");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            printf("AP_manager: Device disconnected from AP\n");

            break;
        case WIFI_EVENT_STA_START: {
            // No auto-connect here - handled by wifi_manager's wifi_event_handler
            printf("AP_manager: STA interface started\n");
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *) event_data;
            printf("Disconnected\nReason: %d\n", disconn->reason);
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
            
            // Get SSID from active connection
            wifi_ap_record_t ap_info;
            if(esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                printf("\nConnected!\nSSID: %.*s\nIP: " IPSTR "\n",
                       18, ap_info.ssid, 
                       IP2STR(&event->ip_info.ip));
            } else {
                printf("\nConnected!\nIP: " IPSTR "\n",
                       IP2STR(&event->ip_info.ip));
            }
            break;
        }
        case IP_EVENT_AP_STAIPASSIGNED:
            printf("Assigned STA IP\n");
            break;
        default:
            break;
        }
    }
}

static esp_err_t load_server_config(void) {
    if (config_loaded) {
        ESP_LOGW(TAG, "Server config already loaded");
        return ESP_OK;
    }

    if (server != NULL) {
        ESP_LOGE(TAG, "Cannot load config while server is running");
        return ESP_FAIL;
    }

    server_config = (httpd_config_t)HTTPD_DEFAULT_CONFIG();
    server_config.server_port = 80;
    server_config.ctrl_port = 32768;
    server_config.max_uri_handlers = 60;
    server_config.stack_size = 8192;
    server_config.recv_wait_timeout = 10;
    server_config.send_wait_timeout = 10;

    handler_count = 0;

#define ADD_URI_HANDLER(uri_path, method_type, handler_func) do { \
    if (handler_count >= sizeof(uri_handlers)/sizeof(uri_handlers[0])) { \
        ESP_LOGE(TAG, "Too many URI handlers, cannot add: %s", uri_path); \
        return ESP_FAIL; \
    } \
    uri_handlers[handler_count++] = (httpd_uri_t){ \
        .uri = uri_path, \
        .method = method_type, \
        .handler = handler_func, \
        .user_ctx = NULL \
    }; \
} while(0)

    ADD_URI_HANDLER("/", HTTP_GET, http_get_handler);
    ADD_URI_HANDLER("/api/settings", HTTP_POST, api_settings_handler);
    ADD_URI_HANDLER("/api/settings", HTTP_GET, api_settings_get_handler);
    ADD_URI_HANDLER("/api/sdcard", HTTP_GET, api_sd_card_get_handler);
    ADD_URI_HANDLER("/api/sdcard/download", HTTP_POST, api_sd_card_post_handler);
    ADD_URI_HANDLER("/api/sdcard/upload", HTTP_POST, api_sd_card_upload_handler);
    ADD_URI_HANDLER("/api/sdcard", HTTP_DELETE, api_sd_card_delete_file_handler);
    ADD_URI_HANDLER("/api/command", HTTP_POST, api_command_handler);
    ADD_URI_HANDLER("/api/logs", HTTP_GET, api_logs_handler);
    ADD_URI_HANDLER("/api/clear_logs", HTTP_POST, api_clear_logs_handler);
    ADD_URI_HANDLER("/api/esp_comm/status", HTTP_GET, api_esp_comm_status_handler);
    ADD_URI_HANDLER("/api/esp_comm/control", HTTP_POST, api_esp_comm_control_handler);
    ADD_URI_HANDLER("/api/esp_comm/send", HTTP_POST, api_esp_comm_send_handler);

#undef ADD_URI_HANDLER

    config_loaded = true;
    ESP_LOGI(TAG, "Server configuration loaded successfully with %d handlers", handler_count);
    return ESP_OK;
}

static esp_err_t start_http_server(void) {
    if (!config_loaded) {
        ESP_LOGE(TAG, "Server config not loaded");
        return ESP_FAIL;
    }

    if (server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    esp_err_t ret = httpd_start(&server, &server_config);
    if (ret == ESP_ERR_HTTPD_TASK && server_config.stack_size > 4096) {
        server_config.stack_size = 4096;
        ret = httpd_start(&server, &server_config);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < handler_count; i++) {
        ret = httpd_register_uri_handler(server, &uri_handlers[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI handler %d: %s", i, esp_err_to_name(ret));
            httpd_stop(server);
            server = NULL;
            return ret;
        }
    }

    ESP_LOGI(TAG, "HTTP server started successfully on port %d", server_config.server_port);
    return ESP_OK;
}

static esp_err_t stop_http_server(void) {
    if (server) {
        esp_err_t ret = httpd_stop(server);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(ret));
            return ret;
        }
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ESP_OK;
}

static void reset_server_config(void) {
    config_loaded = false;
    handler_count = 0;
    memset(&server_config, 0, sizeof(server_config));
    memset(uri_handlers, 0, sizeof(uri_handlers));
    ESP_LOGI(TAG, "Server configuration reset");
}

static bool is_server_running(void) {
    return server != NULL;
}

static bool is_config_loaded(void) {
    return config_loaded;
}

esp_err_t ap_manager_reload_config(void) {
    ESP_LOGI(TAG, "Reloading server configuration and mDNS");
    
    esp_err_t ret = stop_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop server before reload: %s", esp_err_to_name(ret));
        return ret;
    }
    
    teardown_mdns();
    reset_server_config();
    
    ret = load_server_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reload config: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = setup_mdns();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup mDNS after reload: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = start_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart server after reload: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Server configuration and mDNS reloaded successfully");
    return ESP_OK;
}

void ap_manager_get_status(bool *server_running, bool *config_loaded_status, int *handler_count_status) {
    if (server_running) *server_running = is_server_running();
    if (config_loaded_status) *config_loaded_status = is_config_loaded();
    if (handler_count_status) *handler_count_status = handler_count;
}

static esp_err_t setup_mdns(void) {
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    mdns_freed = false;

    ret = mdns_hostname_set("ghostesp");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mdns_instance_name_set("GhostESP Web Interface");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(ret));
    }

    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "192.168.4.1");
    mdns_txt_item_t serviceTxtData[] = {{"ip", ip_str}};

    ret = mdns_service_add("GhostESP", "_http", "_tcp", 80, serviceTxtData, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mdns_service_txt_set("_http", "_tcp", serviceTxtData, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_txt_set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char ip_txt[20];
    snprintf(ip_txt, sizeof(ip_txt), "192.168.4.1");
    mdns_txt_item_t ip_data[] = {{"ipv4", ip_txt}};
    ret = mdns_service_txt_set("_http", "_tcp", ip_data, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_txt_set failed: %s", esp_err_to_name(ret));
    }

    ret = mdns_service_add(NULL, "_http", "_http", 80, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "mDNS setup completed successfully");
    return ESP_OK;
}

static esp_err_t teardown_mdns(void) {
    if (!mdns_freed) {
        mdns_free();
        mdns_freed = true;
        ESP_LOGI(TAG, "mDNS teardown completed");
    }
    return ESP_OK;
}
