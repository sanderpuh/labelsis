/*
 * pt_app -- Wi-Fi + HTTP + QL-700 transport glue. Owns:
 *   - NVS-persisted Wi-Fi creds with first-boot fallback to compiled-in
 *   - SoftAP onboarding ("labelsis-setup" + setup page) when no creds
 *   - STA bring-up + transport open + mDNS + the HTTP API
 *   - long-press GPIO that wipes creds and re-enters AP mode
 */

#include "pt_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_crc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pt_dns.h"
#include "pt_led.h"
#include "ql700_status.h"
#include "ql700_raster.h"
#include "pt_transport.h"
#include "pt_transport_usb_host.h"

static const char *TAG = "pt_app";

/* Wi-Fi creds (NVS) -- ordered list of APs.
 *
 * Persisted as a single blob under "wifi_v1" so add / delete / reorder
 * are atomic NVS writes. Index 0 is highest priority; on boot we try
 * last_used first (if it's still in the list), then walk the list in
 * order. This is a portable device, so different sites (home, office,
 * workshop) want to coexist. AP-mode onboarding only kicks in when
 * EVERY entry fails.
 *
 * The legacy single-cred schema (CREDS_NS / CREDS_SSID / CREDS_PASS,
 * the "pt_app/ssid" + "pt_app/pass" keys) is migrated into a
 * single-entry list on first boot of new firmware -- see credlist_load.
 *
 * Buffer sizes per IEEE 802.11 SSID (32 bytes + NUL) and the WPA2 PSK
 * string form (64 chars + NUL). Cap at MAX_APS so the blob is fixed
 * size; 4 covers the typical portable use (home, office, hotspot,
 * workshop) without bloating NVS. */
#define CREDS_NS         "pt_app"
#define CREDS_LEGACY_SSID "ssid"
#define CREDS_LEGACY_PASS "pass"
#define CREDS_BLOB_KEY    "wifi_v1"
#define PRESCAN_KEY       "wifi_prescan"   /* uint8 0/1; default 0 */
#define MAX_APS           4

typedef struct {
    char ssid[33];
    char password[65];
} wifi_ap_t;

typedef struct {
    uint8_t   version;             /* = 1 */
    uint8_t   count;               /* 0..MAX_APS */
    uint8_t   reserved[2];
    char      last_used[33];       /* SSID of last successful connect; "" = none */
    wifi_ap_t aps[MAX_APS];        /* aps[0] = highest priority */
} wifi_creds_blob_t;

/* Migrate legacy single-cred (pt_app/ssid + pt_app/pass) into a
 * single-entry list. Called from credlist_load when the new blob is
 * absent but the legacy keys are present. Erases the legacy keys
 * after a successful migration so the next boot is clean. */
