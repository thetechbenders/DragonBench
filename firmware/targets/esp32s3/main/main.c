#include <inttypes.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "db_run.h"
#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "dragonbench"
#define JSON_BODY_MAX 1024
#define IO_BLOCK 4096
#define FLASH_ERASE_BLOCK 4096

static db_run_t current_run;
static db_event_t events[DB_EVENT_CAPACITY];
static size_t event_head;
static size_t event_count;
static uint64_t event_seq;
static SemaphoreHandle_t state_lock;
static bool wifi_connected;
static int wifi_rssi;
static temperature_sensor_handle_t temp_sensor;
static bool temp_available;
static char reset_reason_text[32];
static char previous_reboot_run_id[DB_RUN_ID_LEN];
static uint32_t boot_nonce;
static uint32_t run_counter;
typedef struct { uint32_t magic; char run_id[DB_RUN_ID_LEN]; } reboot_marker_t;
RTC_DATA_ATTR static reboot_marker_t reboot_marker;
#define REBOOT_MARKER_MAGIC 0x44524254U

static uint64_t uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static const char *run_state_name(db_run_state_t state) {
    static const char *const names[] = {"idle", "running", "aborting", "complete"};
    return state <= DB_RUN_COMPLETE ? names[state] : "unknown";
}

static const char *reset_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        default: return "other_or_unknown";
    }
}

static void emit_event(const char *event, const char *phase, const char *run_id,
                       const char *result, const char *parameters_json, const char *metrics_json) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddNumberToObject(root, "seq", (double)++event_seq);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms());
    cJSON_AddStringToObject(root, "target", "esp32s3-n8r8");
    cJSON_AddStringToObject(root, "firmware_version", CONFIG_DB_FIRMWARE_VERSION);
    if (phase) cJSON_AddStringToObject(root, "phase", phase);
    if (run_id && run_id[0]) cJSON_AddStringToObject(root, "run_id", run_id);
    if (result) cJSON_AddStringToObject(root, "result", result);
    if (parameters_json) {
        cJSON *parameters = cJSON_Parse(parameters_json);
        if (parameters) cJSON_AddItemToObject(root, "parameters", parameters);
    }
    if (metrics_json) {
        cJSON *metrics = cJSON_Parse(metrics_json);
        if (metrics) cJSON_AddItemToObject(root, "metrics", metrics);
    }
    char *json = cJSON_PrintUnformatted(root);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    db_event_t *slot = &events[event_head];
    slot->seq = event_seq;
    snprintf(slot->json, sizeof(slot->json), "%s", json ? json : "{}");
    event_head = (event_head + 1U) % DB_EVENT_CAPACITY;
    if (event_count < DB_EVENT_CAPACITY) ++event_count;
    xSemaphoreGive(state_lock);
    ESP_LOGI(TAG, "%s", json ? json : "{}");
    cJSON_free(json);
    cJSON_Delete(root);
}

static bool should_abort(void) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool value = current_run.abort_requested;
    xSemaphoreGive(state_lock);
    return value;
}

