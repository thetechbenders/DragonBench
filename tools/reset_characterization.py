"""DragonBench larger-N reset characterization harness.

Standalone lab-instrument tool (not firmware, not part of the `cli.dragonbench`
HTTP client package). Drives and records reset events for the ESP32-S3-N8R8
follow-up characterization pass described in docs/RESET_ESP32S3.md.

Phase 1 (this script, unattended): RTS/DTR resets and software esp_restart()
resets via the CONTROLLED_REBOOT workload. Both are triggered by the host;
no operator action is required.

Phase 2 (true cold power cycles and physical EN/reset-button presses, which
require an operator) is not driven by this script -- see phase2_monitor.py
(a continuous background logger that survives USB disconnects) and
phase2_extract.py (turns one operator-prompted action window into the same
EventRecord shape used here).

Raw per-event serial logs and the machine-readable summary are written under
--out-dir (default: artifacts/reset_char/, already gitignored). This script
does not modify firmware, erase NVS, or erase PHY calibration data.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

import serial
from serial.tools import list_ports

RST_BOOT_RE = re.compile(r"rst:(0x[0-9a-fA-F]+)\s*\(([^)]+)\).*?boot:(0x[0-9a-fA-F]+)\s*\(([^)]+)\)")
FLASH_SIZE_RE = re.compile(r"SPI Flash Size\s*:\s*(\S+)")
PSRAM_FOUND_RE = re.compile(r"esp_psram:\s*Found\s+(\S+)\s+PSRAM device")
PSRAM_TEST_OK_RE = re.compile(r"esp_psram:.*memory test OK", re.IGNORECASE)
PSRAM_TEST_FAIL_RE = re.compile(r"esp_psram:.*memory test\s+(FAIL|failed)", re.IGNORECASE)
PHY_WARNING = "saving new calibration data because of checksum failure"
ROM_BANNER = "ESP-ROM:esp32s3"
PANIC_MARKERS = ("Guru Meditation Error", "abort() was called")
DOWNLOAD_MODE_MARKER = "waiting for download"
BROWNOUT_MARKER = "Brownout detector was triggered"
# Exact ESP-IDF 5.3.1 message text (components/esp_system/task_wdt/task_wdt.c
# and components/esp_system/port/arch/*/panic_arch.c) -- the naive substring
# check for uppercase "WDT" this replaced never matched real ESP-IDF task/
# interrupt watchdog output, which uses the lowercase "task_wdt"/"int_wdt"
# tags and doesn't contain a bare "WDT" substring anywhere in the message.
WDT_MARKERS = ("Task watchdog got triggered", "Interrupt wdt timeout")


@dataclass
class EventRecord:
    index: int
    reset_class: str
    operator_action: str
    trigger_time: str
    true_power_disappearance: Optional[bool]
    usb_reenumerated: Optional[bool]
    rom_rst_raw: str = ""
    rom_rst_name: str = ""
    rom_boot_raw: str = ""
    rom_boot_name: str = ""
    app_reset_reason: str = ""
    flash_size: str = ""
    psram_found: str = ""
    psram_test: str = ""  # pass / fail / not_observed
    phy_warning_present: bool = False
    phy_warning_position: str = ""
    sta_associated: Optional[bool] = None
    http_recovery_s: Optional[float] = None
    duplicate_reset_count: int = 0
    panic: bool = False
    wdt: bool = False
    brownout: bool = False
    download_mode: bool = False
    serial_disconnect_during_event: bool = False
    notes: str = ""
    raw_log_path: str = ""


def _hwid_of(port_name: str) -> Optional[str]:
    for p in list_ports.comports():
        if p.device == port_name:
            return p.hwid
    return None


def _vid_pid_of(hwid: Optional[str]) -> Optional[str]:
    m = re.search(r"VID:PID=([0-9A-Fa-f]{4}:[0-9A-Fa-f]{4})", hwid or "")
    return m.group(1).upper() if m else None


class SerialMonitor:
    """Reads the DUT's UART continuously in a background thread for the
    whole run, across many events. RTS/DTR and software resets never power
    the USB-UART bridge off, so the port should never actually disappear in
    Phase 1 -- but an unattended multi-hour run with no operator watching it
    should not let one transient driver hiccup (observed as a SerialException
    from ser.read()) silently kill the reader thread and turn every
    subsequent event into a false "no ROM banner captured" reading. On a read
    error this reconnects by USB VID:PID (not by port name/order, since a
    real disappearance could renumber the port) and keeps going, logging the
    gap so it's visible in the run's own stdout rather than hidden."""

    def __init__(self, port: str, baud: int = 115200):
        self.baud = baud
        self._vid_pid = _vid_pid_of(_hwid_of(port))
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self._buf_lock = threading.Lock()
        self._lines: list[tuple[float, str]] = []
        self.disconnect_events: list[tuple[float, float]] = []  # (lost_at, reacquired_at)
        self._stop = False
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def _reconnect(self) -> None:
        lost_at = time.monotonic()
        try:
            self.ser.close()
        except Exception:  # noqa: BLE001
            pass
        print(f"[serial] port lost at t={lost_at:.1f} (monotonic); attempting reconnect "
              f"by VID:PID={self._vid_pid or 'unknown'}...", file=sys.stderr)
        deadline = time.monotonic() + 120.0
        candidate = None
        while time.monotonic() < deadline:
            if self._vid_pid:
                matches = [p.device for p in list_ports.comports() if self._vid_pid in (p.hwid or "").upper()]
            else:
                matches = []
            if len(matches) == 1:
                candidate = matches[0]
            elif len(matches) > 1:
                time.sleep(0.5)
                continue
            if candidate:
                try:
                    self.ser = serial.Serial(candidate, self.baud, timeout=0.2)
                    reacquired_at = time.monotonic()
                    self.disconnect_events.append((lost_at, reacquired_at))
                    print(f"[serial] reconnected on {candidate} at t={reacquired_at:.1f} "
                          f"(gap {reacquired_at - lost_at:.1f}s)", file=sys.stderr)
                    return
                except (serial.SerialException, OSError):
                    candidate = None
            time.sleep(0.5)
        print("[serial] reconnect FAILED after 120s; giving up", file=sys.stderr)
        self.disconnect_events.append((lost_at, -1.0))
        self._stop = True

    def _read_loop(self):
        partial = b""
        while not self._stop:
            try:
                chunk = self.ser.read(4096)
            except (serial.SerialException, OSError):
                self._reconnect()
                partial = b""
                continue
            if not chunk:
                continue
            partial += chunk
            while b"\n" in partial:
                line, partial = partial.split(b"\n", 1)
                ts = time.monotonic()
                text = line.decode(errors="replace").rstrip("\r")
                with self._buf_lock:
                    self._lines.append((ts, text))

    def snapshot_since(self, t0: float) -> list[tuple[float, str]]:
        with self._buf_lock:
            return [(t, l) for (t, l) in self._lines if t >= t0]

    def disconnects_since(self, t0: float) -> list[tuple[float, float]]:
        return [(lost, got) for (lost, got) in self.disconnect_events if lost >= t0]

    def hard_reset_pulse(self):
        """RTS pulse hard reset (EN toggle), DTR held low so IO0 is not forced
        into download mode. Matches esptool's classic hard-reset sequence."""
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.15)
        self.ser.rts = False

    def close(self):
        self._stop = True
        self._thread.join(timeout=1)
        self.ser.close()