static void credlist_migrate_legacy(wifi_creds_blob_t *out)
{
    nvs_handle_t h;
    if (nvs_open(CREDS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    char ssid[33] = {0}, pass[65] = {0};
    size_t sz = sizeof ssid;
    if (nvs_get_str(h, CREDS_LEGACY_SSID, ssid, &sz) == ESP_OK && ssid[0]) {
        sz = sizeof pass;
        nvs_get_str(h, CREDS_LEGACY_PASS, pass, &sz);  /* may be empty for open APs */

        memset(out, 0, sizeof *out);
        out->version = 1;
        out->count   = 1;
        strncpy(out->aps[0].ssid,     ssid, sizeof out->aps[0].ssid     - 1);
        strncpy(out->aps[0].password, pass, sizeof out->aps[0].password - 1);
        strncpy(out->last_used,       ssid, sizeof out->last_used       - 1);

        nvs_set_blob(h, CREDS_BLOB_KEY, out, sizeof *out);
        nvs_erase_key(h, CREDS_LEGACY_SSID);
        nvs_erase_key(h, CREDS_LEGACY_PASS);
        nvs_commit(h);
        ESP_LOGI(TAG, "wifi: migrated legacy creds (ssid=%s) into list", ssid);
    }
    nvs_close(h);
}

static esp_err_t credlist_load(wifi_creds_blob_t *out)
{
    memset(out, 0, sizeof *out);
    nvs_handle_t h;
    if (nvs_open(CREDS_NS, NVS_READONLY, &h) != ESP_OK) {
        /* No NVS namespace yet -- maybe legacy keys exist via a
         * read-write open. Try migrate path. */
        credlist_migrate_legacy(out);
        return out->count ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    size_t sz = sizeof *out;
    esp_err_t err = nvs_get_blob(h, CREDS_BLOB_KEY, out, &sz);
    nvs_close(h);
    if (err == ESP_OK && out->version == 1 && out->count <= MAX_APS) {
        return ESP_OK;
    }
    /* Either the blob is absent, or its shape doesn't match -- in
     * either case fall back to the legacy migration path. Wipe out
     * first so a corrupt-but-right-sized blob (or a downgrade from a
     * larger MAX_APS) can't leak its bytes into the fallback. */
    memset(out, 0, sizeof *out);
    credlist_migrate_legacy(out);
    return out->count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t credlist_save(const wifi_creds_blob_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CREDS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, CREDS_BLOB_KEY, in, sizeof *in);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t credlist_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CREDS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, CREDS_BLOB_KEY);
    /* Also wipe the legacy keys in case a long-running deployment
     * never went through the migration. */
    nvs_erase_key(h, CREDS_LEGACY_SSID);
    nvs_erase_key(h, CREDS_LEGACY_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Look up by SSID; returns -1 if not in the list. */
static int credlist_find(const wifi_creds_blob_t *cl, const char *ssid)
{
    for (int i = 0; i < cl->count; i++) {
        if (strcmp(cl->aps[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

/* Add a new AP, or replace the password of an existing one. New
 * entries land at the end (lowest priority); the user can reorder
 * via /api/wifi/aps/order. Returns ESP_ERR_NO_MEM when the list is
 * already full. */
static esp_err_t credlist_add(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    wifi_creds_blob_t cl;
    if (credlist_load(&cl) != ESP_OK) {
        memset(&cl, 0, sizeof cl);
        cl.version = 1;
    }
    int idx = credlist_find(&cl, ssid);
    if (idx >= 0) {
        strncpy(cl.aps[idx].password, password ? password : "",
                sizeof cl.aps[idx].password - 1);
        cl.aps[idx].password[sizeof cl.aps[idx].password - 1] = '\0';
    } else {
        if (cl.count >= MAX_APS) return ESP_ERR_NO_MEM;
        idx = cl.count++;
        memset(&cl.aps[idx], 0, sizeof cl.aps[idx]);
        strncpy(cl.aps[idx].ssid, ssid, sizeof cl.aps[idx].ssid - 1);
        strncpy(cl.aps[idx].password, password ? password : "",
                sizeof cl.aps[idx].password - 1);
    }
    return credlist_save(&cl);
}

/* Idempotent: deleting an SSID that isn't in the list returns OK. */
static esp_err_t credlist_delete(const char *ssid)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    wifi_creds_blob_t cl;
    if (credlist_load(&cl) != ESP_OK) return ESP_OK;
    int idx = credlist_find(&cl, ssid);
    if (idx < 0) return ESP_OK;
    /* Shift remaining entries down. */
    for (int i = idx; i < cl.count - 1; i++) cl.aps[i] = cl.aps[i + 1];
    memset(&cl.aps[cl.count - 1], 0, sizeof cl.aps[0]);
    cl.count--;
    if (strcmp(cl.last_used, ssid) == 0) cl.last_used[0] = '\0';
    return credlist_save(&cl);
}

/* Reorder by SSIDs in `order` (newline-separated, parsed by caller).
 * SSIDs in `order` move to the head of the list, in the given order;
 * remaining entries (those not named) keep their relative order at
 * the tail. Unknown SSIDs in `order` are ignored. */
static esp_err_t credlist_reorder(const char *order_buf, size_t len)
{
    wifi_creds_blob_t cl;
    if (credlist_load(&cl) != ESP_OK) return ESP_ERR_NOT_FOUND;

    wifi_ap_t reordered[MAX_APS] = {0};
    bool      consumed[MAX_APS]  = {0};
    int       n = 0;

    /* First pass: walk the order list, picking up matching APs. */
    const char *p   = order_buf;
    const char *end = order_buf + len;
    while (p < end && n < cl.count) {
        const char *line_end = memchr(p, '\n', end - p);
        if (!line_end) line_end = end;
        size_t llen = line_end - p;
        if (llen > 0 && p[llen - 1] == '\r') llen--;   /* tolerate CRLF */
        if (llen > 0 && llen < sizeof reordered[0].ssid) {
            char ssid[33] = {0};
            memcpy(ssid, p, llen);
            int idx = credlist_find(&cl, ssid);
            if (idx >= 0 && !consumed[idx]) {
                reordered[n++]  = cl.aps[idx];
                consumed[idx]   = true;
            }
        }
        p = (line_end < end) ? line_end + 1 : end;
    }
    /* Second pass: append any APs the caller didn't name. */
    for (int i = 0; i < cl.count; i++) {
        if (!consumed[i]) reordered[n++] = cl.aps[i];
    }
    memcpy(cl.aps, reordered, sizeof cl.aps);
    return credlist_save(&cl);
}

static esp_err_t credlist_set_last_used(const char *ssid)
{
    wifi_creds_blob_t cl;
    if (credlist_load(&cl) != ESP_OK) return ESP_ERR_NOT_FOUND;
    if (strcmp(cl.last_used, ssid) == 0) return ESP_OK;  /* no-op */
    memset(cl.last_used, 0, sizeof cl.last_used);
    strncpy(cl.last_used, ssid, sizeof cl.last_used - 1);
    return credlist_save(&cl);
}

/* Pre-scan toggle. When enabled, the boot loop scans the band first
 * and only attempts APs whose SSIDs are visible in the scan -- this
 * avoids paying the 15 s connect timeout for each unreachable entry
 * when the user has moved to a new site. Default off because hidden
 * APs (no probe response) wouldn't show up and would be skipped. */
static bool prescan_load_enabled(void)
{
    nvs_handle_t h;
    if (nvs_open(CREDS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, PRESCAN_KEY, &v);
    nvs_close(h);
    return v != 0;
}

static esp_err_t prescan_save_enabled(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CREDS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, PRESCAN_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* BOOT button -- two gestures share one input. The release moment is
 * what disambiguates: if the user releases between OTA_HOLD_MS and
 * RESET_HOLD_MS, we toggle the OTA gate (lets a dev board test OTA
 * without a printer attached). If they keep holding past RESET_HOLD_MS
 * we wipe Wi-Fi creds and reboot (unchanged behaviour for the long
 * hold). Releases under OTA_HOLD_MS are ignored to swallow accidental
 * taps -- BOOT is also the bootloader-mode pin on power-on, so people
 * tend to press it. */

#define OTA_HOLD_MS    2000    /* min hold-then-release to toggle OTA gate */
#define RESET_HOLD_MS  30000   /* sustained hold past this = wipe creds.
                                * Deliberately long: wipe is destructive
                                * and brushing against the BOOT button
                                * for a few seconds shouldn't trip it. */
#define RESET_POLL_MS  50      /* GPIO sample period */

/* True when the BOOT button has flipped the OTA gate open. Mirrors
 * the role of "printer in Editor Lite" mode: while either is true, the
 * /api/ota endpoint accepts uploads. Volatile because the button
 * task writes it and the HTTP handler thread reads it; bool is
 * word-sized atomic on Xtensa, so no further locking needed. */
static volatile bool s_ota_button_gate = false;

/* Goes true once Wi-Fi STA has an IP and the HTTP server is serving.
 * The button task gates the OTA-toggle gesture on this so a tap
 * during boot (e.g. pulling the device from a box, fingers near the
 * BOOT button before it powers up) can't pre-arm the OTA gate before
 * the device is reachable. The wipe gesture (30 s+) ignores it
 * intentionally -- if the device is bricked and never comes online,
 * holding for 30 s must still recover. */
static volatile bool s_device_online = false;

static bool ota_button_gate_active(void) { return s_ota_button_gate; }

static void reset_button_task(void *arg)
{
    int gpio = (int)(intptr_t)arg;
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&gc) != ESP_OK) {
        ESP_LOGW(TAG, "reset: gpio %d config failed -- disabling", gpio);
        vTaskDelete(NULL);
        return;   /* defensive: vTaskDelete(NULL) doesn't return to this
                   * task in practice, but if it ever did, gpio_get_level
                   * on an unconfigured pin reads garbage and could
                   * spuriously trigger the 30 s wipe. */
    }

    TickType_t pressed_at      = 0;
    bool       wipe_arm_logged = false;
    while (1) {
        bool       down = (gpio_get_level(gpio) == 0);
        TickType_t now  = xTaskGetTickCount();
        if (down && pressed_at == 0) {
            pressed_at      = now;
            wipe_arm_logged = false;
        } else if (down) {
            TickType_t held = now - pressed_at;
            if (held >= pdMS_TO_TICKS(RESET_HOLD_MS)) {
                ESP_LOGW(TAG, "reset: long press on gpio %d -- clearing creds", gpio);
                credlist_clear();
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            } else if (held >= pdMS_TO_TICKS(OTA_HOLD_MS) && !wipe_arm_logged) {
                ESP_LOGI(TAG, "button: held %lu ms -- release any time before %d s "
                              "to toggle OTA gate%s; keep holding to %d s to wipe creds",
                         (unsigned long)pdTICKS_TO_MS(held),
                         RESET_HOLD_MS / 1000,
                         s_device_online ? "" : " (waiting for device to come online)",
                         RESET_HOLD_MS / 1000);
                wipe_arm_logged = true;
            }
        } else if (!down && pressed_at != 0) {
            TickType_t held = now - pressed_at;
            pressed_at = 0;
            if (held >= pdMS_TO_TICKS(OTA_HOLD_MS)) {
                if (!s_device_online) {
                    ESP_LOGW(TAG, "button: ignoring OTA toggle -- device not online yet");
                } else {
                    s_ota_button_gate = !s_ota_button_gate;
                    ESP_LOGW(TAG, "button: OTA gate %s",
                             s_ota_button_gate ? "OPEN -- /api/ota now accepts uploads"
                                               : "closed");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RESET_POLL_MS));
    }
}

static void reset_button_up(int gpio)
{
    if (gpio < 0) {
        ESP_LOGI(TAG, "reset: disabled (gpio < 0)");
        return;
    }
    BaseType_t r = xTaskCreate(reset_button_task, "rstbtn", 2048,
                               (void *)(intptr_t)gpio,
                               tskIDLE_PRIORITY + 1, NULL);
    if (r != pdPASS) ESP_LOGW(TAG, "reset: task create failed");
    else             ESP_LOGI(TAG, "reset: gpio %d, OTA-toggle %d s, wipe-creds %d s",
                              gpio, OTA_HOLD_MS / 1000, RESET_HOLD_MS / 1000);
}

/* Wi-Fi */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   3
/* Per-AP wait. With multi-AP, this is the budget we burn per
 * unreachable entry, so keep it tight; a healthy WPA2 association
 * completes in <5 s. Worst case 4 unreachable APs = 60 s before the
 * captive-portal AP comes up. Pre-scan (opt-in) skips this entirely
 * for SSIDs that aren't on air. */
#define WIFI_STA_WAIT_MS   15000

static EventGroupHandle_t s_wifi_event_group;
static int                s_retry_count;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* APSTA onboarding mode brings up the radio without an STA
         * SSID configured -- skip auto-connect when there's nothing
         * to connect to. */
        wifi_config_t cur;
        if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK
            && cur.sta.ssid[0] != 0) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "wifi reconnect %d", s_retry_count);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "ap: client joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "ap: client left");
    }
}

/* One-shot Wi-Fi stack init: netif, event loop, default STA+AP netifs,
 * wifi_init, our shared event handler. Both wifi_sta_up and wifi_ap_up
 * call this; subsequent calls no-op so APSTA mode (used by the AP-mode
 * setup page's network scanner) doesn't double-init anything. */
static esp_err_t wifi_init_once(void)
{
    static bool inited = false;
    if (inited) return ESP_OK;

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    inited = true;
    return ESP_OK;
}

/* Pre-scan: spin up the radio in STA mode, run a blocking active scan,
 * mark which entries of `cl` were heard on air. Caller is responsible
 * for the subsequent connect attempt (which restarts the radio with
 * a fresh STA config). Returns ESP_OK with `visible[]` populated on
 * success, or ESP_FAIL if the scan itself errored -- callers should
 * fall back to "try every AP" in that case. */
static esp_err_t prescan_filter(const wifi_creds_blob_t *cl,
                                bool *visible_out)
{
    memset(visible_out, 0, sizeof(bool) * MAX_APS);
    if (cl->count == 0) return ESP_OK;
    if (wifi_init_once() != ESP_OK) return ESP_FAIL;

    /* Make sure the radio is started in pure STA mode. esp_wifi_start
     * after a stop is a no-op-ish init; safe to call. */
    esp_wifi_stop();
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return ESP_FAIL;
    if (esp_wifi_start() != ESP_OK)                 return ESP_FAIL;

    wifi_scan_config_t scfg = {0};   /* all channels, all SSIDs, active */
    esp_err_t err = esp_wifi_scan_start(&scfg, true /* blocking */);
    if (err != ESP_OK) return ESP_FAIL;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return ESP_OK;       /* nothing on air -- visible[] all false */

    wifi_ap_record_t *recs = calloc(n, sizeof *recs);
    if (!recs) return ESP_ERR_NO_MEM;
    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) { free(recs); return ESP_FAIL; }

    for (int i = 0; i < cl->count; i++) {
        for (uint16_t j = 0; j < n; j++) {
            if (strcmp(cl->aps[i].ssid, (const char *)recs[j].ssid) == 0) {
                visible_out[i] = true;
                break;
            }
        }
    }
    free(recs);
    return ESP_OK;
}

/* Bring up STA against a single SSID. Re-entrant: when called a second
 * time (because the previous AP failed and we're trying the next in
 * the priority list) it stops the radio first, reconfigures, and
 * starts cleanly. The connect/fail event bits are reset so a stale
 * FAIL_BIT from the previous attempt doesn't short-circuit the next
 * wait. */
static esp_err_t wifi_sta_up(const char *ssid, const char *password)
{
    if (wifi_init_once() != ESP_OK) return ESP_FAIL;

    /* Tear down any previous STA attempt cleanly. ESP_OK if not
     * started, ESP_ERR_WIFI_NOT_STARTED otherwise -- both fine. */
    esp_wifi_disconnect();
    esp_wifi_stop();

    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t sta = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)sta.sta.ssid,     ssid,     sizeof sta.sta.ssid - 1);
    strncpy((char *)sta.sta.password, password, sizeof sta.sta.password - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_STA_WAIT_MS));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* Bring up an open SoftAP for first-boot onboarding on 192.168.4.1
 * (esp-netif default). APSTA mode lets the setup page scan nearby
 * networks without tearing the AP down. */
static esp_err_t wifi_ap_up(const char *ssid)
{
    if (wifi_init_once() != ESP_OK) return ESP_FAIL;

    /* If wifi_sta_up was called and failed, the radio is already
     * started in STA mode -- stop before switching modes. */
    esp_wifi_stop();

    wifi_config_t ap = {
        .ap = {
            .ssid_len       = (uint8_t)strlen(ssid),
            .channel        = 6,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap.ap.ssid, ssid, sizeof ap.ap.ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

/* transport */

static pt_transport_t           s_transport;
static pt_transport_usb_host_t *s_usb;
static const char              *s_transport_state = "waiting";
static SemaphoreHandle_t        s_printer_mutex;

#define PRINTER_LOCK(timeout_ms) \
    (xSemaphoreTake(s_printer_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
#define PRINTER_UNLOCK() \
    xSemaphoreGive(s_printer_mutex)

/* Forward declaration: the status endpoint exposes whether OTA is
 * currently unlocked (Editor Lite mode or the BOOT-button override). */
static bool ota_gate_open(void);

static bool transport_ready(void)
{
    return s_usb && s_transport.send && s_transport.recv &&
           pt_transport_usb_host_alive(s_usb);
}

/* Try to pair with the QL-700. Editor Lite mode is reported separately
 * so the UI can explain why the printer interface is not available. */
static bool transport_try_usb(uint32_t connect_timeout_ms)
{
    pt_transport_usb_host_t *u = pt_transport_usb_host_open(connect_timeout_ms);
    if (u) {
        pt_transport_usb_host_t *prev = s_usb;
        s_usb = u;
        s_transport = pt_transport_usb_host_transport(s_usb);
        s_transport_state = "usb_host";
        if (prev) pt_transport_usb_host_close(prev);
        ESP_LOGI(TAG, "transport: QL-700 attached over USB");
        pt_led_set(PT_LED_READY);
        return true;
    }

    memset(&s_transport, 0, sizeof s_transport);
    s_transport_state = pt_transport_usb_host_editor_lite_seen()
        ? "editor_lite" : "waiting";
    return false;
}

#define TRANSPORT_POLL_MS 2000

static void transport_monitor_task(void *arg)
{
    (void)arg;
    bool last_ready = transport_ready();
    pt_led_set(last_ready ? PT_LED_READY : PT_LED_USB_WAITING);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TRANSPORT_POLL_MS));
        bool ready = transport_ready();
        if (ready != last_ready) {
            if (ready) {
                ESP_LOGI(TAG, "transport: QL-700 online");
                pt_led_set(PT_LED_READY);
            } else {
                ESP_LOGW(TAG, "transport: QL-700 disconnected");
                s_transport_state = "waiting";
                pt_led_set(PT_LED_USB_WAITING);
            }
            last_ready = ready;
        }
        if (!ready) transport_try_usb(1500);
    }
}

static void transport_up(const pt_app_config_t *cfg)
{
    uint32_t timeout = cfg->usb_connect_timeout_ms
        ? cfg->usb_connect_timeout_ms : 5000;
    if (!transport_try_usb(timeout)) {
        ESP_LOGW(TAG, "transport: QL-700 not found -- waiting for USB attach");
    }

    BaseType_t result = xTaskCreate(transport_monitor_task, "tx_mon", 4096,
                                    NULL, tskIDLE_PRIORITY + 1, NULL);
    if (result != pdPASS)
        ESP_LOGW(TAG, "transport: monitor task creation failed");
}

/* HTTP API */

/* Permissive CORS so the SPA can be developed and tested from any
 * origin (e.g. file://, localhost dev server) hitting a real device
 * over the LAN. Same security posture as before: the API has no auth,
 * anyone reachable on the LAN could already drive it from a same-
 * origin tool -- CORS just lets browser pages do it too. */
static void cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age",       "3600");
}

/* OPTIONS catch-all -- answers CORS preflight for any path with the
 * shared CORS headers. Browsers fire this before any non-simple
 * request (Content-Type: application/json, custom X-* headers, etc.). */
static esp_err_t cors_preflight(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* A read-only QL-700 probe. ESC i S asks for status; it does not move media. */
static bool ql700_query_status(ql700_status_t *out)
{
    static const uint8_t request[] = {0x1b, 0x69, 0x53};
    uint8_t frame[32];
    size_t collected = 0;
    if (pt_transport_send(&s_transport, request, sizeof request) !=
        (int)sizeof request) return false;

    /* Allow fragmented USB frames and short timeouts. Bounded polling. */
    for (int i = 0; i < 5 && collected < sizeof frame; i++) {
        size_t received = 0;
        if (pt_transport_recv(&s_transport, frame + collected,
                              sizeof frame - collected, &received, 300) != 0)
            return false;
        if (received > sizeof frame - collected) return false;
        collected += received;
    }
    return ql700_status_decode(frame, collected, out);
}

static esp_err_t ql700_api_status(httpd_req_t *req, const char *ota_av)
{
    if (!transport_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        char body[192];
        int length = snprintf(body, sizeof body,
            "{\"ok\":false,\"printer\":\"QL-700\",\"transport\":\"%s\",\"ota_available\":%s,\"error\":\"no_printer\"}",
            s_transport_state, ota_av);
        return httpd_resp_send(req, body, length);
    }
    if (!PRINTER_LOCK(500)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        char body[160];
        int length = snprintf(body, sizeof body,
            "{\"ok\":false,\"printer\":\"QL-700\",\"transport\":\"busy\",\"ota_available\":%s,\"error\":\"busy\"}",
            ota_av);
        return httpd_resp_send(req, body, length);
    }
    ql700_status_t st = {0};
    bool success = ql700_query_status(&st);
    PRINTER_UNLOCK();
    if (!success) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        char body[192];
        int length = snprintf(body, sizeof body,
            "{\"ok\":false,\"printer\":\"QL-700\",\"transport\":\"usb_host\",\"usb_detected\":true,\"ota_available\":%s,\"error\":\"ql700_status_timeout_or_invalid\"}",
            ota_av);
        return httpd_resp_send(req, body, length);
    }
    const char *primary_error = ql700_primary_error(&st);
    const bool supported_media = st.media_type == 0x0a && st.media_width_mm == 62 && st.media_length_mm == 0;
    const bool ready = st.error1 == 0 && st.error2 == 0 &&
                       st.media_type != 0 && st.phase_type == 0;
    char body[512];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"transport\":\"usb_host\",\"printer\":\"QL-700\","
        "\"usb_detected\":true,\"print_supported\":%s,"
        "\"ota_available\":%s,\"ready\":%s,"
        "\"media_width_mm\":%u,\"media_length_mm\":%u,"
        "\"media_type\":%u,\"media_kind\":\"%s\","
        "\"error1\":%u,\"error2\":%u,\"error\":\"%s\","
        "\"status_type\":%u,\"phase_type\":%u,"
        "\"phase_number\":%u,\"notification\":%u}",
        supported_media ? "true" : "false", ota_av, ready ? "true" : "false",
        (unsigned)st.media_width_mm, (unsigned)st.media_length_mm,
        (unsigned)st.media_type, ql700_media_kind(st.media_type),
        (unsigned)st.error1, (unsigned)st.error2, primary_error,
        (unsigned)st.status_type, (unsigned)st.phase_type,
        (unsigned)st.phase_number, (unsigned)st.notification);
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"overflow\"}");
    }
    return httpd_resp_send(req, body, n);
}