static bool wait_abortable(uint32_t duration_ms) {
    const uint64_t deadline = uptime_ms() + duration_ms;
    while (uptime_ms() < deadline) {
        if (should_abort()) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static bool run_cpu(uint32_t duration_ms, uint64_t *operations) {
    volatile uint32_t x = 0x12345678U;
    const uint64_t deadline = uptime_ms() + duration_ms;
    *operations = 0;
    while (uptime_ms() < deadline && !should_abort()) {
        for (int i = 0; i < 4096; ++i) x = (x << 5) ^ (x >> 3) ^ (uint32_t)i;
        *operations += 4096;
        taskYIELD();
    }
    return !should_abort() && x != 0U;
}

static bool run_nvs(uint32_t duration_ms, uint64_t *bytes) {
    nvs_handle_t handle;
    if (nvs_open("db_stress", NVS_READWRITE, &handle) != ESP_OK) return false;
    uint8_t block[512];
    const uint64_t deadline = uptime_ms() + duration_ms;
    *bytes = 0;
    bool ok = true;
    while (uptime_ms() < deadline && !should_abort()) {
        esp_fill_random(block, sizeof(block));
        ok = nvs_set_blob(handle, "payload", block, sizeof(block)) == ESP_OK && nvs_commit(handle) == ESP_OK;
        if (!ok) break;
        *bytes += sizeof(block);
    }
    nvs_erase_key(handle, "payload");
    nvs_commit(handle);
    nvs_close(handle);
    return ok && !should_abort();
}

static bool partition_cycle(const esp_partition_t *partition, uint32_t duration_ms, uint64_t *bytes) {
    if (!partition || partition == esp_ota_get_running_partition()) return false;
    uint8_t write_block[IO_BLOCK], read_block[IO_BLOCK];
    const uint64_t deadline = uptime_ms() + duration_ms;
    size_t offset = 0;
    *bytes = 0;
    while (uptime_ms() < deadline && !should_abort()) {
        if (offset + IO_BLOCK > partition->size) offset = 0;
        if ((offset % FLASH_ERASE_BLOCK) == 0 &&
            esp_partition_erase_range(partition, offset, FLASH_ERASE_BLOCK) != ESP_OK) return false;
        esp_fill_random(write_block, sizeof(write_block));
        if (esp_partition_write(partition, offset, write_block, sizeof(write_block)) != ESP_OK ||
            esp_partition_read(partition, offset, read_block, sizeof(read_block)) != ESP_OK ||
            memcmp(write_block, read_block, sizeof(write_block)) != 0) return false;
        offset += sizeof(write_block);
        *bytes += sizeof(write_block);
        taskYIELD();
    }
    return !should_abort();
}

static int connect_peer(const char *host, uint16_t port) {
    char service[6];
    snprintf(service, sizeof(service), "%u", port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    if (getaddrinfo(host, service, &hints, &result) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *it = result; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd >= 0 && connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        if (fd >= 0) close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

static bool run_network(const db_run_request_t *request, uint64_t *tx, uint64_t *rx) {
    int fd = connect_peer(request->host, request->port);
    if (fd < 0) return false;
    uint8_t block[IO_BLOCK];
    esp_fill_random(block, sizeof(block));
    *tx = *rx = 0;
    const uint64_t deadline = uptime_ms() + request->duration_ms;
    bool ok = true;
    while (uptime_ms() < deadline && !should_abort()) {
        if (request->workload != DB_NET_RX) {
            int n = send(fd, block, sizeof(block), 0);
            if (n <= 0) { ok = false; break; }
            *tx += (uint64_t)n;
        }
        if (request->workload != DB_NET_TX) {
            int n = recv(fd, block, sizeof(block), 0);
            if (n <= 0) { ok = false; break; }
            *rx += (uint64_t)n;
        }
        if (request->rate_bps) {
            uint32_t delay_ms = (uint32_t)((IO_BLOCK * 8ULL * 1000ULL) / request->rate_bps);
            if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return ok && !should_abort();
}

static void workload_task(void *unused) {
    (void)unused;
    db_run_t run;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    run = current_run;
    xSemaphoreGive(state_lock);
    const char *phase = db_workload_name(run.request.workload);
    char parameters[384];
    snprintf(parameters, sizeof(parameters),
             "{\"duration_ms\":%" PRIu32 ",\"rate_bps\":%" PRIu32
             ",\"host\":\"%s\",\"port\":%u}",
             run.request.duration_ms, run.request.rate_bps, run.request.host, run.request.port);
    emit_event("phase_start", phase, run.run_id, NULL, parameters, NULL);
    bool ok = true;
    uint64_t a = 0, b = 0;
    switch (run.request.workload) {
        case DB_BOOT:
        case DB_IDLE:
            ok = wait_abortable(run.request.duration_ms); break;
        case DB_WIFI_ASSOCIATED_IDLE:
            ok = wifi_connected && wait_abortable(run.request.duration_ms); break;
        case DB_NET_TX:
        case DB_NET_RX:
        case DB_NET_BIDIRECTIONAL:
            ok = run_network(&run.request, &a, &b); break;
        case DB_CPU_STRESS:
            ok = run_cpu(run.request.duration_ms, &a); break;
        case DB_FLASH_WRITE: {
            const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "scratch");
            ok = partition_cycle(p, run.request.duration_ms, &a); break;
        }
        case DB_NVS_WRITE:
            ok = run_nvs(run.request.duration_ms, &a); break;
        case DB_OTA_PARTITION_WRITE: {
            const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
            ok = partition_cycle(p, run.request.duration_ms, &a); break;
        }
        case DB_CONTROLLED_REBOOT:
            ok = wait_abortable(run.request.duration_ms); break;
        default: ok = false; break;
    }
    const char *result = should_abort() ? "aborted" : (ok ? "pass" : "fail");
    char metrics[128];
    snprintf(metrics, sizeof(metrics), "{\"operations_or_bytes\":%" PRIu64 ",\"bytes_rx\":%" PRIu64 "}", a, b);
    if (!ok && !should_abort()) emit_event("fault", phase, run.run_id, "fail", parameters, metrics);
    emit_event("phase_end", phase, run.run_id, result, parameters, metrics);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    db_run_finish(&current_run, result, uptime_ms());
    xSemaphoreGive(state_lock);
    emit_event("run_complete", phase, run.run_id, result, parameters, metrics);
    if (run.request.workload == DB_CONTROLLED_REBOOT && ok && !should_abort()) {
        reboot_marker.magic = REBOOT_MARKER_MAGIC;
        snprintf(reboot_marker.run_id, sizeof(reboot_marker.run_id), "%s", run.run_id);
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }
    vTaskDelete(NULL);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status) {
    httpd_resp_set_status(req, status == 200 ? "200 OK" : status == 201 ? "201 Created" : status == 400 ? "400 Bad Request" : status == 404 ? "404 Not Found" : "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    char *text = cJSON_PrintUnformatted(root);
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

static cJSON *identity_json(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "product", "DragonBench");
    cJSON_AddStringToObject(o, "target", "esp32s3-n8r8");
    cJSON_AddStringToObject(o, "firmware_version", CONFIG_DB_FIRMWARE_VERSION);
    cJSON_AddStringToObject(o, "image_class", "characterization");
    cJSON_AddBoolToObject(o, "heater_capability", false);
    cJSON_AddBoolToObject(o, "fan_control_capability", false);
    cJSON_AddStringToObject(o, "measurement_authority", "external_bench_equipment");
    return o;
}

static esp_err_t device_get(httpd_req_t *req) { return send_json(req, identity_json(), 200); }

static esp_err_t status_get(httpd_req_t *req) {
    cJSON *o = identity_json();
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cJSON_AddStringToObject(o, "run_id", current_run.run_id);
    cJSON_AddStringToObject(o, "last_run_id", current_run.run_id);
    cJSON_AddStringToObject(o, "run_state", run_state_name(current_run.state));
    cJSON_AddStringToObject(o, "workload", db_workload_name(current_run.request.workload));
    cJSON_AddStringToObject(o, "result", current_run.result);
    xSemaphoreGive(state_lock);
    cJSON_AddNumberToObject(o, "uptime_ms", (double)uptime_ms());
    cJSON_AddStringToObject(o, "reset_reason", reset_reason_text);
    if (previous_reboot_run_id[0]) cJSON_AddStringToObject(o, "previous_reboot_run_id", previous_reboot_run_id);
    cJSON_AddBoolToObject(o, "network_connected", wifi_connected);
    return send_json(req, o, 200);
}

static void add_sensor(cJSON *a, const char *id, const char *name, const char *unit,
                       const char *source, const char *status, double value, bool include_value) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id); cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "unit", unit); cJSON_AddStringToObject(o, "source", source);
    cJSON_AddStringToObject(o, "status", status);
    if (include_value) cJSON_AddNumberToObject(o, "value", value);
    cJSON_AddItemToArray(a, o);
}

static esp_err_t sensors_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(root, "sensors");
    float temperature = 0;
    bool temp_ok = temp_available && temperature_sensor_get_celsius(temp_sensor, &temperature) == ESP_OK;
    add_sensor(a, "soc_temperature", "SoC temperature", "degC", "on_die", temp_ok ? "available" : "unavailable", temperature, temp_ok);
    add_sensor(a, "wifi_rssi", "Wi-Fi RSSI", "dBm", "wifi", wifi_connected ? "available" : "unavailable", wifi_rssi, wifi_connected);
    add_sensor(a, "supply_voltage", "MCU supply voltage", "V", "none", "unsupported", 0, false);
    add_sensor(a, "supply_current", "MCU supply current", "A", "none", "unsupported", 0, false);
    return send_json(req, root, 200);
}

static esp_err_t workloads_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(root, "workloads");
    for (int i = 0; i < DB_WORKLOAD_COUNT; ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", db_workload_name((db_workload_t)i));
        cJSON_AddStringToObject(o, "status", db_workload_supported((db_workload_t)i) ? "available" : "unsupported");
        cJSON_AddItemToArray(a, o);
    }
    static const char *const unsupported[] = {
        "BLE_STRESS", "GENERIC_PERIPHERAL_LOAD", "CRYPTO_BENCHMARK",
        "FILESYSTEM_BENCHMARK", "REBOOT_LOOP"
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", unsupported[i]);
        cJSON_AddStringToObject(o, "status", "unsupported");
        cJSON_AddItemToArray(a, o);
    }
    return send_json(req, root, 200);
}

static cJSON *read_body(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len >= JSON_BODY_MAX) return NULL;
    char body[JSON_BODY_MAX];
    size_t total = 0;
    while (total < (size_t)req->content_len) {
        int got = httpd_req_recv(req, body + total, req->content_len - total);
        if (got <= 0) return NULL;
        total += (size_t)got;
    }
    body[total] = '\0';
    return cJSON_Parse(body);
}

static esp_err_t runs_post(httpd_req_t *req) {
    cJSON *body = read_body(req);
    cJSON *w = body ? cJSON_GetObjectItemCaseSensitive(body, "workload") : NULL;
    cJSON *duration = body ? cJSON_GetObjectItemCaseSensitive(body, "duration_ms") : NULL;
    db_run_request_t request = {.duration_ms = 30000};
    bool parsed = cJSON_IsString(w) && db_workload_parse(w->valuestring, &request.workload);
    if (cJSON_IsNumber(duration)) {
        if (duration->valuedouble < 1 || duration->valuedouble > 3600000 ||
            duration->valuedouble != (double)(uint32_t)duration->valuedouble) parsed = false;
        else request.duration_ms = (uint32_t)duration->valuedouble;
    }
    cJSON *host = body ? cJSON_GetObjectItemCaseSensitive(body, "host") : NULL;
    cJSON *port = body ? cJSON_GetObjectItemCaseSensitive(body, "port") : NULL;
    cJSON *rate = body ? cJSON_GetObjectItemCaseSensitive(body, "rate_bps") : NULL;
    if (cJSON_IsString(host) && strlen(host->valuestring) < sizeof(request.host)) snprintf(request.host, sizeof(request.host), "%s", host->valuestring);
    if (cJSON_IsNumber(port)) {
        if (port->valuedouble < 1 || port->valuedouble > 65535 ||
            port->valuedouble != (double)(uint16_t)port->valuedouble) parsed = false;
        else request.port = (uint16_t)port->valueint;
    }
    if (cJSON_IsNumber(rate)) {
        if (rate->valuedouble < 0 || rate->valuedouble > UINT32_MAX ||
            rate->valuedouble != (double)(uint32_t)rate->valuedouble) parsed = false;
        else request.rate_bps = (uint32_t)rate->valuedouble;
    }
    char validation[96] = "invalid workload";
    bool valid = parsed && db_request_validate(&request, validation, sizeof(validation));
    cJSON_Delete(body);
    if (!valid) { cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "error", validation); return send_json(req, o, 400); }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (current_run.state == DB_RUN_RUNNING || current_run.state == DB_RUN_ABORTING) {
        xSemaphoreGive(state_lock); cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "error", "run already active"); return send_json(req, o, 409);
    }
    char id[DB_RUN_ID_LEN];
    snprintf(id, sizeof(id), "%08" PRIx32 "-%08" PRIx32, boot_nonce, ++run_counter);
    db_run_begin(&current_run, &request, id, uptime_ms());
    xSemaphoreGive(state_lock);
    if (xTaskCreate(workload_task, "db_workload", 8192, NULL, 5, NULL) != pdPASS) {
        xSemaphoreTake(state_lock, portMAX_DELAY); db_run_finish(&current_run, "fail", uptime_ms()); xSemaphoreGive(state_lock);
        cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "error", "task creation failed"); return send_json(req, o, 409);
    }
    cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "run_id", id); cJSON_AddStringToObject(o, "state", "running");
    return send_json(req, o, 201);
}

