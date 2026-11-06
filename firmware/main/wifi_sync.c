#include "wifi_sync.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "recorder.h"
#include "storage.h"

#define AP_CHANNEL       6
#define AP_IP            "192.168.4.1"
#define WIFI_IDLE_OFF_S  (10 * 60)
#define SEND_CHUNK       (16 * 1024)
#define MAX_META         512

static const char *TAG = "wifi";

static char s_ssid[24];
static char s_password[12];
static char s_credentials[64];
static httpd_handle_t s_server;
static volatile bool s_on;
static volatile int64_t s_last_activity_us;

// Accepts names like REC_0007.opus / REC_0007.json only: no paths, nothing hidden.
static bool valid_name(const char *name)
{
    if (strncmp(name, "REC_", 4) != 0 || strlen(name) > 40) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_' ||
              *p == '.')) {
            return false;
        }
    }
    return strstr(name, "..") == NULL;
}

// True if `name` belongs to the recording being written right now.
static bool is_current_recording(const char *name)
{
    const char *base = recorder_file_name();
    size_t n = strlen(base);
    return recorder_is_recording() && n > 0 && strncasecmp(name, base, n) == 0 && name[n] == '.';
}

static bool is_audio(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && (strcasecmp(ext, ".opus") == 0 || strcasecmp(ext, ".wav") == 0);
}

static void touch(void)
{
    s_last_activity_us = esp_timer_get_time();
}

// GET /api/files
// {"recording":"REC_0017"|null,"files":[{"name":"REC_0009.opus","size":84892,"meta":{...}|null}, ...]}
static esp_err_t files_get(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "application/json");
    char buf[320];
    snprintf(buf, sizeof(buf), "{\"recording\":%s%s%s,\"files\":[", recorder_is_recording() ? "\"" : "",
             recorder_is_recording() ? recorder_file_name() : "null", recorder_is_recording() ? "\"" : "");
    httpd_resp_sendstr_chunk(req, buf);

    DIR *dir = storage_is_mounted() ? opendir(STORAGE_MOUNT_POINT) : NULL;
    bool first = true;
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL) {
        if (!is_audio(e->d_name) || !valid_name(e->d_name) || is_current_recording(e->d_name)) {
            continue;
        }
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", e->d_name);
        long size = stat(path, &st) == 0 ? (long)st.st_size : 0;

        // Embed REC_NNNN.json as-is (it's small and written by us).
        char meta[MAX_META] = "null";
        char *dot = strrchr(path, '.');
        strcpy(dot, ".json");
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = fread(meta, 1, sizeof(meta) - 1, f);
            fclose(f);
            meta[n] = 0;
            while (n > 0 && (meta[n - 1] == '\n' || meta[n - 1] == '\r')) {
                meta[--n] = 0;
            }
            if (n == 0 || meta[0] != '{') {
                strcpy(meta, "null");
            }
        }
        snprintf(buf, sizeof(buf), "%s{\"name\":\"%s\",\"size\":%ld,\"meta\":", first ? "" : ",", e->d_name, size);
        httpd_resp_sendstr_chunk(req, buf);
        httpd_resp_sendstr_chunk(req, meta);
        httpd_resp_sendstr_chunk(req, "}");
        first = false;
    }
    if (dir) {
        closedir(dir);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static const char *name_from_uri(httpd_req_t *req)
{
    const char *name = req->uri + strlen("/files/");
    return valid_name(name) ? name : NULL;
}

// GET /files/<name>
static esp_err_t file_get(httpd_req_t *req)
{
    touch();
    const char *name = name_from_uri(req);
    if (!name || is_current_recording(name)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such recording");
    }
    char path[64];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such recording");
    }
    const char *ext = strrchr(name, '.');
    httpd_resp_set_type(req, strcasecmp(ext, ".opus") == 0  ? "audio/ogg"
                             : strcasecmp(ext, ".wav") == 0 ? "audio/wav"
                                                            : "application/json");
    uint8_t *chunk = heap_caps_malloc(SEND_CHUNK, MALLOC_CAP_SPIRAM);
    esp_err_t err = chunk ? ESP_OK : ESP_ERR_NO_MEM;
    size_t n;
    while (err == ESP_OK && (n = fread(chunk, 1, SEND_CHUNK, f)) > 0) {
        err = httpd_resp_send_chunk(req, (const char *)chunk, n);
        touch();
    }
    fclose(f);
    free(chunk);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Download of %s aborted", name);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// DELETE /files/<name>
static esp_err_t file_delete(httpd_req_t *req)
{
    touch();
    const char *name = name_from_uri(req);
    esp_err_t err = name ? wifi_sync_delete_recording(name) : ESP_ERR_INVALID_ARG;
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Recording in progress");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such recording");
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static void start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    static const httpd_uri_t uris[] = {
        {.uri = "/api/files", .method = HTTP_GET, .handler = files_get},
        {.uri = "/files/*", .method = HTTP_GET, .handler = file_get},
        {.uri = "/files/*", .method = HTTP_DELETE, .handler = file_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
}

static void idle_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10 * 1000));
        if (s_on && esp_timer_get_time() - s_last_activity_us > (int64_t)WIFI_IDLE_OFF_S * 1000000) {
            ESP_LOGI(TAG, "No requests for %d min, turning Wi-Fi off", WIFI_IDLE_OFF_S / 60);
            wifi_sync_stop();
        }
    }
}

esp_err_t wifi_sync_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);  // same suffix as the BLE name
    snprintf(s_ssid, sizeof(s_ssid), "recordPan-%02X%02X", mac[4], mac[5]);
    // Random per boot; the phone gets it over BLE. No look-alike characters.
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 10; i++) {
        s_password[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    snprintf(s_credentials, sizeof(s_credentials), "%s\n%s\n" AP_IP, s_ssid, s_password);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    xTaskCreate(idle_task, "wifi_idle", 3072, NULL, 2, NULL);
    return ESP_OK;
}

esp_err_t wifi_sync_start(void)
{
    if (s_on) {
        touch();
        return ESP_OK;
    }
    wifi_config_t cfg = {
        .ap = {
            .channel = AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 2,
            .pmf_cfg = {.required = false},
        },
    };
    strlcpy((char *)cfg.ap.ssid, s_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(s_ssid);
    strlcpy((char *)cfg.ap.password, s_password, sizeof(cfg.ap.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    start_server();
    touch();
    s_on = true;
    ESP_LOGI(TAG, "Wi-Fi AP %s up, http://" AP_IP "/api/files", s_ssid);
    return ESP_OK;
}

void wifi_sync_stop(void)
{
    if (!s_on) {
        return;
    }
    s_on = false;
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi off");
}

bool wifi_sync_is_on(void)
{
    return s_on;
}

const char *wifi_sync_credentials(void)
{
    return s_credentials;
}

esp_err_t wifi_sync_delete_recording(const char *name)
{
    if (!valid_name(name) || !storage_is_mounted()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_current_recording(name)) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[64];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", name);
    if (unlink(path) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    char *dot = strrchr(path, '.');
    if (is_audio(name) && dot) {
        strcpy(dot, ".json");
        unlink(path);  // metadata may not exist
    }
    ESP_LOGI(TAG, "Deleted %s", name);
    return ESP_OK;
}