/* Transport callback used by the QL-700 raster encoder. */
static bool ql700_send_to_usb(void *ctx, const uint8_t *data, size_t size)
{
    const pt_transport_t *transport = (const pt_transport_t *)ctx;
    return pt_transport_send(transport, data, size) == (int)size;
}

/* Allocate large shipping-label raster buffers from PSRAM first. The
 * ESP32-S3 N16R8 target has 8 MB octal PSRAM; keeping the full HTTP body
 * there avoids consuming scarce internal RAM while the printer job is sent.
 * Internal 8-bit RAM is a fallback for smaller jobs or boards without PSRAM. */
static uint8_t *ql700_alloc_bitmap(size_t bytes)
{
    uint8_t *bitmap = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bitmap) {
        ESP_LOGD(TAG, "Allocated %u-byte QL-700 bitmap in PSRAM", (unsigned)bytes);
        return bitmap;
    }

    ESP_LOGW(TAG, "PSRAM allocation of %u bytes failed (largest PSRAM block: %u); trying internal RAM",
             (unsigned)bytes,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bitmap = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!bitmap) {
        ESP_LOGE(TAG, "Raster allocation failed: requested %u bytes, largest internal block: %u",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return bitmap;
}

/* QL-700 bitmap endpoint for DK-22205 62 mm continuous stock.
 * Body: 150..2952 rows (12.7..250 mm), each 90 bytes,
 * MSB-first head dots. The full body is received before the printer is
 * touched, so a client disconnect cannot leave a partial job in the printer.
 */
static esp_err_t api_ql700_bitmap_print(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");

    char confirmation[24] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-QL700-Confirm", confirmation,
                                    sizeof confirmation) != ESP_OK ||
        strcmp(confirmation, "print-62mm") != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"confirmation_required\"}");
    }
    const size_t body_len = (size_t)req->content_len;
    if (body_len < (size_t)QL700_BITMAP_MIN_ROWS * QL700_ROW_BYTES ||
        body_len > (size_t)QL700_BITMAP_MAX_ROWS * QL700_ROW_BYTES ||
        body_len % QL700_ROW_BYTES != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"invalid_90_byte_raster_length\"}");
    }
    if (!transport_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"ql700_not_connected\"}");
    }

    uint8_t *bitmap = ql700_alloc_bitmap(body_len);
    if (!bitmap) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"bitmap_out_of_memory\"}");
    }
    size_t received = 0;
    while (received < body_len) {
        int n = httpd_req_recv(req, (char *)bitmap + received,
                               body_len - received);
        if (n <= 0) {
            heap_caps_free(bitmap);
            /* No print commands have been sent. */
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    if (!PRINTER_LOCK(150000)) {
        heap_caps_free(bitmap);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"busy\"}");
    }
    ql700_status_t st = {0};
    const bool ready = ql700_query_status(&st) &&
        st.media_type == 0x0a && st.media_width_mm == 62 &&
        st.media_length_mm == 0 && st.error1 == 0 &&
        st.error2 == 0 && st.phase_type == 0;
    if (!ready) {
        PRINTER_UNLOCK();
        heap_caps_free(bitmap);
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"requires_idle_62mm_continuous_roll\"}");
    }

    const unsigned rows = (unsigned)(body_len / QL700_ROW_BYTES);
    pt_led_set(PT_LED_PRINTING);
    const bool sent = ql700_send_bitmap_page_62mm(
        ql700_send_to_usb, &s_transport, bitmap, (uint16_t)rows);
    pt_led_set(transport_ready() ? PT_LED_READY : PT_LED_USB_WAITING);
    PRINTER_UNLOCK();
    heap_caps_free(bitmap);
    if (!sent) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"usb_send_failed_check_printer_before_retry\"}");
    }
    char response[160];
    int len = snprintf(response, sizeof response,
        "{\"ok\":true,\"rows\":%u,\"printer_complete_verified\":false,\"format\":\"62mm_continuous\"}",
        rows);
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, response, len);
}