def strip_ansi(text: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", text)


def http_get(host: str, path: str, timeout: float = 3.0):
    req = urllib.request.Request(f"http://{host}{path}", headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def http_post(host: str, path: str, body: dict, timeout: float = 5.0):
    data = json.dumps(body).encode()
    req = urllib.request.Request(
        f"http://{host}{path}", data=data, method="POST",
        headers={"Accept": "application/json", "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def analyze_capture(lines: list[tuple[float, str]], t0: float) -> dict:
    result: dict = {
        "rom_rst_raw": "", "rom_rst_name": "", "rom_boot_raw": "", "rom_boot_name": "",
        "flash_size": "", "psram_found": "", "psram_test": "not_observed",
        "phy_warning_present": False, "phy_warning_position": "",
        "duplicate_reset_count": 0, "panic": False, "wdt": False,
        "brownout": False, "download_mode": False,
    }
    banner_count = 0
    for ts, raw in lines:
        line = strip_ansi(raw)
        if ROM_BANNER in line:
            banner_count += 1
            continue
        if not result["rom_rst_raw"]:
            m = RST_BOOT_RE.search(line)
            if m:
                result["rom_rst_raw"], result["rom_rst_name"] = m.group(1), m.group(2)
                result["rom_boot_raw"], result["rom_boot_name"] = m.group(3), m.group(4)
        if not result["flash_size"]:
            m = FLASH_SIZE_RE.search(line)
            if m:
                result["flash_size"] = m.group(1)
        if not result["psram_found"]:
            m = PSRAM_FOUND_RE.search(line)
            if m:
                result["psram_found"] = m.group(1)
        if PSRAM_TEST_OK_RE.search(line):
            result["psram_test"] = "pass"
        elif PSRAM_TEST_FAIL_RE.search(line):
            result["psram_test"] = "fail"
        if PHY_WARNING in line and not result["phy_warning_present"]:
            result["phy_warning_present"] = True
            result["phy_warning_position"] = f"t+{ts - t0:.2f}s"
        if any(marker in line for marker in PANIC_MARKERS):
            result["panic"] = True
        if any(marker in line for marker in WDT_MARKERS):
            result["wdt"] = True
        if BROWNOUT_MARKER in line:
            result["brownout"] = True
        if DOWNLOAD_MODE_MARKER in line:
            result["download_mode"] = True
    result["duplicate_reset_count"] = max(0, banner_count - 1)
    return result


def poll_status_for_recovery(host: str, trigger_monotonic: float, uptime_before_ms: Optional[float],
                              max_wait: float = 10.0):
    """Poll /api/v1/status until a response reflects a genuine post-reset
    boot (uptime_ms lower than the pre-trigger baseline, when known), then
    return (reset_reason, network_connected, recovery_seconds, status_json).

    A response with uptime_ms >= uptime_before_ms is a stale read of the
    still-running pre-reset session (e.g. CONTROLLED_REBOOT keeps the HTTP
    server up for ~1.25s after the trigger before esp_restart() actually
    fires) and is not treated as recovery."""
    deadline = time.monotonic() + max_wait
    last_err = None
    last_stale = None
    while time.monotonic() < deadline:
        try:
            status = http_get(host, "/api/v1/status", timeout=2.0)
            uptime_ms = status.get("uptime_ms")
            if uptime_before_ms is not None and uptime_ms is not None and uptime_ms >= uptime_before_ms:
                last_stale = status
                time.sleep(0.3)
                continue
            recovery_s = time.monotonic() - trigger_monotonic
            return status.get("reset_reason", ""), bool(status.get("network_connected")), recovery_s, status
        except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
            last_err = exc
            time.sleep(0.3)
    if last_stale is not None:
        return last_stale.get("reset_reason", ""), bool(last_stale.get("network_connected")), None, \
            {**last_stale, "_note": "stale pre-reset read only; no post-reset response observed within max_wait"}
    return "", None, None, {"error": str(last_err)}


def run_event(mon: SerialMonitor, host: str, index: int, reset_class: str,
              operator_action: str, out_dir: Path,
              trigger_fn, settle_s: float = 8.0,
              true_power_disappearance: Optional[bool] = None,
              usb_reenumerated: Optional[bool] = None) -> EventRecord:
    try:
        uptime_before_ms = http_get(host, "/api/v1/status", timeout=2.0).get("uptime_ms")
    except Exception:
        uptime_before_ms = None

    t0 = time.monotonic()
    trigger_wall = time.strftime("%Y-%m-%dT%H:%M:%S")
    trigger_fn()

    poll_result: dict = {}

    def poll():
        poll_result["value"] = poll_status_for_recovery(host, t0, uptime_before_ms, max_wait=max(settle_s, 15.0))

    poll_thread = threading.Thread(target=poll, daemon=True)
    poll_thread.start()

    time.sleep(settle_s)
    lines = mon.snapshot_since(t0)
    analysis = analyze_capture(lines, t0)

    poll_thread.join(timeout=max(0.0, 15.0 - settle_s) + 1.0)
    app_reason, net_connected, recovery_s, status = poll_result.get("value", ("", None, None, {"error": "poll_thread_timeout"}))
    disconnects = mon.disconnects_since(t0)

    raw_path = out_dir / f"{index:03d}_{reset_class}.log"
    raw_path.write_text(
        "\n".join(f"[t+{t - t0:.3f}s] {l}" for t, l in lines) + f"\n\n-- status --\n{json.dumps(status, indent=2)}\n",
        encoding="utf-8",
    )

    rec = EventRecord(
        index=index,
        reset_class=reset_class,
        operator_action=operator_action,
        trigger_time=trigger_wall,
        true_power_disappearance=true_power_disappearance,
        usb_reenumerated=usb_reenumerated,
        rom_rst_raw=analysis["rom_rst_raw"],
        rom_rst_name=analysis["rom_rst_name"],
        rom_boot_raw=analysis["rom_boot_raw"],
        rom_boot_name=analysis["rom_boot_name"],
        app_reset_reason=app_reason,
        flash_size=analysis["flash_size"],
        psram_found=analysis["psram_found"],
        psram_test=analysis["psram_test"],
        phy_warning_present=analysis["phy_warning_present"],
        phy_warning_position=analysis["phy_warning_position"],
        sta_associated=net_connected,
        http_recovery_s=round(recovery_s, 2) if recovery_s is not None else None,
        duplicate_reset_count=analysis["duplicate_reset_count"],
        panic=analysis["panic"],
        wdt=analysis["wdt"],
        brownout=analysis["brownout"],
        download_mode=analysis["download_mode"],
        serial_disconnect_during_event=bool(disconnects),
        notes=f"serial reconnect during event window: {disconnects}" if disconnects else "",
        raw_log_path=str(raw_path),
    )
    flags = []
    if rec.panic: flags.append("PANIC")
    if rec.wdt: flags.append("WDT")
    if rec.brownout: flags.append("BROWNOUT")
    if rec.download_mode: flags.append("DOWNLOAD_MODE")
    if rec.duplicate_reset_count: flags.append(f"DUPLICATE_RESET x{rec.duplicate_reset_count}")
    if not rec.rom_rst_raw: flags.append("NO_ROM_BANNER_CAPTURED")
    if rec.rom_boot_raw and rec.rom_boot_raw not in ("0x8", "0xa"): flags.append(f"UNEXPECTED_BOOT_FIELD={rec.rom_boot_raw}")
    if rec.http_recovery_s is None: flags.append("HTTP_RECOVERY_NOT_OBSERVED")
    if rec.serial_disconnect_during_event: flags.append("SERIAL_RECONNECT_DURING_EVENT")
    print(f"[{index:03d}] {reset_class:12s} rst={rec.rom_rst_raw or '?':6s} boot={rec.rom_boot_raw or '?':6s} "
          f"app_reason={rec.app_reset_reason or '?':10s} psram={rec.psram_test:12s} phy_warn={rec.phy_warning_present} "
          f"recovery={rec.http_recovery_s}s" + (f"  ANOMALY: {', '.join(flags)}" if flags else ""))
    return rec


def write_summary(records: list[EventRecord], out_dir: Path):
    csv_path = out_dir / "summary.csv"
    json_path = out_dir / "summary.json"
    fieldnames = list(asdict(records[0]).keys()) if records else []
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for r in records:
            w.writerow(asdict(r))
    json_path.write_text(json.dumps([asdict(r) for r in records], indent=2), encoding="utf-8")
    print(f"\nWrote {csv_path} and {json_path}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True, help="Serial port of the DUT, e.g. COM11")
    p.add_argument("--host", default="dragonbench.local", help="DUT HTTP host")
    p.add_argument("--out-dir", default="artifacts/reset_char", type=Path)
    p.add_argument("--rts-dtr-count", type=int, default=10)
    p.add_argument("--software-count", type=int, default=10)
    p.add_argument("--settle-s", type=float, default=8.0, help="Serial capture window per event")
    p.add_argument("--sequential", action="store_true",
                    help="Run all RTS/DTR events then all software events, instead of interleaving "
                         "(interleaved is the default: RTS/DTR, software, RTS/DTR, software, ... "
                         "which spreads any time/session drift evenly across both classes).")
    args = p.parse_args(argv)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    mon = SerialMonitor(args.port)
    records: list[EventRecord] = []
    idx = 0

    def trigger_software():
        http_post(args.host, "/api/v1/runs", {"workload": "CONTROLLED_REBOOT", "duration_ms": 1000, "rate_bps": 0})

    class_specs = {
        "rts_dtr": ("host-triggered RTS pulse (esptool-style hard reset)", mon.hard_reset_pulse),
        "software": ("host-triggered CONTROLLED_REBOOT via HTTP API", trigger_software),
    }

    if args.sequential:
        sequence = ["rts_dtr"] * args.rts_dtr_count + ["software"] * args.software_count
    else:
        sequence = []
        n = max(args.rts_dtr_count, args.software_count)
        for i in range(n):
            if i < args.rts_dtr_count:
                sequence.append("rts_dtr")
            if i < args.software_count:
                sequence.append("software")

    try:
        # Preflight: confirm baseline reachability before starting.
        try:
            http_get(args.host, "/api/v1/device", timeout=5.0)
        except Exception as exc:
            print(f"error: DUT not reachable at {args.host} before starting: {exc}", file=sys.stderr)
            return 2

        print(f"== {len(sequence)} events ({args.rts_dtr_count} rts_dtr, {args.software_count} software), "
              f"{'sequential' if args.sequential else 'interleaved'} ==")
        for reset_class in sequence:
            idx += 1
            operator_action, trigger_fn = class_specs[reset_class]
            rec = run_event(
                mon, args.host, idx, reset_class, operator_action,
                args.out_dir, trigger_fn=trigger_fn, settle_s=args.settle_s,
                true_power_disappearance=False, usb_reenumerated=False,
            )
            records.append(rec)
            time.sleep(1.0)
    finally:
        mon.close()

    write_summary(records, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
