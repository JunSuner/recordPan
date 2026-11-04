#include "ble_service.h"

#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "buttons.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "recorder.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "storage.h"

#define STATUS_VERSION      1
#define STATUS_INTERVAL_MS  1000
#define AUDIO_MAX_PACKETS   3      // 60 ms of audio per notification
#define AUDIO_BUF_SIZE      244    // fits a 247-byte ATT MTU
#define FILE_LIST_END       0xFFFFFFFFu

enum {
    CMD_START = 0x01,
    CMD_STOP = 0x02,
    CMD_MARK = 0x03,
    CMD_LIST_FILES = 0x04,
};

static const char *TAG = "ble";

// 6a2e00XX-8b1f-4c3e-9d2a-7265636f7264 ("record" in the last 6 bytes), little-endian here.
#define RP_UUID(xx) \
    BLE_UUID128_INIT(0x64, 0x72, 0x6f, 0x63, 0x65, 0x72, 0x2a, 0x9d, 0x3e, 0x4c, 0x1f, 0x8b, xx, 0x00, 0x2e, 0x6a)
static const ble_uuid128_t s_svc_uuid = RP_UUID(0x01);
static const ble_uuid128_t s_control_uuid = RP_UUID(0x02);
static const ble_uuid128_t s_status_uuid = RP_UUID(0x03);
static const ble_uuid128_t s_files_uuid = RP_UUID(0x04);
static const ble_uuid128_t s_audio_uuid = RP_UUID(0x05);

static uint16_t s_status_handle, s_files_handle, s_audio_handle;
static QueueHandle_t s_app_events;
static QueueHandle_t s_worker_cmds;  // commands handled outside the NimBLE host task
static uint8_t s_own_addr_type;

static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_status_sub, s_files_sub, s_audio_sub;

// Audio packets are batched so a phone with a 30 ms connection interval keeps up.
static uint8_t s_audio_buf[AUDIO_BUF_SIZE];
static size_t s_audio_len;
static int s_audio_count;

static void advertise(void);

static size_t build_status(uint8_t *b)
{
    // Little-endian: version, state, marks, elapsed_ms, free_kb, peak_db, reserved
    uint8_t state = !storage_is_mounted() ? 2 : (recorder_is_recording() ? 1 : 0);
    uint16_t marks = recorder_mark_count();
    uint32_t elapsed = recorder_is_recording() ? recorder_elapsed_ms() : 0;
    uint32_t free_kb = (uint32_t)(storage_free_bytes() / 1024);
    b[0] = STATUS_VERSION;
    b[1] = state;
    memcpy(b + 2, &marks, 2);
    memcpy(b + 4, &elapsed, 4);
    memcpy(b + 8, &free_kb, 4);
    b[12] = (uint8_t)(int8_t)recorder_peak_db();
    b[13] = 0;
    return 14;
}

static void notify(uint16_t handle, const void *data, size_t len)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        ble_gatts_notify_custom(conn, handle, om);  // consumes om
    }
}

static size_t max_payload(void)
{
    uint16_t conn = s_conn;
    return conn == BLE_HS_CONN_HANDLE_NONE ? 0 : ble_att_mtu(conn) - 3;
}

// Sends one notification per audio file: u32 size + file name, then a u32 0xFFFFFFFF end marker.
static void send_file_list(void)
{
    DIR *dir = storage_is_mounted() ? opendir(STORAGE_MOUNT_POINT) : NULL;
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            const char *ext = strrchr(e->d_name, '.');
            if (!ext || (strcasecmp(ext, ".opus") != 0 && strcasecmp(ext, ".wav") != 0)) {
                continue;
            }
            char path[300];
            struct stat st;
            snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", e->d_name);
            uint32_t size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
            uint8_t buf[4 + 64];
            size_t name_len = strnlen(e->d_name, sizeof(buf) - 4);
            memcpy(buf, &size, 4);
            memcpy(buf + 4, e->d_name, name_len);
            notify(s_files_handle, buf, 4 + name_len);
            vTaskDelay(pdMS_TO_TICKS(10));  // let the controller drain
        }
        closedir(dir);
    }
    uint32_t end = FILE_LIST_END;
    notify(s_files_handle, &end, 4);
}

