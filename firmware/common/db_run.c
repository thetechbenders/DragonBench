#include "db_run.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const names[DB_WORKLOAD_COUNT] = {
    "BOOT", "IDLE", "WIFI_ASSOCIATED_IDLE", "NET_TX", "NET_RX",
    "NET_BIDIRECTIONAL", "CPU_STRESS", "FLASH_WRITE", "NVS_WRITE",
    "OTA_PARTITION_WRITE", "CONTROLLED_REBOOT"
};

const char *db_workload_name(db_workload_t workload) {
    return workload >= DB_BOOT && workload < DB_WORKLOAD_COUNT ? names[workload] : "UNKNOWN";
}

bool db_workload_parse(const char *name, db_workload_t *out) {
    if (!name || !out) return false;
    for (int i = 0; i < DB_WORKLOAD_COUNT; ++i) {
        if (strcmp(name, names[i]) == 0) {
            *out = (db_workload_t)i;
            return true;
        }
    }
    return false;
}

bool db_workload_supported(db_workload_t workload) {
    return workload >= DB_BOOT && workload < DB_WORKLOAD_COUNT;
}

bool db_request_validate(const db_run_request_t *request, char *error, size_t error_len) {
    if (!request || !db_workload_supported(request->workload)) {
        snprintf(error, error_len, "unsupported workload");
        return false;
    }
    if (request->duration_ms == 0 || request->duration_ms > 3600000U) {
        snprintf(error, error_len, "duration_ms must be 1..3600000");
        return false;
    }
    const bool network = request->workload == DB_NET_TX || request->workload == DB_NET_RX ||
                         request->workload == DB_NET_BIDIRECTIONAL;
    if (network && (request->host[0] == '\0' || request->port == 0)) {
        snprintf(error, error_len, "network workloads require host and port");
        return false;
    }
    if (network) {
        for (const unsigned char *p = (const unsigned char *)request->host; *p; ++p) {
            if (!(isalnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == ':')) {
                snprintf(error, error_len, "host contains unsupported characters");
                return false;
            }
        }
    }
    return true;
}

void db_run_begin(db_run_t *run, const db_run_request_t *request, const char *run_id, uint64_t now_ms) {
    memset(run, 0, sizeof(*run));
    run->request = *request;
    snprintf(run->run_id, sizeof(run->run_id), "%s", run_id);
    run->state = DB_RUN_RUNNING;
    run->started_ms = now_ms;
    snprintf(run->result, sizeof(run->result), "running");
}

bool db_run_abort(db_run_t *run, const char *run_id) {
    if (!run || !run_id || run->state != DB_RUN_RUNNING || strcmp(run->run_id, run_id) != 0) return false;
    run->abort_requested = true;
    run->state = DB_RUN_ABORTING;
    return true;
}

void db_run_finish(db_run_t *run, const char *result, uint64_t now_ms) {
    run->state = DB_RUN_COMPLETE;
    run->ended_ms = now_ms;
    snprintf(run->result, sizeof(run->result), "%s", result ? result : "fail");
}