static const char *path_run_id(httpd_req_t *req, bool abort_path, char *id, size_t size) {
    const char *prefix = "/api/v1/runs/";
    const char *start = req->uri + strlen(prefix);
    const char *end = abort_path ? strstr(start, "/abort") : start + strlen(start);
    if (!end || end == start || (size_t)(end - start) >= size) return NULL;
    memcpy(id, start, (size_t)(end - start)); id[end - start] = '\0'; return id;
}

static esp_err_t run_get(httpd_req_t *req) {
    char id[DB_RUN_ID_LEN];
    if (!path_run_id(req, false, id, sizeof(id))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad run id");
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool match = strcmp(id, current_run.run_id) == 0;
    cJSON *o = cJSON_CreateObject();
    if (match) { cJSON_AddStringToObject(o, "run_id", id); cJSON_AddStringToObject(o, "workload", db_workload_name(current_run.request.workload)); cJSON_AddStringToObject(o, "state", run_state_name(current_run.state)); cJSON_AddStringToObject(o, "result", current_run.result); }
    xSemaphoreGive(state_lock);
    if (!match) { cJSON_AddStringToObject(o, "error", "run not found"); return send_json(req, o, 404); }
    return send_json(req, o, 200);
}

static esp_err_t abort_post(httpd_req_t *req) {
    char id[DB_RUN_ID_LEN];
    if (!path_run_id(req, true, id, sizeof(id))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad run id");
    xSemaphoreTake(state_lock, portMAX_DELAY); bool ok = db_run_abort(&current_run, id); xSemaphoreGive(state_lock);
    cJSON *o = cJSON_CreateObject();
    if (!ok) { cJSON_AddStringToObject(o, "error", "run not active"); return send_json(req, o, 409); }
    cJSON_AddStringToObject(o, "run_id", id); cJSON_AddStringToObject(o, "state", "aborting"); return send_json(req, o, 200);
}

// One /api/v1/events response: a copy of the event ring taken under state_lock
// and transmitted only after the lock is released. HTTP sends can block for the
// socket send timeout, and state_lock is also taken by should_abort() in every
// workload loop, so holding it across the network would let a slow or dead
// client stall running workloads, aborts and event producers.
typedef struct {
    size_t count;
    char json[DB_EVENT_CAPACITY][DB_EVENT_JSON_LEN];
} event_snapshot_t;

// Caller holds state_lock. Copies the ring oldest-first, as one coherent view.
static void snapshot_events_locked(event_snapshot_t *snapshot) {
    size_t start = (event_head + DB_EVENT_CAPACITY - event_count) % DB_EVENT_CAPACITY;
    snapshot->count = event_count;
    for (size_t i = 0; i < event_count; ++i) {
        const char *src = events[(start + i) % DB_EVENT_CAPACITY].json;
        const char *nul = memchr(src, '\0', DB_EVENT_JSON_LEN - 1);
        size_t len = nul ? (size_t)(nul - src) : DB_EVENT_JSON_LEN - 1;
        memcpy(snapshot->json[i], src, len);
        snapshot->json[i][len] = '\0';
    }
}

static esp_err_t events_get(httpd_req_t *req) {
    // Bounded: DB_EVENT_CAPACITY * DB_EVENT_JSON_LEN (~32 KiB), too large for
    // the httpd task stack; released before returning on every path.
    event_snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"event snapshot unavailable\"}");
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    snapshot_events_locked(snapshot);
    xSemaphoreGive(state_lock);

    httpd_resp_set_type(req, "application/x-ndjson");
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < snapshot->count && err == ESP_OK; ++i) {
        err = httpd_resp_send_chunk(req, snapshot->json[i], HTTPD_RESP_USE_STRLEN);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, "\n", 1);
    }
    free(snapshot);
    // A failed send means the client is gone; stop instead of waiting out the
    // send timeout for every remaining chunk.
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const char landing[] =
"<!doctype html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<meta name=\"color-scheme\" content=\"light dark\">"
"<title>DragonBench</title>"
"<style>"
":root{"
"  color-scheme:light dark;"
"  --bg:light-dark(#fff,#181818);"
"  --fg:light-dark(#1a1c1f,#fff);"
"  --card:color-mix(in oklab, var(--fg) 5%, transparent);"
"  --muted-fg:light-dark(rgba(26,28,31,.58),rgba(255,255,255,.58));"
"  --border:light-dark(rgba(26,28,31,.1),rgba(255,255,255,.1));"
"  --code:light-dark(#f6f7f8,#0e0f10);"
"  --primary:light-dark(#339cff,#83c3ff);"
"  --ok:light-dark(#2d9a4f,#74d58b);"
"  --warn:light-dark(#cc671a,#f59a56);"
"  --bad:light-dark(#c43827,#ff8549);"
"  --radius:6px;"
"  --radius-sm:4px;"
"}"
"*{box-sizing:border-box}"
"[hidden]{display:none!important}"
"body{margin:0;min-height:100vh;color:var(--fg);background:var(--bg);font:14px/1.4 -apple-system,system-ui,\"Segoe UI\",Roboto,sans-serif;padding:0 16px}"
"header,main,footer,.banner,.capabilities{width:min(1100px,100%);margin-inline:auto}"
".app-header{display:flex;justify-content:space-between;gap:24px;align-items:end;flex-wrap:wrap;padding:20px 0 14px}"
".eyebrow{color:var(--primary);font-size:.72rem;font-weight:700;letter-spacing:.1em;text-transform:uppercase;margin:0 0 4px;font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
"h1{margin:0 0 4px;font-size:clamp(1.7rem,4vw,2.4rem);letter-spacing:-.03em;line-height:1}"
".lede{margin:0;color:var(--muted-fg)}"
".api-status{display:grid;justify-items:end;gap:4px}"
".api-status>span{color:var(--primary);font-size:.64rem;font-weight:700;letter-spacing:.09em;text-transform:uppercase;font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
"h2{font-size:1.05rem;margin:0 0 6px}"
".banner{margin-bottom:12px;border:1px solid var(--border);border-left:4px solid var(--warn);border-radius:var(--radius);background:var(--card);box-shadow:0 1px 3px light-dark(rgba(0,0,0,.08),rgba(0,0,0,.3));padding:12px 14px}"
".banner h2{font-size:.95rem;letter-spacing:.02em;text-transform:uppercase}"
".banner p{margin:6px 0 0;color:var(--muted-fg)}"
".capabilities{margin-bottom:12px;display:flex;gap:16px;flex-wrap:wrap;border:1px solid var(--border);border-radius:var(--radius);background:var(--card);box-shadow:0 1px 3px light-dark(rgba(0,0,0,.08),rgba(0,0,0,.3));padding:12px 14px}"
".capability{display:flex;align-items:baseline;gap:8px}"
".capability>span{color:var(--muted-fg);font-size:.8rem}"
".capability>strong{--status-color:var(--warn);font:700 .78rem ui-monospace,SFMono-Regular,Consolas,monospace;padding:2px 9px;border-radius:var(--radius-sm);border:1px solid var(--border);color:var(--status-color);background:color-mix(in srgb,var(--status-color) 10%,transparent)}"
"main{display:grid;gap:12px;padding-bottom:20px}"
".panel{border:1px solid var(--border);border-radius:var(--radius);background:var(--card);box-shadow:0 1px 3px light-dark(rgba(0,0,0,.08),rgba(0,0,0,.3));padding:14px}"
".panel-heading{display:flex;justify-content:space-between;align-items:start;gap:12px;margin-bottom:8px}"
".endpoint-note{margin:0;color:var(--bad);font-size:.8rem}"
".metric-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}"
".metric{display:grid;gap:4px;min-width:0;padding:8px 9px;border:1px solid var(--border);border-radius:var(--radius-sm);box-shadow:0 1px 2px light-dark(rgba(0,0,0,.06),rgba(0,0,0,.24))}"
".metric span{color:var(--muted-fg);font-size:.68rem;text-transform:uppercase;letter-spacing:.07em}"
".metric strong{font:700 .85rem ui-monospace,SFMono-Regular,Consolas,monospace;overflow-wrap:anywhere}"
".sensor-list{display:flex;flex-direction:column;gap:7px}"
".sensor-row{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:7px 10px;border:1px solid var(--border);border-radius:var(--radius-sm);flex-wrap:wrap}"
".sensor-name{color:var(--muted-fg)}"
".sensor-value{font:700 .85rem ui-monospace,SFMono-Regular,Consolas,monospace;overflow-wrap:anywhere}"
".workload-list{display:flex;flex-direction:row;flex-wrap:wrap;gap:7px}"
".badge{--status-color:var(--muted-fg);display:inline-flex;align-items:center;gap:6px;border:1px solid var(--border);border-radius:var(--radius-sm);padding:4px 9px;color:var(--status-color);background:color-mix(in srgb,var(--status-color) 10%,transparent);font-size:.76rem;white-space:nowrap}"
".badge::before{content:\"\";width:6px;height:6px;border-radius:50%;background:var(--status-color)}"
"[data-status=\"available\"],[data-status=\"pass\"],[data-status=\"complete\"],[data-status=\"connected\"]{--status-color:var(--ok)}"
"[data-status=\"unavailable\"],[data-status=\"unsupported\"],[data-status=\"aborting\"],[data-status=\"aborted\"],[data-status=\"stale\"],[data-status=\"disconnected\"]{--status-color:var(--warn)}"
"[data-status=\"error\"],[data-status=\"fail\"]{--status-color:var(--bad)}"
"[data-status=\"running\"],[data-status=\"connecting\"],[data-status=\"live\"]{--status-color:var(--primary)}"
"strong[data-status]{color:var(--status-color)}"
"details{border-top:1px solid var(--border);padding-top:8px;margin-top:8px}"
"summary{cursor:pointer;color:var(--muted-fg);font-size:.78rem;width:fit-content;font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
"pre{margin:8px 0 0;max-height:320px;overflow:auto;background:var(--code);border:1px solid var(--border);border-radius:var(--radius-sm);padding:10px;font:.72rem/1.5 ui-monospace,SFMono-Regular,Consolas,monospace;white-space:pre}"
"footer{padding:16px 0 24px;color:var(--muted-fg);text-align:center;font-size:.76rem}"
"@media (max-width:900px){.metric-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}"
"@media (max-width:620px){.app-header{flex-direction:column;align-items:start}.api-status{justify-items:start}.metric-grid{grid-template-columns:1fr}.sensor-row{flex-direction:column;align-items:start}}"
"@media (prefers-reduced-motion:reduce){*{transition-duration:0s!important;animation-duration:0s!important}}"
"</style>"
"</head>"
"<body>"
"<header class=\"app-header\">"
"  <div>"
"    <p class=\"eyebrow\">Dragon-family characterization instrument</p>"
"    <h1>DragonBench</h1>"
"    <p class=\"lede\" data-role=\"identity\">Target: esp32s3-n8r8 &middot; Firmware: " CONFIG_DB_FIRMWARE_VERSION "</p>"
"  </div>"
"  <div class=\"api-status\" aria-label=\"API connection state\">"
"    <span>API</span>"
"    <div class=\"badge\" data-role=\"freshness\" data-status=\"connecting\">connecting&hellip;</div>"
"  </div>"
"</header>"
"<section class=\"banner\" data-role=\"actuator-boundary\" aria-labelledby=\"actuator-boundary-heading\">"
"  <h2 id=\"actuator-boundary-heading\">DragonBench characterization image</h2>"
"  <p><strong>No product actuator support.</strong> This image cannot drive a heater or fan. All voltage, current, and rail evidence remains owned by external bench equipment.</p>"
"</section>"
"<section class=\"capabilities\" aria-label=\"Actuator capability boundary\">"
"  <div class=\"capability\">"
"    <span>Heater capability</span>"
"    <strong data-role=\"capability-heater\" data-status=\"unsupported\">ABSENT</strong>"
"  </div>"
"  <div class=\"capability\">"
"    <span>Fan-control capability</span>"
"    <strong data-role=\"capability-fan\" data-status=\"unsupported\">ABSENT</strong>"
"  </div>"
"</section>"
"<noscript><p style=\"width:min(1100px,100%);margin:0 auto 16px\">JavaScript is required to populate live values. The DragonBench API remains directly reachable at /api/v1/status, /api/v1/sensors, and /api/v1/workloads.</p></noscript>"
"<main>"
"  <section class=\"panel\" data-role=\"panel-status\" aria-labelledby=\"status-heading\">"
"    <div class=\"panel-heading\">"
"      <h2 id=\"status-heading\">Status</h2>"
"      <p class=\"endpoint-note\" data-role=\"status-error\" hidden></p>"
"    </div>"
"    <div class=\"metric-grid\" data-role=\"status-fields\"></div>"
"    <details><summary>Raw /api/v1/status</summary><pre data-role=\"raw-status\">unavailable</pre></details>"
"  </section>"
"  <section class=\"panel\" data-role=\"panel-sensors\" aria-labelledby=\"sensors-heading\">"
"    <div class=\"panel-heading\">"
"      <h2 id=\"sensors-heading\">Sensors</h2>"
"      <p class=\"endpoint-note\" data-role=\"sensors-error\" hidden></p>"
"    </div>"
"    <div class=\"sensor-list\" data-role=\"sensor-fields\"></div>"
"    <details><summary>Raw /api/v1/sensors</summary><pre data-role=\"raw-sensors\">unavailable</pre></details>"
"  </section>"
"  <section class=\"panel\" data-role=\"panel-workloads\" aria-labelledby=\"workloads-heading\">"
"    <div class=\"panel-heading\">"
"      <h2 id=\"workloads-heading\">Workloads</h2>"
"      <p class=\"endpoint-note\" data-role=\"workloads-error\" hidden></p>"
"    </div>"
"    <div class=\"workload-list\" data-role=\"workload-fields\"></div>"
"    <details><summary>Raw /api/v1/workloads</summary><pre data-role=\"raw-workloads\">unavailable</pre></details>"
"  </section>"
"</main>"
"<footer>DragonBench characterizes deterministic workloads; it does not control product behavior.</footer>"
"<script>"
"(function(){"
"  var ENDPOINTS=['status','sensors','workloads'];"
"  var STALE_MS=5000;"
"  var last={};"
"  function role(name){return document.querySelector('[data-role=\"'+name+'\"]')}"
"  function clear(el){while(el.firstChild)el.removeChild(el.firstChild)}"
"  function text(value){return value===undefined||value===null||value===''?'—':String(value)}"
"  function fmtUptime(ms){"
"    if(typeof ms!=='number')return '—';"
"    var s=Math.floor(ms/1000),h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;"
"    return h+'h '+m+'m '+sec+'s';"
"  }"
"  function metricTile(label,value,status){"
"    var art=document.createElement('article');art.className='metric';"
"    var span=document.createElement('span');span.textContent=label;art.appendChild(span);"
"    var strong=document.createElement('strong');strong.textContent=text(value);"
"    if(status)strong.dataset.status=status;"
"    art.appendChild(strong);"
"    return art;"
"  }"
"  function renderStatus(data){"
"    var grid=role('status-fields');clear(grid);"
"    grid.appendChild(metricTile('Target',data.target));"
"    grid.appendChild(metricTile('Firmware',data.firmware_version));"
"    grid.appendChild(metricTile('Image class',data.image_class));"
"    grid.appendChild(metricTile('Run state',data.run_state,data.run_state));"
"    grid.appendChild(metricTile('Workload',data.workload));"
"    grid.appendChild(metricTile('Result',data.result,data.result));"
"    grid.appendChild(metricTile('Uptime',fmtUptime(data.uptime_ms)));"
"    grid.appendChild(metricTile('Reset reason',data.reset_reason));"
"    grid.appendChild(metricTile('Previous reboot run',data.previous_reboot_run_id));"
"    var net=data.network_connected;"
"    grid.appendChild(metricTile('Network',net===true?'connected':net===false?'disconnected':undefined,net===true?'connected':net===false?'disconnected':undefined));"
"    grid.appendChild(metricTile('Run ID',data.run_id));"
"    grid.appendChild(metricTile('Measurement authority',data.measurement_authority));"
"    var heater=role('capability-heater'),fan=role('capability-fan');"
"    if(typeof data.heater_capability==='boolean'){heater.textContent=data.heater_capability?'PRESENT':'ABSENT';heater.dataset.status=data.heater_capability?'error':'unsupported'}"
"    if(typeof data.fan_control_capability==='boolean'){fan.textContent=data.fan_control_capability?'PRESENT':'ABSENT';fan.dataset.status=data.fan_control_capability?'error':'unsupported'}"
"    role('identity').textContent=(data.target||'target —')+' · firmware '+(data.firmware_version||'—');"
"  }"
"  function renderSensors(data){"
"    var list=role('sensor-fields');clear(list);"
"    (data.sensors||[]).forEach(function(s){"
"      var row=document.createElement('div');row.className='sensor-row';row.dataset.sensorId=s.id||'';"
"      var name=document.createElement('span');name.className='sensor-name';name.textContent=s.name||s.id||'sensor';"
"      var value=document.createElement('strong');value.className='sensor-value';"
"      value.textContent=(s.value!==undefined&&s.value!==null)?(s.value+(s.unit?(' '+s.unit):'')):'—';"
"      var badge=document.createElement('span');badge.className='badge';badge.dataset.status=s.status||'unavailable';badge.textContent=s.status||'unavailable';"
"      row.appendChild(name);row.appendChild(value);row.appendChild(badge);"
"      list.appendChild(row);"
"    });"
"  }"
"  function renderWorkloads(data){"
"    var list=role('workload-fields');clear(list);"
"    (data.workloads||[]).forEach(function(w){"
"      var chip=document.createElement('span');chip.className='badge';chip.dataset.status=w.status||'unsupported';"
"      chip.textContent=(w.id||'workload')+' · '+(w.status||'unsupported');"
"      list.appendChild(chip);"
"    });"
"  }"
"  var renderers={status:renderStatus,sensors:renderSensors,workloads:renderWorkloads};"
"  function setError(name,message){"
"    var note=role(name+'-error');if(!note)return;"
"    if(message){note.hidden=false;note.textContent=message}else{note.hidden=true;note.textContent=''}"
"  }"
"  function poll(name){"
"    return fetch('/api/v1/'+name,{cache:'no-store'}).then(function(res){"
"      if(!res.ok)throw new Error('HTTP '+res.status);"
"      return res.text();"
"    }).then(function(text){"
"      var data;"
"      try{data=JSON.parse(text)}catch(e){throw new Error('malformed JSON')}"
"      role('raw-'+name).textContent=JSON.stringify(data,null,2);"
"      renderers[name](data);"
"      setError(name,null);"
"      last[name]=Date.now();"
"    }).catch(function(err){"
"      setError(name,'unreachable: '+err.message);"
"    });"
"  }"
"  function pollAll(){Promise.allSettled(ENDPOINTS.map(poll)).then(updateFreshness)}"
"  function updateFreshness(){"
"    var badge=role('freshness');"
"    var any=ENDPOINTS.some(function(n){return last[n]});"
"    if(!any){badge.textContent='unreachable';badge.dataset.status='error';return}"
"    var worst=Math.max.apply(null,ENDPOINTS.map(function(n){return last[n]?Date.now()-last[n]:Infinity}));"
"    if(worst>STALE_MS*3){badge.textContent='stale';badge.dataset.status='error'}"
"    else if(worst>STALE_MS){badge.textContent='stale';badge.dataset.status='stale'}"
"    else{badge.textContent='live';badge.dataset.status='live'}"
"  }"
"  pollAll();"
"  setInterval(pollAll,2000);"
"  setInterval(updateFreshness,1000);"
"})();"
"</script>"
"</body>"
"</html>";