static void worker_task(void *arg)
{
    while (true) {
        uint8_t cmd;
        if (xQueueReceive(s_worker_cmds, &cmd, pdMS_TO_TICKS(STATUS_INTERVAL_MS)) == pdTRUE) {
            if (cmd == CMD_LIST_FILES) {
                send_file_list();
                continue;
            }
        }
        // Periodic (or requested) status update.
        if (s_status_sub) {
            uint8_t b[16];
            notify(s_status_handle, b, build_status(b));
        }
    }
}

static void post_button(btn_event_t evt)
{
    xQueueSend(s_app_events, &evt, 0);
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr == s_status_handle) {
        uint8_t b[16];
        return os_mbuf_append(ctxt->om, b, build_status(b)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t cmd;
        uint16_t len;
        if (OS_MBUF_PKTLEN(ctxt->om) < 1 || ble_hs_mbuf_to_flat(ctxt->om, &cmd, 1, &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ESP_LOGI(TAG, "Command 0x%02x", cmd);
        switch (cmd) {
        case CMD_START:
            if (!recorder_is_recording()) {
                post_button(BTN_EVT_REC);
            }
            return 0;
        case CMD_STOP:
            if (recorder_is_recording()) {
                post_button(BTN_EVT_REC);
            }
            return 0;
        case CMD_MARK:
            post_button(BTN_EVT_MARK);
            return 0;
        case CMD_LIST_FILES:
            xQueueSend(s_worker_cmds, &cmd, 0);
            return 0;
        default:
            return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_control_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            {
                .uuid = &s_files_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_files_handle,
            },
            {
                .uuid = &s_audio_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_audio_handle,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_status_sub = s_files_sub = s_audio_sub = false;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_handle) {
            s_status_sub = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_files_handle) {
            s_files_sub = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_audio_handle) {
            s_audio_sub = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Audio stream %s", s_audio_sub ? "on" : "off");
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        break;
    }
    return 0;
}

static void advertise(void)
{
    // The 128-bit service UUID fills most of the 31-byte advertisement, so the
    // name goes in the scan response.
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_hs_adv_fields rsp = {0};
    const char *name = ble_svc_gap_device_name();
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Advertising failed: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
    ESP_LOGI(TAG, "Advertising as %s", ble_svc_gap_device_name());
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_service_init(QueueHandle_t app_events)
{
    s_app_events = app_events;
    s_worker_cmds = xQueueCreate(4, sizeof(uint8_t));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;

    // Name it recordPan-XXXX after the last MAC bytes, so several boards can be told apart.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char name[24];
    snprintf(name, sizeof(name), "recordPan-%02X%02X", mac[4], mac[5]);

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(name);
    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }

    xTaskCreate(worker_task, "ble_worker", 4096, NULL, 4, NULL);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void ble_service_status_changed(void)
{
    if (s_worker_cmds) {
        uint8_t cmd = 0;  // anything but LIST_FILES just sends status
        xQueueSend(s_worker_cmds, &cmd, 0);
    }
}

// Notification layout: u16 frame index of the first packet, u8 packet count,
// then for each packet u8 length + Opus data.
void ble_service_audio_packet(const uint8_t *packet, size_t len, uint32_t frame)
{
    if (frame == 0) {
        s_audio_len = 0;  // drop leftovers of the previous recording
    }
    if (!s_audio_sub || len > 255) {
        s_audio_len = 0;
        return;
    }
    size_t limit = max_payload();
    if (limit > AUDIO_BUF_SIZE) {
        limit = AUDIO_BUF_SIZE;
    }
    if (s_audio_len > 0 && s_audio_len + 1 + len > limit) {
        notify(s_audio_handle, s_audio_buf, s_audio_len);
        s_audio_len = 0;
    }
    if (s_audio_len == 0) {
        if (3 + 1 + len > limit) {
            return;  // MTU too small for even one packet
        }
        uint16_t seq = (uint16_t)frame;
        memcpy(s_audio_buf, &seq, 2);
        s_audio_buf[2] = 0;
        s_audio_len = 3;
        s_audio_count = 0;
    }
    s_audio_buf[s_audio_len++] = (uint8_t)len;
    memcpy(s_audio_buf + s_audio_len, packet, len);
    s_audio_len += len;
    s_audio_buf[2] = ++s_audio_count;
    if (s_audio_count >= AUDIO_MAX_PACKETS) {
        notify(s_audio_handle, s_audio_buf, s_audio_len);
        s_audio_len = 0;
    }
}
