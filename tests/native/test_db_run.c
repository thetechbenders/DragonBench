#include "db_run.h"
#include "db_network.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

int main(void) {
    const uint8_t mac[] = {0x3c, 0x0f, 0x02, 0xe2, 0xa1, 0x2c};
    char suffix[DB_DEVICE_SUFFIX_LEN];
    char device_id[DB_DEVICE_ID_LEN];
    char ssid[DB_AP_SSID_LEN];
    assert(db_network_identity(mac, suffix, sizeof(suffix), device_id, sizeof(device_id), ssid, sizeof(ssid)));
    assert(strcmp(suffix, "E2A12C") == 0);
    assert(strcmp(device_id, "dragonbench-E2A12C") == 0);
    assert(strcmp(ssid, "DragonBench-E2A12C") == 0);
    assert(!db_network_identity(NULL, suffix, sizeof(suffix), device_id, sizeof(device_id), ssid, sizeof(ssid)));
    assert(!db_network_identity(mac, suffix, sizeof(suffix) - 1, device_id, sizeof(device_id), ssid, sizeof(ssid)));
    assert(!db_sta_is_configured(NULL));
    assert(!db_sta_is_configured(""));
    assert(db_sta_is_configured("synthetic-lab-network"));
    assert(strcmp(db_ap_state_name(DB_AP_STARTING), "starting") == 0);
    assert(strcmp(db_ap_state_name(DB_AP_ACTIVE), "active") == 0);
    assert(strcmp(db_ap_state_name(DB_AP_FAILED), "failed") == 0);
    assert(strcmp(db_sta_state_name(DB_STA_UNCONFIGURED), "unconfigured") == 0);
    static const uint32_t expected_delays[] = {0, 0, 0, 5000, 10000, 20000, 40000, 60000, 60000};
    for (unsigned i = 0; i < sizeof(expected_delays) / sizeof(expected_delays[0]); ++i)
        assert(db_sta_retry_delay_ms(i) == expected_delays[i]);
    assert(db_sta_retry_delay_ms(UINT_MAX) == DB_STA_BACKOFF_MAX_MS);

    db_workload_t workload = DB_WORKLOAD_COUNT;
    assert(db_workload_parse("CPU_STRESS", &workload));
    assert(workload == DB_CPU_STRESS);
    assert(!db_workload_parse("BLE_STRESS", &workload));
    assert(!db_workload_supported(DB_WORKLOAD_COUNT));

    db_run_request_t request = {.workload = DB_NET_TX, .duration_ms = 1000, .port = 5001};
    char error[96];
    assert(!db_request_validate(&request, error, sizeof(error)));
    strcpy(request.host, "bench-host.local");
    assert(db_request_validate(&request, error, sizeof(error)));
    strcpy(request.host, "bad\"host");
    assert(!db_request_validate(&request, error, sizeof(error)));

    request.workload = DB_CPU_STRESS;
    request.host[0] = '\0';
    request.port = 0;
    db_run_t run;
    db_run_begin(&run, &request, "boot0001-00000001", 10);
    assert(run.state == DB_RUN_RUNNING);
    assert(!db_run_abort(&run, "wrong-id"));
    assert(db_run_abort(&run, "boot0001-00000001"));
    assert(run.state == DB_RUN_ABORTING && run.abort_requested);
    db_run_finish(&run, "aborted", 20);
    assert(run.state == DB_RUN_COMPLETE);
    assert(strcmp(run.result, "aborted") == 0);
    assert(run.started_ms == 10 && run.ended_ms == 20);
    return 0;
}
