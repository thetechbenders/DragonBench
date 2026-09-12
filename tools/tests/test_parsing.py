"""Offline tests for the reset-characterization log parser
(tools/reset_characterization.py: analyze_capture and friends), so parsing
logic can be validated without hardware attached.

Fixtures in tools/tests/fixtures/ are one raw serial line per line (no
timestamp prefix, no operator/PHY-heartbeat framing added by the harness).
Files prefixed `real_` are sanitized excerpts of actual DragonBench captures
(artifacts/reset_char/, gitignored) -- trimmed to the boot-sequence lines
needed for the test and stripped of the harness's own "[t+...s] " timestamp
prefix; none of them include Wi-Fi credentials. Files prefixed `synthetic_`
are hand-constructed and do NOT represent anything actually observed on this
project's hardware -- panic, task-watchdog, brownout, and download-mode
conditions have never occurred in any DragonBench characterization pass to
date; these fixtures exist only to prove the parser recognizes the exact
ESP-IDF/ROM message text for those conditions if they ever do occur.
"""

from __future__ import annotations

import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))
from reset_characterization import analyze_capture, now_iso  # noqa: E402
import phase2_monitor  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"


def load_lines(name: str) -> list[tuple[float, str]]:
    """Load a fixture file as the (monotonic_offset, line) pairs analyze_capture
    expects, assigning each line an arbitrary increasing fake timestamp."""
    text = (FIXTURES / name).read_text(encoding="utf-8")
    return [(float(i), line) for i, line in enumerate(text.splitlines())]


class NormalBootTests(unittest.TestCase):
    def test_real_normal_boot_0x8_parses_all_fields(self):
        result = analyze_capture(load_lines("real_normal_boot_0x8.log"), 0.0)
        self.assertEqual(result["rom_rst_raw"], "0x1")
        self.assertEqual(result["rom_rst_name"], "POWERON")
        self.assertEqual(result["rom_boot_raw"], "0x8")
        self.assertEqual(result["rom_boot_name"], "SPI_FAST_FLASH_BOOT")
        self.assertEqual(result["flash_size"], "8MB")
        self.assertEqual(result["psram_found"], "8MB")
        self.assertEqual(result["psram_test"], "pass")
        self.assertFalse(result["phy_warning_present"])
        self.assertEqual(result["duplicate_reset_count"], 0)
        self.assertFalse(result["panic"])
        self.assertFalse(result["wdt"])
        self.assertFalse(result["brownout"])
        self.assertFalse(result["download_mode"])

    def test_real_phy_warning_is_detected_with_position(self):
        result = analyze_capture(load_lines("real_phy_warning_0x8.log"), 0.0)
        self.assertTrue(result["phy_warning_present"])
        self.assertTrue(result["phy_warning_position"].startswith("t+"))
        # A genuine PHY warning boot is still a normal, otherwise-healthy boot.
        self.assertEqual(result["rom_boot_raw"], "0x8")
        self.assertEqual(result["psram_test"], "pass")
        self.assertFalse(result["panic"])


class TruncatedCaptureTests(unittest.TestCase):
    def test_real_cold_power_gap_yields_no_rom_fields_not_a_crash(self):
        """This fixture is a genuine capture where the USB-UART bridge
        re-enumeration lag lost the ROM banner/rst:/boot:/flash-size/
        PSRAM-found lines (see docs/RESET_ESP32S3.md follow-up section).
        The parser must degrade gracefully -- empty fields, no exception --
        rather than crash or fabricate a value, and must still pick up
        whatever WAS captured (PSRAM test result, PHY warning)."""
        result = analyze_capture(load_lines("real_truncated_cold_power_no_rom_banner.log"), 0.0)
        self.assertEqual(result["rom_rst_raw"], "")
        self.assertEqual(result["rom_boot_raw"], "")
        self.assertEqual(result["flash_size"], "")
        self.assertEqual(result["psram_found"], "")
        # What *did* stream after the gap should still parse correctly.
        self.assertEqual(result["psram_test"], "pass")
        self.assertTrue(result["phy_warning_present"])

    def test_empty_capture_does_not_crash(self):
        result = analyze_capture([], 0.0)
        self.assertEqual(result["rom_rst_raw"], "")
        self.assertEqual(result["duplicate_reset_count"], 0)


class DuplicateAndAnomalyTests(unittest.TestCase):
    def test_synthetic_duplicate_reset_counts_extra_banner(self):
        result = analyze_capture(load_lines("synthetic_duplicate_reset.log"), 0.0)
        self.assertEqual(result["duplicate_reset_count"], 1)
        # First-seen values win; both banners in this fixture agree anyway.
        self.assertEqual(result["rom_boot_raw"], "0x8")

    def test_synthetic_unexpected_boot_field_is_captured_verbatim(self):
        result = analyze_capture(load_lines("synthetic_unexpected_boot_field.log"), 0.0)
        self.assertEqual(result["rom_boot_raw"], "0x12")
        self.assertEqual(result["rom_boot_name"], "SOME_UNKNOWN_BOOT_MODE")
        # Aggregation (tools/analyze_summary.py) is responsible for bucketing
        # anything outside {0x8, 0xa} as "other" -- the parser's job here is
        # only to not silently drop or misparse an unfamiliar value.

    def test_synthetic_panic_is_flagged(self):
        result = analyze_capture(load_lines("synthetic_panic.log"), 0.0)
        self.assertTrue(result["panic"])

    def test_synthetic_wdt_brownout_download_mode_are_flagged(self):
        result = analyze_capture(load_lines("synthetic_wdt_and_brownout_and_download_mode.log"), 0.0)
        self.assertTrue(result["wdt"])
        self.assertTrue(result["brownout"])
        self.assertTrue(result["download_mode"])
        # The rst:/boot: line itself contains neither "WDT" nor these
        # markers, so a healthy boot line must never trip these flags --
        # guard against the wdt regex accidentally matching the rst: line.
        self.assertEqual(result["rom_boot_raw"], "0x8")


class AbsoluteTimestampConventionTests(unittest.TestCase):
    """All future DragonBench characterization tooling records absolute
    wall-clock timestamps as timezone-aware UTC ISO 8601, not a naive
    time.strftime() string. Earlier captures (the original Phase 1 pass and
    the 100-event unattended run) used naive local-time strings while Phase 2
    used explicit UTC from the start; those existing artifacts are untouched
    historical evidence, and this test only governs what new tooling code
    produces going forward."""

    def _assert_is_timezone_aware_utc_iso8601(self, ts: str):
        parsed = datetime.fromisoformat(ts)
        self.assertIsNotNone(parsed.tzinfo, f"{ts!r} parsed as timezone-naive")
        self.assertEqual(parsed.utcoffset().total_seconds(), 0, f"{ts!r} is not UTC")

    def test_reset_characterization_now_iso_is_timezone_aware_utc(self):
        self._assert_is_timezone_aware_utc_iso8601(now_iso())

    def test_phase2_monitor_now_iso_is_timezone_aware_utc(self):
        self._assert_is_timezone_aware_utc_iso8601(phase2_monitor.now_iso())

    def test_both_now_iso_helpers_agree_on_convention(self):
        # Not asserting identical microsecond values (they're called at
        # different instants) -- just that both produce the same shape.
        a, b = now_iso(), phase2_monitor.now_iso()
        for ts in (a, b):
            self.assertRegex(ts, r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}\+00:00$")


if __name__ == "__main__":
    unittest.main()
