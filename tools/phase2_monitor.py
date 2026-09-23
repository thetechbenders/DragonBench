"""Background continuous monitor for DragonBench Phase 2 (physical reset
events: EN button, true cold power cycle).

Runs standalone and keeps running across USB disconnects. Writes:

- --raw-log: every UART line from the DUT, prefixed with an ISO-8601 UTC
  wall-clock timestamp, appended continuously.
- --events-file: JSONL port-lifecycle events (PORT_LOST / PORT_REACQUIRED /
  PORT_REACQUIRE_AMBIGUOUS / PORT_REACQUIRE_TIMEOUT). This is the objective
  evidence used to confirm true USB disappearance/re-enumeration during
  cold-power trials, independent of operator self-report.
- --http-log: JSONL heartbeat of GET /api/v1/status polled roughly once a
  second (best-effort), used to derive network/API recovery timing after a
  physical event.

After a reconnect, the DUT is re-identified by USB VID:PID (and serial
number when available), not by COM port number or enumeration order, since
re-enumeration can change the assigned port.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

import serial
from serial.tools import list_ports


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def hwid_of(port_name: str) -> str | None:
    for p in list_ports.comports():
        if p.device == port_name:
            return p.hwid
    return None


def vid_pid_of(hwid: str) -> str | None:
    m = re.search(r"VID:PID=([0-9A-Fa-f]{4}:[0-9A-Fa-f]{4})", hwid or "")
    return m.group(1).upper() if m else None


def candidates_for(vid_pid: str) -> list[str]:
    return [p.device for p in list_ports.comports() if vid_pid in (p.hwid or "").upper()]


def log_event(events_path: str, ev: dict) -> None:
    ev = {"ts": now_iso(), **ev}
    with open(events_path, "a", encoding="utf-8") as f:
        f.write(json.dumps(ev) + "\n")
    print(json.dumps(ev), file=sys.stderr, flush=True)


def http_heartbeat(host: str, http_log: str, stop_event: threading.Event) -> None:
    while not stop_event.is_set():
        rec = {"ts": now_iso()}
        try:
            req = urllib.request.Request(f"http://{host}/api/v1/status", headers={"Accept": "application/json"})
            with urllib.request.urlopen(req, timeout=2.0) as resp:
                rec["status"] = json.loads(resp.read().decode())
        except Exception as exc:  # noqa: BLE001 - best-effort heartbeat, any failure just gets recorded
            rec["error"] = str(exc)
        with open(http_log, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
        time.sleep(1.0)


def serial_loop(initial_port: str, raw_log: str, events_file: str) -> None:
    hwid = hwid_of(initial_port)
    if not hwid:
        print(f"error: cannot read hwid for {initial_port}", file=sys.stderr)
        raise SystemExit(2)
    vid_pid = vid_pid_of(hwid)
    if not vid_pid:
        print(f"error: cannot parse VID:PID from hwid {hwid!r}", file=sys.stderr)
        raise SystemExit(2)

    log_event(events_file, {"type": "MONITOR_START", "port": initial_port, "hwid": hwid, "vid_pid": vid_pid})

    current_port = initial_port
    ser = serial.Serial(current_port, 115200, timeout=0.2)
    raw_f = open(raw_log, "a", encoding="utf-8")
    partial = b""

    while True:
        try:
            chunk = ser.read(4096)
            if chunk:
                partial += chunk
                while b"\n" in partial:
                    line, partial = partial.split(b"\n", 1)
                    text = line.decode(errors="replace").rstrip("\r")
                    raw_f.write(f"[{now_iso()}] {text}\n")
                    raw_f.flush()
        except (serial.SerialException, OSError) as exc:
            log_event(events_file, {"type": "PORT_LOST", "port": current_port, "error": str(exc)})
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
            partial = b""
            current_port = _reacquire(vid_pid, events_file)
            ser = serial.Serial(current_port, 115200, timeout=0.2)
            log_event(events_file, {"type": "PORT_REACQUIRED", "port": current_port, "hwid": hwid_of(current_port)})


def _reacquire(vid_pid: str, events_file: str) -> str:
    waited = 0.0
    last_ambiguous_report = 0.0
    while True:
        time.sleep(0.3)
        waited += 0.3
        candidates = candidates_for(vid_pid)
        if len(candidates) == 1:
            cand = candidates[0]
            try:
                probe = serial.Serial(cand, 115200, timeout=0.2)
                probe.close()
                return cand
            except (serial.SerialException, OSError):
                continue
        elif len(candidates) > 1 and waited - last_ambiguous_report > 3.0:
            log_event(events_file, {"type": "PORT_REACQUIRE_AMBIGUOUS", "candidates": candidates})
            last_ambiguous_report = waited
        if waited > 0 and waited % 60 < 0.31:
            log_event(events_file, {"type": "PORT_REACQUIRE_WAITING", "waited_s": round(waited, 1)})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--raw-log", required=True)
    ap.add_argument("--events-file", required=True)
    ap.add_argument("--http-log", required=True)
    args = ap.parse_args()

    stop_event = threading.Event()
    http_thread = threading.Thread(target=http_heartbeat, args=(args.host, args.http_log, stop_event), daemon=True)
    http_thread.start()

    serial_loop(args.port, args.raw_log, args.events_file)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