static esp_err_t api_ql700_testprint(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");

    /* A POST plus exact acknowledgement header is required to avoid
     * accidental print/cut from a browser, link preview or stale SPA.
     */
    char acknowledge[24] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-QL700-Confirm", acknowledge,
                                     sizeof acknowledge) != ESP_OK ||
        strcmp(acknowledge, "print-62mm") != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"confirmation_required\"}");
    }
    if (req->content_len != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"unexpected_body\"}");
    }
    if (!transport_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"ql700_not_connected\"}");
    }
    if (!PRINTER_LOCK(150000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"busy\"}");
    }

    ql700_status_t st = {0};
    bool received = ql700_query_status(&st);
    if (!received) {
        PRINTER_UNLOCK();
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"ql700_status_unavailable\"}");
    }
    /* Explicitly require known continuous 62mm stock, and idle printer.
     * Die-cut and other width rolls need their own raster geometry.
     */
    if (st.media_type != 0x0a || st.media_width_mm != 62 ||
        st.media_length_mm != 0 || st.error1 != 0 || st.error2 != 0 ||
        st.phase_type != 0) {
        PRINTER_UNLOCK();
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"requires_idle_62mm_continuous_roll\"}");
    }

    pt_led_set(PT_LED_PRINTING);
    bool sent = ql700_send_test_page_62mm(ql700_send_to_usb, &s_transport);
    pt_led_set(transport_ready() ? PT_LED_READY : PT_LED_USB_WAITING);
    PRINTER_UNLOCK();

    if (!sent) {
        /* Partial USB writes may mean printer needs power-cycling. Avoid
         * blindly retrying: that can create duplicate or malformed jobs. */
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"usb_send_failed_check_printer_before_retry\"}");
    }
    /* USB send completed; this does NOT prove the printer finished. */
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req,
        "{\"ok\":true,\"sent\":true,\"printer_complete_verified\":false,"
        "\"format\":\"62mm_continuous\",\"raster_rows\":360}");
}

static esp_err_t api_status(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    /* Status is polled frequently; close the socket after every poll so
     * the small HTTP socket pool remains available for OTA uploads. */
    httpd_resp_set_hdr(req, "Connection", "close");
    const char *ota_available = ota_gate_open() ? "true" : "false";
    return ql700_api_status(req, ota_available);
}

/* GET /api/info -- firmware metadata, independent of printer connection. */
static esp_err_t api_info(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    const esp_app_desc_t *app = esp_app_get_description();
    const char *version = (app && app->version[0]) ? app->version : "";
    char body[128];
    int length = snprintf(body, sizeof body,
                          "{\"ok\":true,\"printer\":\"QL-700\",\"fw_version\":\"%s\"}",
                          version);
    if (length < 0 || (size_t)length >= sizeof body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"overflow\"}");
    }
    return httpd_resp_send(req, body, length);
}

/* SPA assets embedded via EMBED_FILES in CMakeLists.txt. The text
 * assets (index.html, setup.html, material-icons.json) come in two
 * forms -- plain bytes and gzip-compressed -- emitted in lockstep
 * by scripts/build_spa_assets.py at build time so they cannot drift.
 * send_text_asset() picks which to ship based on Accept-Encoding.
 * qrcode.min.js is no longer a separate route; the bundler inlines
 * it into index.html. */
extern const uint8_t index_html_start[]    asm("_binary_index_html_start");
extern const uint8_t index_html_end[]      asm("_binary_index_html_end");
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

extern const uint8_t setup_html_start[]    asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[]      asm("_binary_setup_html_end");
extern const uint8_t setup_html_gz_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_gz_end[]   asm("_binary_setup_html_gz_end");

/* Material Icons (filled) font (woff2, ~125 KB, already compressed --
 * gzipping won't help) + name-to-codepoint table (JSON, gzips well).
 * Apache-2.0, (c) Google, https://github.com/marella/material-icons. */
extern const uint8_t icon_woff2_start[]   asm("_binary_material_icons_woff2_start");
extern const uint8_t icon_woff2_end[]     asm("_binary_material_icons_woff2_end");
extern const uint8_t icon_json_start[]    asm("_binary_material_icons_json_start");
extern const uint8_t icon_json_end[]      asm("_binary_material_icons_json_end");
extern const uint8_t icon_json_gz_start[] asm("_binary_material_icons_json_gz_start");
extern const uint8_t icon_json_gz_end[]   asm("_binary_material_icons_json_gz_end");

/* Multi-resolution favicon.ico (~34 KB), served at /favicon.ico. */
extern const uint8_t favicon_start[]    asm("_binary_favicon_ico_start");
extern const uint8_t favicon_end[]      asm("_binary_favicon_ico_end");

/* True when the request advertises gzip in Accept-Encoding. Crude
 * substring match -- false negatives just mean we ship plain bytes,
 * which is always safe. False positives on weird tokens like
 * "x-gzip" don't happen in real-world clients. */
static bool client_accepts_gzip(httpd_req_t *req)
{
    char buf[64];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", buf, sizeof buf) != ESP_OK)
        return false;
    return strstr(buf, "gzip") != NULL;
}

/* Send one of two embedded blobs (gzipped or plain) based on the
 * request's Accept-Encoding. Sets CORS + Content-Type + Vary so
 * intermediate caches keep the two forms separate. */
static esp_err_t send_text_asset(httpd_req_t *req,
                                 const char *content_type,
                                 const uint8_t *plain, size_t plain_len,
                                 const uint8_t *gz,    size_t gz_len)
{
    cors_headers(req);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    if (client_accepts_gzip(req)) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)gz, gz_len);
    }
    return httpd_resp_send(req, (const char *)plain, plain_len);
}

/* CRC32 of the gzipped index.html blob, formatted as a quoted ETag.
 * Computed once on first request and cached -- the embedded blob
 * never changes within a single firmware image, so neither does its
 * CRC. CRC32 (vs git describe) means any rebuild that produced
 * different bundled bytes invalidates client caches automatically,
 * even when the working tree was already --dirty so PROJECT_VER did
 * not move. Collision risk for one resource is ~1 in 4 billion. */
static const char *index_etag(void)
{
    static char etag[12];   /* "\"" + 8 hex + "\"" + NUL */
    if (etag[0]) return etag;
    uint32_t crc = esp_crc32_le(0, index_html_gz_start,
                                index_html_gz_end - index_html_gz_start);
    snprintf(etag, sizeof etag, "\"%08lx\"", (unsigned long)crc);
    return etag;
}

static esp_err_t api_index(httpd_req_t *req)
{
    /* Cache for 5 min so reloads within a session are instant; the
     * ETag (CRC32 of the bundled SPA) lets the browser revalidate
     * cheaply across longer gaps and -- crucially -- changes any
     * time the SPA bytes change, including dirty rebuilds where
     * the firmware version string did not move. */
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=300");
    const char *etag = index_etag();
    httpd_resp_set_hdr(req, "ETag", etag);
    char inm[16];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match",
                                    inm, sizeof inm) == ESP_OK
        && strcmp(inm, etag) == 0) {
        cors_headers(req);
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    return send_text_asset(req, "text/html",
        index_html_start,    index_html_end    - index_html_start,
        index_html_gz_start, index_html_gz_end - index_html_gz_start);
}

/* Browsers auto-request /favicon.ico on every page load. Serve the
 * embedded multi-resolution .ico with a long-lived Cache-Control so
 * it's only fetched once. */
static esp_err_t api_favicon(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return httpd_resp_send(req, (const char *)favicon_start,
                           favicon_end - favicon_start);
}

static esp_err_t api_icon_woff2(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "font/woff2");
    /* font assets are immutable; long Cache-Control lets the browser
     * skip re-downloading the 125 KB blob on every page reload. */
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return httpd_resp_send(req, (const char *)icon_woff2_start,
                           icon_woff2_end - icon_woff2_start);
}

static esp_err_t api_icon_json(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return send_text_asset(req, "application/json",
        icon_json_start,    icon_json_end    - icon_json_start,
        icon_json_gz_start, icon_json_gz_end - icon_json_gz_start);
}

/* ----- OTA, gated on physical QL-700 Editor Lite mode ----- *
 *
 * Auth model: the printer's physical slider in EL position is the
 * gate. Anyone uploading a new firmware must have walked up to the
 * printer and slid the switch -- the same threat model as the BOOT-
 * button Wi-Fi reset, applied to a more discoverable user-facing
 * control. Editor Lite already coincides with "printer cannot print", so
 * the OTA window is naturally a maintenance window.
 *
 * The window stays open as long as the slider stays in EL -- we
 * trust the user to slide back. If Editor Lite ends mid-upload, we abort
 * the in-flight write so a half-baked image never becomes the boot
 * target. esp_ota_end() validates SHA256 from the image header; we
 * additionally verify the new image's project_name is "labelsis"
 * before flipping the boot partition, so a different ESP32 binary
 * uploaded by accident bricks nothing. Bootloader rollback (enabled
 * via CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) catches a new image
 * that crashes before it can call esp_ota_mark_app_valid_*. */

#include "esp_ota_ops.h"

static bool ota_editor_lite_gate(void)
{
    return s_transport_state && strcmp(s_transport_state, "editor_lite") == 0;
}

static bool ota_gate_open(void)
{
    return ota_button_gate_active() || ota_editor_lite_gate();
}

static esp_err_t api_ota_status(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    const esp_app_desc_t  *app  = esp_app_get_description();
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    bool open = ota_gate_open();
    /* Reason text aimed at whoever just hit the panel: name the
     * actual gate that's open (so the user knows why), or both
     * options to open it when neither is engaged. */
    const char *reason;
    if (ota_button_gate_active() && ota_editor_lite_gate())     reason = "Editor Lite mode + BOOT button gate";
    else if (ota_button_gate_active())                    reason = "BOOT button gate (hold 2-30 s + release to close)";
    else if (ota_editor_lite_gate())                            reason = "Editor Lite mode";
    else reason = "printer must be in Editor Lite mode (slide switch to EL), "
                  "or hold the BOOT button 2-30 s and release (device must be online)";
    /* next_slot_state surfaces the bootloader's view of the inactive
     * slot -- "valid" / "invalid" / "aborted" tells the user (or a
     * client like labelsis-ota.py) whether the slot is healthy enough
     * to accept the next OTA. Without it, a wedged slot looks
     * identical to a fresh one until the upload actually fails. */
    const char *next_state = "unknown";
    esp_ota_img_states_t st_next;
    if (next && esp_ota_get_state_partition(next, &st_next) == ESP_OK) {
        switch (st_next) {
            case ESP_OTA_IMG_NEW:            next_state = "new";            break;
            case ESP_OTA_IMG_PENDING_VERIFY: next_state = "pending_verify"; break;
            case ESP_OTA_IMG_VALID:          next_state = "valid";          break;
            case ESP_OTA_IMG_INVALID:        next_state = "invalid";        break;
            case ESP_OTA_IMG_ABORTED:        next_state = "aborted";        break;
            case ESP_OTA_IMG_UNDEFINED:      next_state = "undefined";      break;
        }
    }
    char body[400];
    int n = snprintf(body, sizeof body,
        "{\"available\":%s,\"reason\":\"%s\","
        "\"running_slot\":\"%s\",\"next_slot\":\"%s\","
        "\"next_slot_state\":\"%s\","
        "\"app\":{\"name\":\"%s\",\"version\":\"%s\"}}",
        open ? "true" : "false",
        reason,
        run  ? run->label  : "?",
        next ? next->label : "?",
        next_state,
        app ? app->project_name : "",
        app ? app->version      : "");
    return httpd_resp_send(req, body, n);
}