static esp_err_t landing_get(httpd_req_t *req) { httpd_resp_set_type(req, "text/html"); return httpd_resp_send(req, landing, HTTPD_RESP_USE_STRLEN); }

static void start_http(void) {
    const httpd_uri_t routes[] = {
        {.uri="/",.method=HTTP_GET,.handler=landing_get}, {.uri="/api/v1/device",.method=HTTP_GET,.handler=device_get},
        {.uri="/api/v1/status",.method=HTTP_GET,.handler=status_get}, {.uri="/api/v1/sensors",.method=HTTP_GET,.handler=sensors_get},
        {.uri="/api/v1/workloads",.method=HTTP_GET,.handler=workloads_get}, {.uri="/api/v1/runs",.method=HTTP_POST,.handler=runs_post},
        {.uri="/api/v1/runs/*/abort",.method=HTTP_POST,.handler=abort_post}, {.uri="/api/v1/runs/*",.method=HTTP_GET,.handler=run_get},
        {.uri="/api/v1/events",.method=HTTP_GET,.handler=events_get},
    };
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = sizeof(routes) / sizeof(routes[0]);
    httpd_handle_t server = NULL; ESP_ERROR_CHECK(httpd_start(&server, &config));
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && CONFIG_DB_WIFI_SSID[0] != '\0') esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (CONFIG_DB_WIFI_SSID[0] != '\0') esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
        wifi_ap_record_t record;
        if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) wifi_rssi = record.rssi;
    }
}

