#include "managers/http_proxy.h"

#include <string.h>
#include <stdio.h>

bool proxy_should_use(const char *url) {
    return url && strncmp(url, "https://", 8) == 0;
}

static bool is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static bool proxy_can_use_plain_http(const char *url) {
    return strncmp(url, "https://gesp.fuckyourcdn.com/", 29) == 0 ||
           strncmp(url, "https://gespota.fuckyourcdn.com/", 32) == 0;
}

static esp_err_t build_plain_http_url(const char *orig_url, char *out, size_t out_len) {
    if (!orig_url || !out || out_len == 0) return ESP_ERR_INVALID_ARG;
    if (strlen(orig_url) >= out_len) return ESP_ERR_INVALID_SIZE;
    memcpy(out, "http://", 7);
    strcpy(out + 7, orig_url + 8);
    return ESP_OK;
}

esp_err_t proxy_build_url(const char *orig_url, char *out, size_t out_len) {
    if (!orig_url || !out || out_len == 0) return ESP_ERR_INVALID_ARG;

    size_t prefix_len = strlen(HTTP_PROXY_BASE_URL);
    if (out_len <= prefix_len) return ESP_ERR_INVALID_SIZE;

    memcpy(out, HTTP_PROXY_BASE_URL, prefix_len);
    size_t pos = prefix_len;

    for (size_t i = 0; orig_url[i] != '\0'; i++) {
        char c = orig_url[i];
        if (is_unreserved(c)) {
            if (pos + 1 >= out_len) return ESP_ERR_INVALID_SIZE;
            out[pos++] = c;
        } else {
            if (pos + 3 >= out_len) return ESP_ERR_INVALID_SIZE;
            static const char hex[] = "0123456789ABCDEF";
            out[pos++] = '%';
            out[pos++] = hex[(unsigned char)c >> 4];
            out[pos++] = hex[(unsigned char)c & 0x0F];
        }
    }

    if (pos >= out_len) return ESP_ERR_INVALID_SIZE;
    out[pos] = '\0';
    return ESP_OK;
}

esp_err_t proxy_apply(esp_http_client_config_t *cfg, char *url_buf, size_t url_buf_len) {
    if (!cfg || !cfg->url || !url_buf) return ESP_ERR_INVALID_ARG;

    if (!proxy_should_use(cfg->url)) return ESP_OK;

    esp_err_t err = proxy_can_use_plain_http(cfg->url)
        ? build_plain_http_url(cfg->url, url_buf, url_buf_len)
        : proxy_build_url(cfg->url, url_buf, url_buf_len);
    if (err != ESP_OK) return err;

    cfg->url = url_buf;
    cfg->cert_pem = NULL;
    cfg->crt_bundle_attach = NULL;
    cfg->transport_type = 0;
    return ESP_OK;
}
