#include "db_run.h"

#include <assert.h>
#include <string.h>

int main(void) {
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
