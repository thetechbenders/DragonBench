import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
WORKLOADS = [
    "BOOT", "IDLE", "WIFI_ASSOCIATED_IDLE", "NET_TX", "NET_RX",
    "NET_BIDIRECTIONAL", "CPU_STRESS", "FLASH_WRITE", "NVS_WRITE",
    "OTA_PARTITION_WRITE", "CONTROLLED_REBOOT",
]


def _firmware_landing_html():
    """Extract and de-escape the embedded landing[] C string from main.c.

    Isolating this from the rest of main.c matters: the file also legitimately
    contains the /api/v1/runs route registrations, which would otherwise
    false-positive a "no write controls in the landing page" check.
    """
    source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
    start = source.index("static const char landing[] =")
    i, in_string, end = start, False, None
    while i < len(source):
        ch = source[i]
        if in_string and ch == "\\":
            i += 2
            continue
        if ch == '"':
            in_string = not in_string
        elif ch == ";" and not in_string:
            end = i
            break
        i += 1
    block = source[start:end]
    literal_bodies = re.findall(r'"((?:[^"\\]|\\.)*)"', block)
    html = "".join(literal_bodies)
    return html.replace('\\"', '"').replace("\\\\", "\\")


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
            self.assertIn(f"'/api/v1/'+name", web)
            self.assertIn(f'data-role="raw-{endpoint}"', web)

    def test_web_and_firmware_landing_pages_are_synchronized(self):
        """web/index.html is the dev copy; firmware/.../main.c:landing[] is what the
        device actually serves. They must stay byte-equivalent so a UI edit to one
        can never silently leave the other stale.

        The only sanctioned difference is the firmware-version slot: main.c
        interpolates CONFIG_DB_FIRMWARE_VERSION at build time, while the static
        dev copy carries a fixed placeholder. That slot is normalized to a common
        token before comparing; everything else must match exactly. Whitespace is
        also stripped, because the firmware string concatenates its per-line C
        literals with no separator while web/index.html keeps real newlines
        between them; that is a source-formatting artifact, not a content
        difference the two pages would ever actually render.
        """
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_landing_html()
        version_slot = re.compile(r"(Firmware: )(.*?)(</p>)")

        def normalize(html):
            html = version_slot.sub(r"\1VERSION\3", html)
            return re.sub(r"\s+", "", html)

        self.assertEqual(normalize(web), normalize(firmware_landing))

    def test_landing_page_represents_actuator_boundary(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_landing_html()
        for page in (web, firmware_landing):
            self.assertIn("CHARACTERIZATION IMAGE", page.upper())
            self.assertIn("NO PRODUCT ACTUATOR SUPPORT", page.upper())
            self.assertIn('data-role="capability-heater"', page)
            self.assertIn('data-role="capability-fan"', page)
            self.assertIn("ABSENT", page)

    def test_landing_page_has_stable_automation_hooks(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_landing_html()
        required_hooks = (
            "panel-status", "panel-sensors", "panel-workloads",
            "capability-heater", "capability-fan",
            "raw-status", "raw-sensors", "raw-workloads",
            "freshness",
        )
        for page in (web, firmware_landing):
            for hook in required_hooks:
                self.assertIn(f'data-role="{hook}"', page)

    def test_landing_page_has_no_external_dependency(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_landing_html()
        forbidden = ("http://", "https://", "cdn.", "//fonts.")
        for page in (web, firmware_landing):
            for token in forbidden:
                self.assertNotIn(token, page)

    def test_landing_page_avoids_innerhtml_for_dut_values(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_landing_html()
        for page in (web, firmware_landing):
            self.assertNotIn("innerHTML", page)

    def test_landing_page_has_no_write_controls(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_landing_html()
        forbidden_exact = ("/api/v1/runs",)
        forbidden_ci = ("method=\"post\"", "method='post'")
        for page in (web, firmware_landing):
            for token in forbidden_exact:
                self.assertNotIn(token, page)
            lowered = page.lower()
            for token in forbidden_ci:
                self.assertNotIn(token, lowered)

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
