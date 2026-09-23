#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DB_DEVICE_SUFFIX_LEN 7
#define DB_DEVICE_ID_LEN 19
#define DB_AP_SSID_LEN 33

typedef enum { DB_AP_STARTING, DB_AP_ACTIVE, DB_AP_FAILED } db_ap_state_t;
typedef enum {
    DB_STA_UNCONFIGURED,
    DB_STA_CONNECTING,
    DB_STA_CONNECTED,
    DB_STA_DISCONNECTED,
} db_sta_state_t;

bool db_network_identity(const uint8_t mac[6], char *suffix, size_t suffix_size,
                         char *device_id, size_t device_id_size,
                         char *ssid, size_t ssid_size);
bool db_sta_is_configured(const char *ssid);

/* The configured hostname if set, otherwise the lowercased device id. */
bool db_mdns_hostname(const char *configured, const char *device_id, char *out, size_t out_size);

#define DB_STA_FAST_RETRIES 3U
#define DB_STA_BACKOFF_MIN_MS 5000U
#define DB_STA_BACKOFF_MAX_MS 60000U

/* 0 for the first DB_STA_FAST_RETRIES attempts, then doubling up to the max. */
uint32_t db_sta_retry_delay_ms(unsigned attempt);
const char *db_ap_state_name(db_ap_state_t state);
const char *db_sta_state_name(db_sta_state_t state);