/* Drain (discard) up to `total` bytes from the request body. Used by
 * ota_fail before sending the error response so the client gets a
 * clean read of the response instead of an RST mid-stream -- httpd
 * closes the connection when the handler returns, so any unread
 * request body becomes a connection reset that pre-empts the
 * response. Bounded by httpd's recv_wait_timeout (5 s); a stalled
 * client can't hold the handler hostage. */
static void drain_request_body(httpd_req_t *req, int total)
{
    char buf[1024];
    while (total > 0) {
        int want = total < (int)sizeof buf ? total : (int)sizeof buf;
        int got  = httpd_req_recv(req, buf, want);
        if (got <= 0) return;     /* timeout / RST / EOF -- stop */
        total -= got;
    }
}

/* Always include esp_err name + Connection: close so:
 *  - the client (CLI / SPA) can surface the underlying ESP-IDF
 *    failure (ESP_ERR_FLASH_OP_TIMEOUT etc.) instead of just the
 *    coarse token (ota_write).
 *  - the FIN is well-defined; clients don't try to reuse the socket.
 * `drain_bytes` is how many request-body bytes the caller hasn't
 * read yet (0 if the body wasn't streamed, or content_len - written
 * if it was); see drain_request_body for why this matters. */
static esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t h,
                          const char *status, const char *reason,
                          esp_err_t esp_err, int drain_bytes)
{
    if (h) esp_ota_abort(h);
    if (drain_bytes > 0) drain_request_body(req, drain_bytes);
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, status);
    char body[192];
    int n;
    if (esp_err == ESP_OK) {
        n = snprintf(body, sizeof body,
                     "{\"ok\":false,\"error\":\"%s\"}", reason);
    } else {
        n = snprintf(body, sizeof body,
                     "{\"ok\":false,\"error\":\"%s\",\"esp_err\":\"%s\"}",
                     reason, esp_err_to_name(esp_err));
    }
    return httpd_resp_send(req, body, n);
}

/* Serialise concurrent OTA uploads. Two simultaneous POSTs would
 * race in esp_ota_begin and corrupt each other's writes; the second
 * gets 409 ota_busy. Set on entry, cleared on every non-success
 * exit (success doesn't return -- esp_restart() reboots).
 *
 * s_ota_busy_acquired_ms is the esp_timer epoch (ms) when the flag
 * was last acquired. If the wrapper's handler crashes / task panics
 * without clearing, the flag would otherwise stay set forever and
 * brick OTA over Wi-Fi until manual reboot. Auto-clear after 5 min
 * gives any legitimately long OTA plenty of margin (real ones are
 * ~30 s) while bounding the worst-case "OTA blocked" window. */
#include <stdatomic.h>
#include "esp_timer.h"
#define OTA_BUSY_STALE_MS (5 * 60 * 1000)
static atomic_flag s_ota_busy           = ATOMIC_FLAG_INIT;
static int64_t    s_ota_busy_acquired_ms = 0;

/* Try esp_ota_begin once; on failure, force-erase the partition and
 * retry once. Recovers a slot that's been left in a bad state by a
 * prior aborted upload (RST mid-stream, gate closed mid-upload, etc.)
 * without requiring a power-cycle. If the retry also fails, the
 * caller surfaces esp_err to the client. */
static esp_err_t ota_begin_with_retry(const esp_partition_t *part,
                                      esp_ota_handle_t *out)
{
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, out);
    if (err == ESP_OK) return ESP_OK;
    ESP_LOGW(TAG, "ota: esp_ota_begin failed (%s) -- erasing %s and retrying",
             esp_err_to_name(err), part->label);
    esp_err_t e2 = esp_partition_erase_range(part, 0, part->size);
    if (e2 != ESP_OK) {
        ESP_LOGE(TAG, "ota: erase_range failed: %s", esp_err_to_name(e2));
        return err;   /* original error -- erase didn't help */
    }
    return esp_ota_begin(part, OTA_SIZE_UNKNOWN, out);
}

static esp_err_t api_ota_upload_inner(httpd_req_t *req)
{
    int total = req->content_len;
    if (!ota_gate_open()) {
        return ota_fail(req, 0, "403 Forbidden", "gate_closed",
                        ESP_OK, total);
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        return ota_fail(req, 0, "500 Internal Server Error", "no_ota_slot",
                        ESP_OK, total);
    }
    /* Reject up-front instead of writing megabytes only to fail when
     * the partition runs out. Saves flash wear on a malformed client. */
    if (total > (int)next->size) {
        ESP_LOGW(TAG, "ota: rejecting Content-Length %d > slot %s size %d",
                 total, next->label, (int)next->size);
        return ota_fail(req, 0, "413 Payload Too Large", "too_large",
                        ESP_OK, total);
    }
    ESP_LOGI(TAG, "ota: starting upload to slot %s (%d bytes incoming)",
             next->label, total);

    esp_ota_handle_t ota = 0;
    esp_err_t err = ota_begin_with_retry(next, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_begin failed (after retry): %s",
                 esp_err_to_name(err));
        return ota_fail(req, 0, "500 Internal Server Error", "ota_begin",
                        err, total);
    }

    /* Stream the request body straight into flash. 1 KB chunks keep
     * the stack budget and the per-write erase-cycle overhead modest;
     * the limiting factor is flash write throughput, not RAM. */
    char buf[1024];
    int  remaining       = total;
    int  written         = 0;
    bool hdr_validated   = false;
    /* esp_app_desc_t lives at offset 0x20 in an ESP-IDF app image
     * (right after the 24-byte image header + 8-byte segment header).
     * Validating its magic + project_name from the very first chunk
     * lets us reject a wrong-project / non-IDF binary in <1 s instead
     * of after the full multi-MB upload. */
    const int hdr_offset_bytes = 0x20 + (int)sizeof(esp_app_desc_t);
    while (remaining > 0) {
        if (!ota_gate_open()) {
            ESP_LOGW(TAG, "ota: gate closed mid-upload after %d bytes -- aborting", written);
            return ota_fail(req, ota, "403 Forbidden",
                            "gate_closed_mid_upload", ESP_OK, remaining);
        }
        int want = remaining < (int)sizeof buf ? remaining : (int)sizeof buf;
        int got  = httpd_req_recv(req, buf, want);
        if (got <= 0) {
            /* Recv already failed -- the connection is dead, no
             * point trying to drain. ota_fail still cleans up the
             * OTA handle and best-effort sends the response (it
             * won't reach the client but at least we don't leak). */
            ESP_LOGE(TAG, "ota: recv failed at %d bytes (got=%d)", written, got);
            return ota_fail(req, ota, "400 Bad Request", "recv_failed",
                            ESP_OK, 0);
        }
        /* First chunk: validate magic + project_name BEFORE flushing
         * to flash. The 1 KB recv buffer comfortably contains the
         * full app desc (~256 B). For tiny uploads where the first
         * chunk is shorter, hdr_validated stays false and the
         * existing post-end check catches the mismatch. */
        if (!hdr_validated && written == 0 && got >= hdr_offset_bytes) {
            /* memcpy into a stack-aligned esp_app_desc_t -- buf+0x20
             * happens to be 4-byte aligned today (buf is 16-aligned
             * stack memory) but the unaligned pointer cast is UB on
             * Xtensa per the ABI; copy is portable + free-ish. */
            esp_app_desc_t desc;
            memcpy(&desc, buf + 0x20, sizeof desc);
            if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strncmp(desc.project_name, "labelsis",
                        sizeof desc.project_name) != 0) {
                ESP_LOGW(TAG, "ota: early reject -- magic=0x%08lx project=%.32s",
                         (unsigned long)desc.magic_word, desc.project_name);
                return ota_fail(req, ota, "400 Bad Request", "wrong_project",
                                ESP_OK, remaining - got);
            }
            hdr_validated = true;
        }
        err = esp_ota_write(ota, buf, got);
        if (err != ESP_OK && written == 0) {
            /* First-write failure: classic symptom of a slot wedged
             * by a prior aborted OTA -- esp_ota_begin returned OK
             * (claimed erase succeeded) but the underlying flash
             * sectors weren't actually erased. Force a full erase
             * via esp_partition_erase_range, restart the OTA
             * transaction, and retry the write once. We can do this
             * because the failed chunk is still in `buf` and no
             * earlier data has been committed (written == 0).
             *
             * For chunks past the first, the prior N chunks are
             * already in flash; an abort+erase would discard them
             * and we can't replay them from the wire. So this
             * recovery only fires on the first chunk; subsequent
             * failures bail. */
            ESP_LOGW(TAG, "ota: first esp_ota_write failed (%s) -- "
                          "abort + force-erase + retry once",
                     esp_err_to_name(err));
            esp_ota_abort(ota);
            ota = 0;
            esp_err_t e2 = esp_partition_erase_range(next, 0, next->size);
            if (e2 == ESP_OK) e2 = esp_ota_begin(next, OTA_SIZE_UNKNOWN, &ota);
            if (e2 == ESP_OK) e2 = esp_ota_write(ota, buf, got);
            if (e2 != ESP_OK) {
                ESP_LOGE(TAG, "ota: write recovery failed: %s",
                         esp_err_to_name(e2));
                return ota_fail(req, ota, "500 Internal Server Error",
                                "ota_write", e2, remaining - got);
            }
            ESP_LOGI(TAG, "ota: write recovery OK -- continuing");
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota: esp_ota_write failed at %d bytes: %s",
                     written, esp_err_to_name(err));
            return ota_fail(req, ota, "500 Internal Server Error",
                            "ota_write", err, remaining - got);
        }
        written   += got;
        remaining -= got;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        /* esp_ota_end validates the SHA-in-header against the bytes we
         * wrote. ESP_ERR_OTA_VALIDATE_FAILED here means the image is
         * corrupt or truncated; never accept it. */
        ESP_LOGE(TAG, "ota: esp_ota_end failed: %s", esp_err_to_name(err));
        return ota_fail(req, 0, "400 Bad Request",
                        err == ESP_ERR_OTA_VALIDATE_FAILED
                            ? "image_validate" : "ota_end",
                        err, 0);
    }

    /* After SHA passes, sanity-check that this is actually a LabelSis
     * binary -- otherwise a stray ESP32 firmware uploaded to /api/ota
     * could brick the device on next boot. */
    esp_app_desc_t new_desc;
    if (esp_ota_get_partition_description(next, &new_desc) == ESP_OK
        && strcmp(new_desc.project_name, "labelsis") != 0) {
        ESP_LOGE(TAG, "ota: project_name mismatch \"%s\" != \"labelsis\"",
                 new_desc.project_name);
        /* Image is on disk but never made boot target; next OTA will
         * overwrite the slot. */
        return ota_fail(req, 0, "400 Bad Request", "wrong_project",
                        ESP_OK, 0);
    }

    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: set_boot failed: %s", esp_err_to_name(err));
        return ota_fail(req, 0, "500 Internal Server Error", "set_boot",
                        err, 0);
    }

    ESP_LOGI(TAG, "ota: %d bytes accepted, version=\"%s\", booting from %s",
             written, new_desc.version, next->label);

    /* Send the response BEFORE rebooting so the SPA gets confirmation
     * (and can show its countdown) instead of a connection-reset.
     * Connection: close so the client sees a clean FIN and doesn't
     * attempt to reuse the socket while we're rebooting. */
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    char body[160];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"slot\":\"%s\",\"version\":\"%s\",\"reboot_in_ms\":2000}",
        next->label, new_desc.version);
    httpd_resp_send(req, body, n);

    /* Defer the reboot to a small delay so the response actually
     * lands. esp_restart returns void and triggers SoC reset. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

/* Public handler -- guards api_ota_upload_inner with a busy flag so
 * two concurrent OTA POSTs can't race in esp_ota_begin. The success
 * path doesn't return (esp_restart() reboots), so we only need to
 * clear the flag on every error path -- that happens here.
 *
 * If the flag is currently set BUT the timestamp is >5 min old, the
 * previous handler must have panicked / been killed without clearing
 * (FreeRTOS task crash, stack overflow, etc.). Treat as stale and
 * proceed -- otherwise a single buggy OTA could brick the device's
 * Wi-Fi-only update path until the next physical reboot. */
