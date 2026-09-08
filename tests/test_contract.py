import json
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
WORKLOADS = [
    "BOOT", "IDLE", "WIFI_ASSOCIATED_IDLE", "NET_TX", "NET_RX",
    "NET_BIDIRECTIONAL", "CPU_STRESS", "FLASH_WRITE", "NVS_WRITE",
    "OTA_PARTITION_WRITE", "CONTROLLED_REBOOT",
]


class ContractTests(unittest.TestCase):
    def test_event_schema_has_required_lifecycle(self):
        schema = json.loads((ROOT / "protocol/event.schema.json").read_text())
        events = schema["properties"]["event"]["enum"]
        self.assertEqual(events, ["boot", "ready", "phase_start", "phase_end", "fault", "reset_reason", "run_complete"])

    def test_registry_contains_exact_v1_vocabulary(self):
        source = (ROOT / "firmware/common/db_run.c").read_text()
        observed = [name for name in WORKLOADS if f'"{name}"' in source]
        self.assertEqual(observed, WORKLOADS)

    def test_api_and_landing_use_same_read_endpoints(self):
        api = (ROOT / "protocol/openapi.yaml").read_text()
        web = (ROOT / "web/index.html").read_text()
        firmware = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        for endpoint in ("status", "sensors", "workloads"):
            self.assertIn(f"/api/v1/{endpoint}", api)
            self.assertIn(f"/api/v1/{endpoint}", firmware)
            self.assertIn(f"get('{endpoint}')", web)

    def test_actuator_exclusion(self):
        source = "\n".join(p.read_text(errors="ignore") for p in (ROOT / "firmware").rglob("*.[ch]"))
        for forbidden in ("HEATER_GPIO", "FAN_GPIO", "heater_set", "fan_set"):
            self.assertNotIn(forbidden, source)

    def test_ota_never_selects_boot_partition(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        self.assertIn("esp_ota_get_next_update_partition", source)
        self.assertNotIn("esp_ota_set_boot_partition", source)

    def test_failures_emit_fault_before_phase_end(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        fault = source.index('emit_event("fault"')
        phase_end = source.index('emit_event("phase_end"')
        self.assertLess(fault, phase_end)

    def test_run_identity_uses_boot_session_and_counter(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        self.assertIn('boot_nonce, ++run_counter', source)

    def test_capabilities_are_truthful_and_unsupported_is_explicit(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        self.assertIn('"heater_capability", false', source)
        self.assertIn('"fan_control_capability", false', source)
        self.assertIn('"supply_voltage"', source)
        self.assertIn('"unsupported"', source)
        self.assertIn('"BLE_STRESS"', source)

    def test_reset_reason_is_reported_and_correlated(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        self.assertIn('"reset_reason"', source)
        self.assertIn('previous_reboot_run_id', source)
        self.assertIn('RTC_DATA_ATTR', source)


if __name__ == "__main__":
    unittest.main()
