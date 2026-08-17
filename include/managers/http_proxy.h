#pragma once

#include "esp_http_client.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_PROXY_BASE_URL "https://httpsproxy.fuckyourcdn.com/?url="
#define HTTP_PROXY_URL_MAX 2048

bool proxy_should_use(const char *url);

esp_err_t proxy_build_url(const char *orig_url, char *out, size_t out_len);

esp_err_t proxy_apply(esp_http_client_config_t *cfg, char *url_buf, size_t url_buf_len);

#ifdef __cplusplus
}
#endif