static esp_err_t api_ota_upload(httpd_req_t *req)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (atomic_flag_test_and_set(&s_ota_busy)) {
        int64_t age_ms = now_ms - s_ota_busy_acquired_ms;
        if (s_ota_busy_acquired_ms != 0 && age_ms > OTA_BUSY_STALE_MS) {
            ESP_LOGW(TAG, "ota: stale s_ota_busy (held %lld ms) -- "
                          "clearing and proceeding", age_ms);
            /* Flag already set; we now own it (no real concurrent
             * caller -- previous handler died). Fall through. */
        } else {
            ESP_LOGW(TAG, "ota: rejecting concurrent OTA POST -- ota_busy "
                          "(held %lld ms so far)", age_ms);
            return ota_fail(req, 0, "409 Conflict", "ota_busy",
                            ESP_OK, req->content_len);
        }
    }
    s_ota_busy_acquired_ms = now_ms;
    esp_err_t r = api_ota_upload_inner(req);
    s_ota_busy_acquired_ms = 0;
    atomic_flag_clear(&s_ota_busy);
    return r;
}

/* POST /api/ota/erase-next -- explicit erase of the inactive OTA
 * slot, behind the same gate. Recovery path for the case where the
 * automatic write-failure retry in api_ota_upload_inner can't fix
 * the wedge (silent flash failure, etc.). The next OTA upload then
 * writes to a guaranteed-fresh slot.
 *
 * Synchronous -- esp_partition_erase_range blocks until done; on a
 * 1.94 MB S2 slot that's ~5-10 s. Doesn't reboot. */
static esp_err_t api_ota_erase_next(httpd_req_t *req)
{
    if (!ota_gate_open()) {
        return ota_fail(req, 0, "403 Forbidden", "gate_closed",
                        ESP_OK, req->content_len);
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        return ota_fail(req, 0, "500 Internal Server Error", "no_ota_slot",
                        ESP_OK, req->content_len);
    }
    ESP_LOGW(TAG, "ota: explicit erase of slot %s requested (size %u)",
             next->label, (unsigned)next->size);
    esp_err_t err = esp_partition_erase_range(next, 0, next->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: erase_range failed: %s", esp_err_to_name(err));
        return ota_fail(req, 0, "500 Internal Server Error",
                        "erase_failed", err, req->content_len);
    }
    ESP_LOGI(TAG, "ota: slot %s erased", next->label);
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    char body[128];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":true,\"slot\":\"%s\",\"size\":%u}",
                     next->label, (unsigned)next->size);
    return httpd_resp_send(req, body, n);
}

/* POST /api/reboot -- soft reboot, behind the same gate as /api/ota.
 * Lets us recover a wedged device (e.g. an OTA slot left in a bad
 * state by a prior aborted upload) without needing physical access
 * to power-cycle. Same gate so a stray LAN visitor can't toggle it. */
static esp_err_t api_reboot(httpd_req_t *req)
{
    if (!ota_gate_open()) {
        return ota_fail(req, 0, "403 Forbidden", "gate_closed",
                        ESP_OK, req->content_len);
    }
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot_in_ms\":500}");
    ESP_LOGW(TAG, "reboot: soft reboot requested via /api/reboot");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

/* Called once Wi-Fi STA has an IP AND the HTTP server is up. If the
 * currently-running app is in PENDING_VERIFY (just OTA'd into), tell
 * the bootloader the new image is good. Otherwise the bootloader
 * rolls back to the previous slot on the next reset. */
static void ota_confirm_running_image(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "ota: confirmed running image (slot %s) -- rollback cancelled",
                     run->label);
        }
    }
}

static httpd_handle_t s_http;

/* Forward decls: api_scan and api_setup are defined further down with
 * the AP-onboarding handlers, but the STA-mode http_up registers them
 * too so the SPA's Wi-Fi tab can re-provision in place. */
static esp_err_t api_scan(httpd_req_t *req);
static esp_err_t api_setup(httpd_req_t *req);
/* Wi-Fi list endpoints live with api_setup further down (they share
 * NVS plumbing); declare here so http_up can reference them. */
static esp_err_t api_wifi_list(httpd_req_t *req);
static esp_err_t api_wifi_add(httpd_req_t *req);
static esp_err_t api_wifi_delete(httpd_req_t *req);
static esp_err_t api_wifi_order(httpd_req_t *req);
static esp_err_t api_wifi_prescan(httpd_req_t *req);