static void start_wifi(void) {
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default()); esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_instance_t any, got;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, &any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, &got));
    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", CONFIG_DB_WIFI_SSID);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", CONFIG_DB_WIFI_PASSWORD);
    config.sta.threshold.authmode = CONFIG_DB_WIFI_PASSWORD[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config)); ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    state_lock = xSemaphoreCreateMutex();
    boot_nonce = esp_random();
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason != ESP_RST_SW) reboot_marker.magic = 0;
    if (reboot_marker.magic == REBOOT_MARKER_MAGIC) {
        snprintf(previous_reboot_run_id, sizeof(previous_reboot_run_id), "%s", reboot_marker.run_id);
        reboot_marker.magic = 0;
    }
    snprintf(reset_reason_text, sizeof(reset_reason_text), "%s", reset_name(reset_reason));
    current_run.state = DB_RUN_IDLE; current_run.request.workload = DB_IDLE; snprintf(current_run.result, sizeof(current_run.result), "none");
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    temp_available = temperature_sensor_install(&tc, &temp_sensor) == ESP_OK && temperature_sensor_enable(temp_sensor) == ESP_OK;
    char reset_metrics[64];
    snprintf(reset_metrics, sizeof(reset_metrics), "{\"reason\":\"%s\"}", reset_reason_text);
    const char *prior_run = previous_reboot_run_id[0] ? previous_reboot_run_id : NULL;
    emit_event("boot", NULL, prior_run, NULL, NULL, NULL);
    emit_event("reset_reason", NULL, prior_run, NULL, NULL, reset_metrics);
    start_wifi();
    ESP_ERROR_CHECK(mdns_init()); ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_DB_HOSTNAME)); ESP_ERROR_CHECK(mdns_instance_name_set("DragonBench characterization harness"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    start_http(); emit_event("ready", NULL, prior_run, NULL, NULL, NULL);
}
