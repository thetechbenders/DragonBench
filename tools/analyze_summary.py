"""Aggregate analysis across one or more DragonBench reset-characterization
summary.json files (tools/reset_characterization.py output) plus optional
Phase 2 per-event JSON records (tools/phase2_extract.py output).

Produces: boot-field-by-class, PHY-warning-by-class, PHY-warning-by-boot-field,
and per-class recovery-time statistics (min/median/mean/p95/max) with simple
IQR-based outlier flagging. Read-only -- does not touch firmware, NVS, or
PHY calibration, and does not modify any input file.
"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


def load_records(summary_files: list[Path], phase2_record_globs: list[Path]) -> list[dict]:
    records: list[dict] = []
    for f in summary_files:
        if f.exists():
            records.extend(json.loads(f.read_text(encoding="utf-8")))
    for f in phase2_record_globs:
        if f.exists():
            records.append(json.loads(f.read_text(encoding="utf-8")))
    return records


def by_class(records: list[dict]) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {}
    for r in records:
        out.setdefault(r["reset_class"], []).append(r)
    return out


def boot_field_table(grouped: dict[str, list[dict]]) -> list[dict]:
    rows = []
    for cls, recs in grouped.items():
        observed = [r for r in recs if r.get("rom_boot_raw")]
        a = sum(1 for r in observed if r["rom_boot_raw"] == "0xa")
        b = sum(1 for r in observed if r["rom_boot_raw"] == "0x8")
        other = len(observed) - a - b
        other_values = sorted({r["rom_boot_raw"] for r in observed if r["rom_boot_raw"] not in ("0xa", "0x8")})
        rows.append({
            "reset_class": cls,
            "intended": len(recs),
            "observed": len(observed),
            "0xa": a,
            "0x8": b,
            "other": other,
            "other_values": other_values,
            "pct_0xa": round(100 * a / len(observed), 1) if observed else None,
            "pct_0x8": round(100 * b / len(observed), 1) if observed else None,
        })
    return rows


def phy_by_class_table(grouped: dict[str, list[dict]]) -> list[dict]:
    rows = []
    for cls, recs in grouped.items():
        present = sum(1 for r in recs if r.get("phy_warning_present"))
        rows.append({
            "reset_class": cls,
            "present": present,
            "absent": len(recs) - present,
            "total": len(recs),
            "pct_present": round(100 * present / len(recs), 1) if recs else None,
        })
    return rows


def phy_by_boot_field_table(records: list[dict]) -> list[dict]:
    grouped: dict[str, list[dict]] = {}
    for r in records:
        key = r.get("rom_boot_raw") or "not_observed"
        grouped.setdefault(key, []).append(r)
    rows = []
    for field, recs in sorted(grouped.items()):
        present = sum(1 for r in recs if r.get("phy_warning_present"))
        rows.append({
            "boot_field": field,
            "total": len(recs),
            "warning_count": present,
            "pct_warning": round(100 * present / len(recs), 1) if recs else None,
        })
    return rows


def recovery_stats(grouped: dict[str, list[dict]]) -> list[dict]:
    rows = []
    for cls, recs in grouped.items():
        values = sorted(r["http_recovery_s"] for r in recs if r.get("http_recovery_s") is not None)
        missing = sum(1 for r in recs if r.get("http_recovery_s") is None)
        if not values:
            rows.append({"reset_class": cls, "n": 0, "missing": missing})
            continue
        n = len(values)
        q1 = statistics.quantiles(values, n=4)[0] if n >= 4 else values[0]
        q3 = statistics.quantiles(values, n=4)[-1] if n >= 4 else values[-1]
        iqr = q3 - q1
        lo, hi = q1 - 1.5 * iqr, q3 + 1.5 * iqr
        outliers = [v for v in values if v < lo or v > hi]

        def pct(p):
            if n == 1:
                return values[0]
            k = (n - 1) * p
            f, c = int(k), min(int(k) + 1, n - 1)
            return values[f] + (values[c] - values[f]) * (k - f)

        rows.append({
            "reset_class": cls,
            "n": n,
            "missing": missing,
            "min": round(min(values), 2),
            "median": round(statistics.median(values), 2),
            "mean": round(statistics.mean(values), 2),
            "p95": round(pct(0.95), 2),
            "max": round(max(values), 2),
            "outliers": [round(v, 2) for v in outliers],
        })
    return rows


def duplicate_reset_report(grouped: dict[str, list[dict]]) -> list[dict]:
    rows = []
    for cls, recs in grouped.items():
        dup_total = sum(r.get("duplicate_reset_count", 0) for r in recs)
        rows.append({
            "reset_class": cls,
            "intended_events": len(recs),
            "duplicate_events": dup_total,
            "total_observed_resets": len(recs) + dup_total,
        })
    return rows


def anomaly_report(records: list[dict]) -> list[dict]:
    flags_of_interest = ("panic", "wdt", "brownout", "download_mode", "serial_disconnect_during_event")
    out = []
    for r in records:
        flags = [f for f in flags_of_interest if r.get(f)]
        if r.get("duplicate_reset_count"):
            flags.append(f"duplicate_reset_count={r['duplicate_reset_count']}")
        if r.get("rom_boot_raw") and r["rom_boot_raw"] not in ("0x8", "0xa"):
            flags.append(f"unexpected_boot_field={r['rom_boot_raw']}")
        if r.get("http_recovery_s") is None:
            flags.append("http_recovery_not_observed")
        if flags:
            out.append({"index": r.get("index"), "reset_class": r.get("reset_class"), "flags": flags})
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--summary", action="append", default=[], type=Path,
                     help="A summary.json file (repeatable) from reset_characterization.py")
    ap.add_argument("--phase2-record", action="append", default=[], type=Path,
                     help="A single per-event JSON record (repeatable) from phase2_extract.py")
    args = ap.parse_args()

    if not args.summary and not args.phase2_record:
        print("error: pass at least one --summary or --phase2-record", flush=True)
        return 2

    records = load_records(args.summary, args.phase2_record)
    grouped = by_class(records)

    out = {
        "total_events": len(records),
        "events_per_class": {k: len(v) for k, v in grouped.items()},
        "boot_field_by_class": boot_field_table(grouped),
        "phy_warning_by_class": phy_by_class_table(grouped),
        "phy_warning_by_boot_field": phy_by_boot_field_table(records),
        "recovery_time_stats_by_class": recovery_stats(grouped),
        "duplicate_reset_by_class": duplicate_reset_report(grouped),
        "anomalies": anomaly_report(records),
    }
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
