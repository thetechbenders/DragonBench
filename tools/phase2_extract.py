"""Slice the Phase 2 continuous logs (tools/phase2_monitor.py output) into a
structured EventRecord for one physical reset trial, reusing the same
field-parsing logic as Phase 1 (tools/reset_characterization.py).

Usage:
    python tools/phase2_extract.py --raw-log ... --events-file ... --http-log ...
        --start ISO_TS --end ISO_TS --index N --reset-class en_button|cold_power
        --operator-action "..." --out-dir artifacts/reset_char
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from reset_characterization import EventRecord, analyze_capture, strip_ansi  # noqa: E402


def parse_iso(ts: str) -> datetime:
    return datetime.fromisoformat(ts)


def load_raw_lines(raw_log: Path, start: datetime, end: datetime) -> list[tuple[float, str]]:
    out = []
    if not raw_log.exists():
        return out
    for line in raw_log.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("["):
            continue
        ts_end = line.find("]")
        if ts_end < 0:
            continue
        try:
            ts = parse_iso(line[1:ts_end])
        except ValueError:
            continue
        if start <= ts <= end:
            out.append(((ts - start).total_seconds(), line[ts_end + 2:]))
    return out


def load_jsonl(path: Path, start: datetime, end: datetime) -> list[dict]:
    out = []
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        ts = rec.get("ts")
        if not ts:
            continue
        try:
            t = parse_iso(ts)
        except ValueError:
            continue
        if start <= t <= end:
            out.append(rec)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--raw-log", required=True, type=Path)
    ap.add_argument("--events-file", required=True, type=Path)
    ap.add_argument("--http-log", required=True, type=Path)
    ap.add_argument("--start", required=True)
    ap.add_argument("--end", required=True)
    ap.add_argument("--index", required=True, type=int)
    ap.add_argument("--reset-class", required=True, choices=("en_button", "cold_power"))
    ap.add_argument("--operator-action", required=True)
    ap.add_argument("--out-dir", required=True, type=Path)
    args = ap.parse_args()

    start = parse_iso(args.start)
    end = parse_iso(args.end)

    raw_lines = load_raw_lines(args.raw_log, start, end)
    port_events = load_jsonl(args.events_file, start, end)
    http_events = load_jsonl(args.http_log, start, end)

    analysis = analyze_capture(raw_lines, 0.0)

    port_lost = [e for e in port_events if e.get("type") == "PORT_LOST"]
    port_reacquired = [e for e in port_events if e.get("type") == "PORT_REACQUIRED"]
    ambiguous = [e for e in port_events if e.get("type") == "PORT_REACQUIRE_AMBIGUOUS"]

    true_power_disappearance: bool | None
    usb_reenumerated: bool | None
    if args.reset_class == "cold_power":
        true_power_disappearance = len(port_lost) >= 1
        usb_reenumerated = len(port_reacquired) >= 1
    else:
        true_power_disappearance = False
        usb_reenumerated = False

    # First ROM banner in-window marks the actual reset instant. If the ROM
    # banner itself was not captured (e.g. cold-power re-enumeration lag),
    # fall back to the app's own JSON "boot" event line, which appears later
    # in the same boot sequence and is still a genuine post-reset marker.
    reset_t = None
    for t, line in raw_lines:
        if "ESP-ROM:esp32s3" in line:
            reset_t = t
            break
    if reset_t is None:
        for t, line in raw_lines:
            if '"event":"boot"' in line:
                reset_t = t
                break

    app_reason = ""
    net_connected = None
    recovery_s = None
    uptime_before = None
    for h in http_events:
        st = h.get("status")
        if not st:
            continue
        t = (parse_iso(h["ts"]) - start).total_seconds()
        if reset_t is not None and t < reset_t:
            uptime_before = st.get("uptime_ms")
            continue
        if reset_t is not None and t >= reset_t:
            if uptime_before is not None and st.get("uptime_ms") is not None and st["uptime_ms"] >= uptime_before:
                continue  # stale pre-reset read
            app_reason = st.get("reset_reason", "")
            net_connected = bool(st.get("network_connected"))
            recovery_s = round(t - reset_t, 2)
            break

    notes_parts = []
    if args.reset_class == "cold_power" and not analysis["rom_rst_raw"]:
        notes_parts.append(
            "ROM banner/rst:/boot: line, SPI Flash Size line, and PSRAM-found line NOT "
            "observable: DUT emits them before Windows finishes re-enumerating the USB-UART "
            "bridge after a full power cycle (host-side instrumentation limitation, not a "
            "DUT fault). rom_rst_raw/rom_boot_raw/flash_size/psram_found are genuinely "
            "unobserved here, not zero/absent on the device."
        )
    if ambiguous:
        notes_parts.append(f"PORT_REACQUIRE_AMBIGUOUS observed: {ambiguous}")
    if args.reset_class == "cold_power" and not port_lost:
        notes_parts.append("no PORT_LOST evidence in window; NOT classified as confirmed cold power-on")
    if args.reset_class == "cold_power" and port_lost and not port_reacquired:
        notes_parts.append("PORT_LOST observed but no PORT_REACQUIRED evidence in window")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.out_dir / f"{args.index:03d}_{args.reset_class}.log"
    raw_path.write_text(
        "\n".join(f"[t+{t:.3f}s] {l}" for t, l in raw_lines)
        + "\n\n-- port events --\n" + json.dumps(port_events, indent=2)
        + "\n\n-- http heartbeat (in window) --\n" + json.dumps(http_events, indent=2),
        encoding="utf-8",
    )

    rec = EventRecord(
        index=args.index,
        reset_class=args.reset_class,
        operator_action=args.operator_action,
        trigger_time=args.start,
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
        http_recovery_s=recovery_s,
        duplicate_reset_count=analysis["duplicate_reset_count"],
        panic=analysis["panic"],
        wdt=analysis["wdt"],
        brownout=analysis["brownout"],
        download_mode=analysis["download_mode"],
        notes="; ".join(notes_parts),
        raw_log_path=str(raw_path),
    )

    print(json.dumps(asdict(rec), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