static esp_err_t http_up(uint16_t port)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = port ? port : 80;
    cfg.lru_purge_enable = true;
    /* Wildcard matching enables the OPTIONS catch-all below; specific
     * routes still match exactly. */
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    /* Default is 8; we register 21 routes (status, info, print, cut,
     * feed, scan, setup, index, fonts*2, favicon, ota*2, wifi*5,
     * reboot, ota/erase-next, OPTIONS catch-all) and want headroom. */
    cfg.max_uri_handlers = 24;
    /* Leave max_open_sockets at the default 7. esp_http_server caps
     * it at CONFIG_LWIP_MAX_SOCKETS - 3, which is 7 with the default
     * lwIP build; asking for more makes httpd_start error out and
     * the device boots without HTTP -- a self-bricking foot-gun on
     * an OTA target. The real socket-starvation fix is in two
     * complementary places: api_status now sends Connection: close
     * (sockets recycle every poll, no keep-alive squat) and the SPA
     * pauses its 3 s polling for the duration of an OTA upload
     * (no parallel GETs to fill the pool while the POST streams).
     * If we ever need a bigger pool, bump CONFIG_LWIP_MAX_SOCKETS
     * in the board sdkconfig first. */
    /* recv/send_wait_timeout default to 5 s -- fine for our needs. */

    if (httpd_start(&s_http, &cfg) != ESP_OK) return ESP_FAIL;

    static const httpd_uri_t status_route = {
        .uri = "/api/status", .method = HTTP_GET,
        .handler = api_status, .user_ctx = NULL,
    };
    static const httpd_uri_t info_route = {
        .uri = "/api/info", .method = HTTP_GET,
        .handler = api_info, .user_ctx = NULL,
    };
    static const httpd_uri_t ql700_bitmap_route = {
        .uri = "/api/ql700/print", .method = HTTP_POST,
        .handler = api_ql700_bitmap_print, .user_ctx = NULL,
    };
    static const httpd_uri_t ql700_test_route = {
        .uri = "/api/ql700/testprint", .method = HTTP_POST,
        .handler = api_ql700_testprint, .user_ctx = NULL,
    };
    static const httpd_uri_t scan_route = {
        .uri = "/api/scan", .method = HTTP_GET,
        .handler = api_scan, .user_ctx = NULL,
    };
    static const httpd_uri_t setup_route = {
        .uri = "/api/setup", .method = HTTP_POST,
        .handler = api_setup, .user_ctx = NULL,
    };
    static const httpd_uri_t index_route = {
        .uri = "/", .method = HTTP_GET,
        .handler = api_index, .user_ctx = NULL,
    };
    static const httpd_uri_t icon_woff2_route = {
        .uri = "/fonts/material-icons.woff2", .method = HTTP_GET,
        .handler = api_icon_woff2, .user_ctx = NULL,
    };
    static const httpd_uri_t icon_json_route = {
        .uri = "/fonts/material-icons.json", .method = HTTP_GET,
        .handler = api_icon_json, .user_ctx = NULL,
    };
    static const httpd_uri_t favicon_route = {
        .uri = "/favicon.ico", .method = HTTP_GET,
        .handler = api_favicon, .user_ctx = NULL,
    };
    static const httpd_uri_t ota_status_route = {
        .uri = "/api/ota/status", .method = HTTP_GET,
        .handler = api_ota_status, .user_ctx = NULL,
    };
    static const httpd_uri_t ota_upload_route = {
        .uri = "/api/ota", .method = HTTP_POST,
        .handler = api_ota_upload, .user_ctx = NULL,
    };
    static const httpd_uri_t wifi_list_route = {
        .uri = "/api/wifi/aps", .method = HTTP_GET,
        .handler = api_wifi_list, .user_ctx = NULL,
    };
    static const httpd_uri_t wifi_add_route = {
        .uri = "/api/wifi/aps/add", .method = HTTP_POST,
        .handler = api_wifi_add, .user_ctx = NULL,
    };
    static const httpd_uri_t wifi_delete_route = {
        .uri = "/api/wifi/aps/delete", .method = HTTP_POST,
        .handler = api_wifi_delete, .user_ctx = NULL,
    };
    static const httpd_uri_t wifi_order_route = {
        .uri = "/api/wifi/aps/order", .method = HTTP_POST,
        .handler = api_wifi_order, .user_ctx = NULL,
    };
    static const httpd_uri_t wifi_prescan_route = {
        .uri = "/api/wifi/prescan", .method = HTTP_POST,
        .handler = api_wifi_prescan, .user_ctx = NULL,
    };
    static const httpd_uri_t reboot_route = {
        .uri = "/api/reboot", .method = HTTP_POST,
        .handler = api_reboot, .user_ctx = NULL,
    };
    static const httpd_uri_t ota_erase_next_route = {
        .uri = "/api/ota/erase-next", .method = HTTP_POST,
        .handler = api_ota_erase_next, .user_ctx = NULL,
    };
    static const httpd_uri_t cors_route = {
        .uri = "/*", .method = HTTP_OPTIONS,
        .handler = cors_preflight, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_http, &status_route);
    httpd_register_uri_handler(s_http, &info_route);
    httpd_register_uri_handler(s_http, &ql700_test_route);
    httpd_register_uri_handler(s_http, &ql700_bitmap_route);
    httpd_register_uri_handler(s_http, &scan_route);
    httpd_register_uri_handler(s_http, &setup_route);
    httpd_register_uri_handler(s_http, &index_route);
    httpd_register_uri_handler(s_http, &icon_woff2_route);
    httpd_register_uri_handler(s_http, &icon_json_route);
    httpd_register_uri_handler(s_http, &favicon_route);
    httpd_register_uri_handler(s_http, &ota_status_route);
    httpd_register_uri_handler(s_http, &ota_upload_route);
    httpd_register_uri_handler(s_http, &wifi_list_route);
    httpd_register_uri_handler(s_http, &wifi_add_route);
    httpd_register_uri_handler(s_http, &wifi_delete_route);
    httpd_register_uri_handler(s_http, &wifi_order_route);
    httpd_register_uri_handler(s_http, &wifi_prescan_route);
    httpd_register_uri_handler(s_http, &reboot_route);
    httpd_register_uri_handler(s_http, &ota_erase_next_route);
    httpd_register_uri_handler(s_http, &cors_route);
    /* HTTP is serving requests now -- if we just OTA'd into this
     * image, that's enough proof of life to cancel the rollback
     * watchdog. (No-op on factory boots.) */
    ota_confirm_running_image();
    return ESP_OK;
}

/* AP onboarding */

#define AP_SETUP_SSID "labelsis-setup"

/* Catch-all GET handler -- serves the setup page for /, captive-portal
 * detection URLs (Apple/Android/Microsoft probes), and any unknown
 * path. With no DNS hijack the popup behaviour is best-effort: probes
 * see non-success HTML and most phones surface a "sign in" notice.
 * Externs for setup_html_{,gz_}{start,end} live next to the SPA
 * embeds at the top of this file. */
static esp_err_t ap_setup_page(httpd_req_t *req)
{
    return send_text_asset(req, "text/html",
        setup_html_start,    setup_html_end    - setup_html_start,
        setup_html_gz_start, setup_html_gz_end - setup_html_gz_start);
}

/* Append `s` (`n` bytes) to the response stream as a JSON string
 * literal -- RFC 8259 escaping for ", \, and any byte < 0x20 or >=
 * 0x7f. SSIDs are arbitrary bytes; non-printables become \uXXXX. */
static void send_json_string(httpd_req_t *req, const uint8_t *s, size_t n)
{
    httpd_resp_send_chunk(req, "\"", 1);
    char buf[16];
    for (size_t i = 0; i < n && s[i]; i++) {
        uint8_t c = s[i];
        int  blen;
        if      (c == '"' || c == '\\') { buf[0] = '\\'; buf[1] = (char)c; blen = 2; }
        else if (c >= 0x20 && c < 0x7f) { buf[0] = (char)c;                blen = 1; }
        else                            { blen = snprintf(buf, sizeof buf, "\\u%04x", c); }
        httpd_resp_send_chunk(req, buf, blen);
    }
    httpd_resp_send_chunk(req, "\"", 1);
}

/* GET /api/scan -- blocking scan on the STA interface (APSTA mode is
 * up). Returns up to 30 best-RSSI APs; the page dedupes by SSID. */
static esp_err_t api_scan(httpd_req_t *req)
{
    cors_headers(req);
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scan_failed\"}");
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;
    wifi_ap_record_t *recs = n ? calloc(n, sizeof *recs) : NULL;
    if (n && !recs) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    if (n) esp_wifi_scan_get_ap_records(&n, recs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"ok\":true,\"networks\":[", 23);
    for (uint16_t i = 0; i < n; i++) {
        if (i) httpd_resp_send_chunk(req, ",", 1);
        httpd_resp_send_chunk(req, "{\"ssid\":", 8);
        send_json_string(req, recs[i].ssid,
            strnlen((const char *)recs[i].ssid, sizeof recs[i].ssid));
        char tail[64];
        int  len = snprintf(tail, sizeof tail,
                            ",\"rssi\":%d,\"auth\":%u}",
                            recs[i].rssi, (unsigned)recs[i].authmode);
        httpd_resp_send_chunk(req, tail, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    free(recs);
    return ESP_OK;
}

/* Hand-parse "key":"value" out of body. Honours \" and \\ escapes
 * inside the value; everything else is taken byte-for-byte. The
 * setup body is small (< 256 bytes) and known-shape, so pulling in
 * cJSON for two strings isn't worth its .text. */
static bool extract_str(const char *body, const char *key,
                        char *out, size_t cap)
{
    char pat[64];
    int  patlen = snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += patlen;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; }
        else                    { out[i++] = *p++; }
    }
    if (*p != '"') return false;  /* unterminated or truncated */
    out[i] = '\0';
    return true;
}

/* Read the full request body into `body` (NUL-terminated).
 * Returns 0 on success or a negative errno-style code. */
static int read_json_body(httpd_req_t *req, char *body, size_t cap)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)cap) return -1;
    int rcvd = 0;
    while (rcvd < total) {
        int n = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (n <= 0) return -2;
        rcvd += n;
    }
    body[rcvd] = '\0';
    return 0;
}

static esp_err_t json_error(httpd_req_t *req, const char *status, const char *err)
{
    cors_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}", err);
    return httpd_resp_sendstr(req, body);
}

/* POST /api/setup -- onboarding entry point. Body: {ssid, password}.
 * Adds the AP to the list (or replaces its password if SSID exists),
 * replies, then reboots so the next boot tries it for real. Used
 * during AP-mode onboarding; in STA mode use /api/wifi/aps/add to
 * add another AP without rebooting. */
