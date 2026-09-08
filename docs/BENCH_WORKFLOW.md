# Bench workflow

1. Connect external supply and measurement equipment using the bench-approved
   fixture. DragonBench does not control the PSU.
2. Capture PSU/input voltage, relevant board rails, path current, and reset line
   externally. Do not enter those readings as DUT sensor values.
3. Discover the DUT or select its hostname/IP, then record `/api/v1/device`,
   `/api/v1/sensors`, and reset reason.
4. Start one workload at a time. Correlate `phase_start` and `phase_end` sequence
   markers with scope/current-logger time.
5. Export the run and external measurements together, keeping their provenance
   distinct.

The host/operator owns voltage sweeps. Firmware contains no voltage thresholds
or electrical pass/fail limits. `OTA_PARTITION_WRITE` erases, writes, and verifies
only an inactive OTA slot; it never selects that slot for boot. Controlled reboot
is single-shot and never forms a reboot loop.

Recommended external fields are PSU/input voltage, `+5V_SYS_GATE`,
`+5V_MCU_FEED`, `+5V_MCU`, average/peak current, minimum rail voltage, reset or
brownout occurrence, and Wi-Fi disconnect/reconnect.
