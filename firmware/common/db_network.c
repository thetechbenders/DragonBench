#include "db_network.h"

#include <stdio.h>

bool db_network_identity(const uint8_t mac[6], char *suffix, size_t suffix_size,
                         char *device_id, size_t device_id_size,
                         char *ssid, size_t ssid_size) {
    if (!mac || !suffix || !device_id || !ssid ||
        suffix_size < DB_DEVICE_SUFFIX_LEN || device_id_size < DB_DEVICE_ID_LEN ||
        ssid_size < DB_AP_SSID_LEN) return false;
    int a = snprintf(suffix, suffix_size, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    int b = snprintf(device_id, device_id_size, "dragonbench-%s", suffix);
    int c = snprintf(ssid, ssid_size, "DragonBench-%s", suffix);
    return a == DB_DEVICE_SUFFIX_LEN - 1 && b > 0 && (size_t)b < device_id_size &&
           c > 0 && (size_t)c < ssid_size;
}

bool db_sta_is_configured(const char *ssid) { return ssid && ssid[0] != '\0'; }

const char *db_ap_state_name(db_ap_state_t state) {
    static const char *const names[] = {"starting", "active", "failed"};
    return state <= DB_AP_FAILED ? names[state] : "unknown";
}

const char *db_sta_state_name(db_sta_state_t state) {
    static const char *const names[] = {"unconfigured", "connecting", "connected", "disconnected"};
    return state <= DB_STA_DISCONNECTED ? names[state] : "unknown";
}