static esp_err_t api_setup(httpd_req_t *req)
{
    char body[300];
    if (read_json_body(req, body, sizeof body) != 0)
        return json_error(req, "400 Bad Request", "bad_body");

    char ssid[33] = {0}, password[65] = {0};
    bool got_ssid = extract_str(body, "ssid",     ssid,     sizeof ssid);
    extract_str(body, "password", password, sizeof password);
    if (!got_ssid || ssid[0] == '\0')
        return json_error(req, "400 Bad Request", "missing_ssid");

    esp_err_t err = credlist_add(ssid, password);
    if (err == ESP_ERR_NO_MEM)
        return json_error(req, "507 Insufficient Storage", "list_full");
    if (err != ESP_OK)
        return json_error(req, "500 Internal Server Error", "nvs_save");

    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    ESP_LOGI(TAG, "wifi: added \"%s\" to list -- rebooting", ssid);
    /* Let the response FIN onto the wire before we drop the AP. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* GET /api/wifi/aps -- list configured APs (no passwords) plus the
 * prescan toggle (so the SPA renders consistent state in one fetch). */
static esp_err_t api_wifi_list(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    wifi_creds_blob_t cl = {0};
    credlist_load(&cl);   /* empty list still produces valid JSON */
    bool prescan = prescan_load_enabled();

    /* Cap-bounded buffer: MAX_APS entries * ~80 chars each + chrome */
    char body[768];
    int  off = 0;
    off += snprintf(body + off, sizeof body - off, "{\"ok\":true,\"aps\":[");
    for (int i = 0; i < cl.count && off < (int)sizeof body - 64; i++) {
        bool last = (strcmp(cl.aps[i].ssid, cl.last_used) == 0);
        /* JSON-escape the SSID's quote/backslash. SSIDs are arbitrary
         * bytes per IEEE 802.11 but practical APs avoid control chars. */
        char esc[2 * sizeof cl.aps[i].ssid];
        int  e = 0;
        for (const char *s = cl.aps[i].ssid; *s && e < (int)sizeof esc - 2; s++) {
            if (*s == '"' || *s == '\\') esc[e++] = '\\';
            esc[e++] = *s;
        }
        esc[e] = '\0';
        off += snprintf(body + off, sizeof body - off,
                        "%s{\"ssid\":\"%s\",\"last_used\":%s}",
                        i ? "," : "", esc, last ? "true" : "false");
    }
    off += snprintf(body + off, sizeof body - off,
                    "],\"prescan\":%s,\"max_aps\":%d}",
                    prescan ? "true" : "false", MAX_APS);
    return httpd_resp_send(req, body, off);
}

/* POST /api/wifi/aps/add -- {ssid, password}. SSID match replaces
 * password; otherwise appends. No reboot -- the user can stay
 * connected to the current AP while editing the list. */
static esp_err_t api_wifi_add(httpd_req_t *req)
{
    char body[300];
    if (read_json_body(req, body, sizeof body) != 0)
        return json_error(req, "400 Bad Request", "bad_body");
    char ssid[33] = {0}, password[65] = {0};
    bool got_ssid = extract_str(body, "ssid",     ssid,     sizeof ssid);
    extract_str(body, "password", password, sizeof password);
    if (!got_ssid || ssid[0] == '\0')
        return json_error(req, "400 Bad Request", "missing_ssid");

    esp_err_t err = credlist_add(ssid, password);
    if (err == ESP_ERR_NO_MEM) return json_error(req, "507 Insufficient Storage", "list_full");
    if (err != ESP_OK)         return json_error(req, "500 Internal Server Error", "nvs_save");

    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* POST /api/wifi/aps/delete -- {ssid}. Idempotent. */
static esp_err_t api_wifi_delete(httpd_req_t *req)
{
    char body[200];
    if (read_json_body(req, body, sizeof body) != 0)
        return json_error(req, "400 Bad Request", "bad_body");
    char ssid[33] = {0};
    if (!extract_str(body, "ssid", ssid, sizeof ssid) || !ssid[0])
        return json_error(req, "400 Bad Request", "missing_ssid");

    if (credlist_delete(ssid) != ESP_OK)
        return json_error(req, "500 Internal Server Error", "nvs_save");

    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* POST /api/wifi/aps/order -- text/plain body, newline-separated SSIDs
 * in priority order. SSIDs not named keep their relative order at the
 * tail. Cheaper to parse than a JSON array; the SPA's list view sends
 * `ssids.join("\n")`. */
static esp_err_t api_wifi_order(httpd_req_t *req)
{
    int total = req->content_len;
    if (total < 0 || total > 8 * 33 + 16)
        return json_error(req, "400 Bad Request", "bad_body");
    char body[8 * 33 + 16];
    int rcvd = 0;
    while (rcvd < total) {
        int n = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (n <= 0) return ESP_FAIL;
        rcvd += n;
    }
    if (credlist_reorder(body, rcvd) != ESP_OK)
        return json_error(req, "500 Internal Server Error", "nvs_save");

    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* POST /api/wifi/prescan -- {enabled:bool}. Persisted to NVS; takes
 * effect on next boot (the boot loop reads it before the try-list). */
static esp_err_t api_wifi_prescan(httpd_req_t *req)
{
    char body[64];
    if (read_json_body(req, body, sizeof body) != 0)
        return json_error(req, "400 Bad Request", "bad_body");
    /* Accept either bare-bool ({"enabled":true}) -- extract_str is
     * string-only, so look for the literal "true"/"false" tokens. */
    bool enabled;
    if      (strstr(body, "\"enabled\":true"))  enabled = true;
    else if (strstr(body, "\"enabled\":false")) enabled = false;
    else return json_error(req, "400 Bad Request", "missing_enabled");

    if (prescan_save_enabled(enabled) != ESP_OK)
        return json_error(req, "500 Internal Server Error", "nvs_save");

    ESP_LOGI(TAG, "wifi: prescan %s (effective on next boot)",
             enabled ? "enabled" : "disabled");
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t http_ap_up(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;

    if (httpd_start(&s_http, &cfg) != ESP_OK) return ESP_FAIL;

    static const httpd_uri_t scan_route = {
        .uri = "/api/scan", .method = HTTP_GET,
        .handler = api_scan, .user_ctx = NULL,
    };
    static const httpd_uri_t setup_route = {
        .uri = "/api/setup", .method = HTTP_POST,
        .handler = api_setup, .user_ctx = NULL,
    };
    /* Catch-all serves the setup page for any GET (incl. captive-
     * portal probe URLs). Registered last; specific routes win. */
    static const httpd_uri_t catch_all = {
        .uri = "/*", .method = HTTP_GET,
        .handler = ap_setup_page, .user_ctx = NULL,
    };
    static const httpd_uri_t cors_route = {
        .uri = "/*", .method = HTTP_OPTIONS,
        .handler = cors_preflight, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_http, &scan_route);
    httpd_register_uri_handler(s_http, &setup_route);
    httpd_register_uri_handler(s_http, &catch_all);
    httpd_register_uri_handler(s_http, &cors_route);
    return ESP_OK;
}

/* mDNS */

static esp_err_t mdns_up(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    mdns_hostname_set("labelsis");
    mdns_instance_name_set("LabelSis print server");
    /* Advertise the HTTP service on the configured port. */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    return ESP_OK;
}

/* public */

esp_err_t pt_app_run(const pt_app_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_printer_mutex = xSemaphoreCreateMutex();
    if (!s_printer_mutex) return ESP_ERR_NO_MEM;

    /* Status LED first so the boot pattern is visible from the moment
     * power comes up, even if subsequent steps fail. */
    pt_led_init(&cfg->led);
    pt_led_set(PT_LED_BOOT);

    /* Start the reset-button watcher first so a stuck button can rescue
     * a board that would otherwise wedge in a connect/AP loop. */
    reset_button_up(cfg->reset_gpio_num);

    /* Resolve Wi-Fi creds: prefer the NVS list. If it's empty AND the
     * caller passed a compiled-in fallback, seed the list with that.
     * Boot iterates the list: last_used first (if still in the list),
     * then index order. AP-mode onboarding only runs when EVERY entry
     * fails. */
    wifi_creds_blob_t cl = {0};
    bool have_list = (credlist_load(&cl) == ESP_OK && cl.count > 0);
    bool have_cfg  = (cfg->wifi_ssid && cfg->wifi_password
                      && cfg->wifi_ssid[0] && cfg->wifi_password[0]);
    if (!have_list && have_cfg) {
        ESP_LOGI(TAG, "wifi: list empty, seeding from compiled-in cfg (%s)",
                 cfg->wifi_ssid);
        credlist_add(cfg->wifi_ssid, cfg->wifi_password);
        credlist_load(&cl);
        have_list = (cl.count > 0);
    }

    bool connected = false;
    if (have_list) {
        /* Build try-order: last_used (if in list) first, then in
         * priority order. Indices into cl.aps[]. */
        int order[MAX_APS], n = 0;
        int last_idx = (cl.last_used[0]) ? credlist_find(&cl, cl.last_used) : -1;
        if (last_idx >= 0) order[n++] = last_idx;
        for (int i = 0; i < cl.count; i++) {
            if (i != last_idx) order[n++] = i;
        }

        pt_led_set(PT_LED_STA_CONNECTING);

        /* Optional pre-scan: skip APs that aren't on air, so we don't
         * burn 15 s per missing entry. Off by default; opted in by the
         * user via /api/wifi/prescan. If the scan itself errors, fall
         * through to "try every AP" so a flaky radio doesn't strand
         * the device in AP mode. */
        bool visible[MAX_APS] = {0};
        bool filter           = false;
        if (prescan_load_enabled()) {
            ESP_LOGI(TAG, "wifi: pre-scan enabled -- scanning band first");
            if (prescan_filter(&cl, visible) == ESP_OK) {
                int seen = 0;
                for (int i = 0; i < cl.count; i++) if (visible[i]) seen++;
                ESP_LOGI(TAG, "wifi: prescan -- %d/%d configured APs on air",
                         seen, cl.count);
                filter = true;
            } else {
                ESP_LOGW(TAG, "wifi: prescan failed -- trying every AP");
            }
        }

        for (int j = 0; j < n; j++) {
            int idx = order[j];
            if (filter && !visible[idx]) {
                ESP_LOGI(TAG, "wifi: skipping %s (not visible in prescan)",
                         cl.aps[idx].ssid);
                continue;
            }
            const wifi_ap_t *ap = &cl.aps[idx];
            ESP_LOGI(TAG, "wifi: trying %s (%s)",
                     ap->ssid,
                     idx == last_idx     ? "last used" :
                     j   == 0            ? "highest priority" : "next in order");
            if (wifi_sta_up(ap->ssid, ap->password) == ESP_OK) {
                ESP_LOGI(TAG, "wifi: associated to %s", ap->ssid);
                credlist_set_last_used(ap->ssid);
                connected = true;
                break;
            }
            ESP_LOGW(TAG, "wifi: %s failed", ap->ssid);
        }
        if (!connected) {
            ESP_LOGW(TAG, "wifi: %d configured APs %s-- entering AP onboarding",
                     cl.count, filter ? "filtered/failed " : "failed ");
        }
    } else {
        ESP_LOGW(TAG, "wifi: no APs configured -- entering AP onboarding");
    }
    bool ap_mode = !connected;

    if (ap_mode) {
        pt_led_set(PT_LED_AP_ONBOARDING);
        if (wifi_ap_up(AP_SETUP_SSID) != ESP_OK) {
            ESP_LOGE(TAG, "wifi: AP up failed");
            return ESP_FAIL;
        }
        if (http_ap_up() != ESP_OK) {
            ESP_LOGE(TAG, "http: AP server failed");
            return ESP_FAIL;
        }
        /* DNS hijack so phones pop the captive-portal sheet straight
         * onto the setup page -- no manual URL needed. Best effort:
         * onboarding still works if it fails, just less smoothly. */
        if (pt_dns_hijack_start() != ESP_OK) {
            ESP_LOGW(TAG, "dns: hijack start failed -- "
                          "user will need http://192.168.4.1/ manually");
        }
        ESP_LOGI(TAG, "ap: %s up -- connect and visit http://192.168.4.1/",
                 AP_SETUP_SSID);
        /* HTTP server runs forever on its own task; api_setup reboots
         * after it persists the user's chosen creds to NVS. */
        return ESP_OK;
    }

    transport_up(cfg);

    if (mdns_up() == ESP_OK) {
        ESP_LOGI(TAG, "mdns: labelsis.local up");
    } else {
        ESP_LOGW(TAG, "mdns: init failed (use the IP address instead)");
    }

    if (http_up(cfg->http_port) != ESP_OK) {
        ESP_LOGE(TAG, "http: failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "http: serving on port %u", cfg->http_port ? cfg->http_port : 80);
    /* Arm the BOOT button's OTA-gate gesture only now that the device
     * is actually serving requests -- otherwise an early tap could
     * flip the gate before any client could reach /api/ota anyway. */
    s_device_online = true;
    return ESP_OK;
}
