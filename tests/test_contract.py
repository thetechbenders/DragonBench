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


def _firmware_page(name):
    """Extract and de-escape an embedded page (e.g. landing[]) C string from main.c.

    Isolating this from the rest of main.c matters: the file also legitimately
    contains the /api/v1/runs route registrations, which would otherwise
    false-positive a "no write controls in the landing page" check.
    """
    source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
    start = source.index(f"static const char {name}[] =")
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
        firmware_landing = _firmware_page("landing")
        version_slot = re.compile(r"(Firmware: )(.*?)(</p>)")

        def normalize(html):
            html = version_slot.sub(r"\1VERSION\3", html)
            return re.sub(r"\s+", "", html)

        self.assertEqual(normalize(web), normalize(firmware_landing))

    def test_landing_page_represents_actuator_boundary(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_page("landing")
        for page in (web, firmware_landing):
            self.assertIn("CHARACTERIZATION IMAGE", page.upper())
            self.assertIn("NO PRODUCT ACTUATOR SUPPORT", page.upper())
            self.assertIn('data-role="capability-heater"', page)
            self.assertIn('data-role="capability-fan"', page)
            self.assertIn("ABSENT", page)

    def test_landing_page_has_stable_automation_hooks(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_page("landing")
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
        firmware_landing = _firmware_page("landing")
        forbidden = ("http://", "https://", "cdn.", "//fonts.")
        for page in (web, firmware_landing):
            for token in forbidden:
                self.assertNotIn(token, page)

    def test_landing_page_avoids_innerhtml_for_dut_values(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_page("landing")
        for page in (web, firmware_landing):
            self.assertNotIn("innerHTML", page)

    def test_landing_page_has_no_write_controls(self):
        web = (ROOT / "web/index.html").read_text()
        firmware_landing = _firmware_page("landing")
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
        self.assertIn('RTC_NOINIT_ATTR', source)
        for mapping in ('ESP_RST_USB: return "usb"', 'ESP_RST_JTAG: return "jtag"',
                        'ESP_RST_PWR_GLITCH: return "power_glitch"'):
            self.assertIn(mapping, source)

    def test_direct_ap_is_default_and_credentials_are_not_serialized(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        kconfig = (ROOT / "firmware/targets/esp32s3/main/Kconfig.projbuild").read_text()
        self.assertIn("esp_netif_create_default_wifi_ap", source)
        self.assertIn("WIFI_MODE_AP", source)
        self.assertIn("WIFI_AUTH_WPA2_PSK", source)
        self.assertIn('default "Dragon$ru1e"', kconfig)
        serializers = source[source.index("static cJSON *identity_json"):source.index("static void wifi_event")]
        self.assertNotIn("CONFIG_DB_AP_PASSWORD", serializers)
        self.assertNotIn("CONFIG_DB_WIFI_PASSWORD", serializers)

    def test_network_connected_reports_station_association(self):
        # tools/reset_characterization.py records this field as sta_associated.
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        self.assertIn('"network_connected", sta_state == DB_STA_CONNECTED', source)

    @staticmethod
    def _function_body(source, signature):
        start = source.index(signature)
        return source[start:source.index("\n}\n", start)]

    def test_provisioning_is_serialized_through_one_worker(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        handler = self._function_body(source, "static esp_err_t network_sta_post")
        self.assertIn("xQueueOverwrite(sta_config_queue", handler)
        for radio_call in ("configure_sta(", "esp_wifi_set_config", "esp_wifi_connect", "esp_wifi_disconnect"):
            self.assertNotIn(radio_call, handler)
        worker = self._function_body(source, "static void sta_config_task")
        self.assertLess(worker.index("esp_wifi_disconnect()"), worker.index("configure_sta("))
        self.assertLess(worker.index("configure_sta("), worker.index("nvs_save_sta("))

    def test_station_reconnect_backs_off_instead_of_giving_up(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        handler = self._function_body(source, "static void wifi_event")
        disconnected = handler[handler.index("WIFI_EVENT_STA_DISCONNECTED"):handler.index("IP_EVENT_STA_GOT_IP")]
        self.assertIn("sta_schedule_retry();", disconnected)
        self.assertIn("db_sta_retry_delay_ms(", self._function_body(source, "static void sta_schedule_retry"))

    def test_station_password_is_never_serialized(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        self.assertIsNone(re.search(r'cJSON_Add\w*ToObject\([^;]*"password"', source))

    def test_setup_page_only_writes_station_config(self):
        setup = _firmware_page("setup_page")
        main = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        self.assertIn('.uri="/setup"', main)
        self.assertIn("/api/v1/network/sta", setup)
        self.assertIn('data-role="sta-freshness"', setup)
        self.assertIn("if(inFlight)return;", setup)
        self.assertIn("config.lru_purge_enable = true;", main)
        self.assertNotIn("/api/v1/runs", setup)
        self.assertNotIn("innerHTML", setup)
        for token in ("http://", "https://", "cdn.", "//fonts."):
            self.assertNotIn(token, setup)

    def test_board_profiles_report_schema_targets(self):
        schema = json.loads((ROOT / "protocol/event.schema.json").read_text())
        targets = schema["properties"]["target"]["enum"]
        kconfig = (ROOT / "firmware/targets/esp32s3/main/Kconfig.projbuild").read_text()
        base = (ROOT / "sdkconfig.defaults").read_text()
        tinys3d = (ROOT / "sdkconfig.defaults.tinys3d").read_text()
        default_target = re.search(r'config DB_TARGET_NAME\n(?:.*\n)*?\s+default "([^"]+)"', kconfig).group(1)
        self.assertEqual(default_target, "esp32s3-n8r8")
        self.assertIn(default_target, targets)
        self.assertIn("CONFIG_SPIRAM_MODE_OCT=y", base)
        self.assertIn('CONFIG_DB_TARGET_NAME="esp32s3-tinys3d"', tinys3d)
        self.assertIn("esp32s3-tinys3d", targets)
        self.assertIn("CONFIG_SPIRAM_MODE_QUAD=y", tinys3d)
        self.assertIn("CONFIG_DB_RF_SWITCH_GPIO=38", tinys3d)

    def test_rf_switch_selects_onboard_antenna_before_wifi_starts(self):
        source = (ROOT / "firmware/targets/esp32s3/main/main.c").read_text()
        init = source[source.index("static void antenna_init"):]
        init = init[:init.index("\n}\n")]
        self.assertIn("gpio_set_level(CONFIG_DB_RF_SWITCH_GPIO, 0)", init)
        self.assertNotIn("gpio_set_level(CONFIG_DB_RF_SWITCH_GPIO, 1)", source)
        app_main = source[source.index("void app_main(void)"):]
        self.assertLess(app_main.index("antenna_init();"), app_main.index("start_wifi()"))


if __name__ == "__main__":
    unittest.main()
