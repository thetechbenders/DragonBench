#include <inttypes.h>
#include <netdb.h>
#include <stdio.h>
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

static esp_err_t events_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/x-ndjson");
    xSemaphoreTake(state_lock, portMAX_DELAY);
    size_t start = (event_head + DB_EVENT_CAPACITY - event_count) % DB_EVENT_CAPACITY;
    for (size_t i = 0; i < event_count; ++i) {
        db_event_t *event = &events[(start + i) % DB_EVENT_CAPACITY];
        httpd_resp_send_chunk(req, event->json, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, "\n", 1);
    }
    xSemaphoreGive(state_lock);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const char landing[] =
"<!doctype html><meta name=viewport content='width=device-width'><title>DragonBench</title>"
"<style>body{font:16px system-ui;max-width:900px;margin:2rem auto;padding:0 1rem;background:#111;color:#eee}.banner{padding:1rem;background:#701;color:white;font-weight:800}pre{background:#222;padding:1rem;overflow:auto}.absent{color:#7f7}</style>"
"<div class=banner>DRAGONBENCH CHARACTERIZATION IMAGE<br>NO PRODUCT ACTUATOR SUPPORT</div>"
"<h1>DragonBench</h1><p class=absent>Heater capability: ABSENT<br>Fan-control capability: ABSENT</p>"
"<p>Target: ESP32-S3 N8R8 · Firmware: " CONFIG_DB_FIRMWARE_VERSION "</p>"
"<h2>Status</h2><pre id=s>loading</pre><h2>Sensors</h2><pre id=n>loading</pre><h2>Workloads</h2><pre id=w>loading</pre>"
"<script>async function g(p){return (await fetch('/api/v1/'+p)).json()}async function u(){s.textContent=JSON.stringify(await g('status'),null,2);n.textContent=JSON.stringify(await g('sensors'),null,2);w.textContent=JSON.stringify(await g('workloads'),null,2)}u();setInterval(u,2000)</script>";

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
