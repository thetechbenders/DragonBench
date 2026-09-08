"""Dependency-free DragonBench HTTP/JSON client."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


class ClientError(RuntimeError):
    pass


class Client:
    def __init__(self, host: str, timeout: float = 10.0):
        self.base = host if "://" in host else f"http://{host}"
        self.base = self.base.rstrip("/")
        self.timeout = timeout

    def request(self, method: str, path: str, body: dict[str, Any] | None = None) -> Any:
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(
            self.base + path,
            data=data,
            method=method,
            headers={"Accept": "application/json", "Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as response:
                raw = response.read().decode()
                if response.headers.get_content_type() == "application/x-ndjson":
                    return [json.loads(line) for line in raw.splitlines() if line]
                return json.loads(raw) if raw else {}
        except (urllib.error.URLError, json.JSONDecodeError) as exc:
            raise ClientError(str(exc)) from exc


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="dragonbench")
    p.add_argument("--host", default="dragonbench.local")
    p.add_argument("--json", action="store_true", dest="machine")
    p.add_argument("--timeout", type=float, default=10.0)
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("discover")
    sub.add_parser("status")
    sub.add_parser("sensors")
    sub.add_parser("workloads")
    run = sub.add_parser("run")
    run.add_argument("workload")
    run.add_argument("--duration", type=float, default=30.0)
    run.add_argument("--target-host")
    run.add_argument("--port", type=int)
    run.add_argument("--rate", type=int, default=0)
    abort = sub.add_parser("abort")
    abort.add_argument("run_id")
    sub.add_parser("events")
    export = sub.add_parser("export")
    export.add_argument("run_id", nargs="?", default="latest")
    export.add_argument("--output", type=Path)
    traffic = sub.add_parser("traffic-peer")
    traffic.add_argument("--listen", default="0.0.0.0")
    traffic.add_argument("--port", type=int, required=True)
    traffic.add_argument("--mode", choices=("sink", "source", "echo"), required=True)
    traffic.add_argument("--duration", type=float, default=60.0)
    return p


def traffic_peer(bind_host: str, port: int, mode: str, duration: float) -> dict[str, Any]:
    if not 1 <= port <= 65535 or duration <= 0 or duration > 3600:
        raise ClientError("traffic peer requires port 1..65535 and duration >0..3600 seconds")
    tx = rx = 0
    block = bytes((i % 251 for i in range(4096)))
    deadline = time.monotonic() + duration
    with socket.socket(socket.AF_INET6 if ":" in bind_host else socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((bind_host, port))
        server.listen(1)
        server.settimeout(duration)
        conn, peer = server.accept()
        with conn:
            conn.settimeout(1.0)
            while time.monotonic() < deadline:
                try:
                    if mode in {"sink", "echo"}:
                        data = conn.recv(len(block))
                        if not data:
                            break
                        rx += len(data)
                        if mode == "echo":
                            conn.sendall(data)
                            tx += len(data)
                    else:
                        conn.sendall(block)
                        tx += len(block)
                except socket.timeout:
                    continue
    return {"peer": peer[0], "mode": mode, "bytes_tx": tx, "bytes_rx": rx}


def execute(args: argparse.Namespace, client: Client) -> Any:
    if args.command == "traffic-peer":
        return traffic_peer(args.listen, args.port, args.mode, args.duration)
    if args.command == "discover":
        try:
            addresses = sorted({item[4][0] for item in socket.getaddrinfo(args.host, 80)})
        except socket.gaierror as exc:
            raise ClientError(f"mDNS/DNS discovery failed for {args.host}: {exc}") from exc
        return {"hostname": args.host, "addresses": addresses}
    if args.command in {"status", "sensors", "workloads"}:
        return client.request("GET", f"/api/v1/{args.command}")
    if args.command == "run":
        if args.duration <= 0 or args.duration > 3600:
            raise ClientError("duration must be >0 and <=3600 seconds")
        if args.port is not None and not 1 <= args.port <= 65535:
            raise ClientError("port must be 1..65535")
        body: dict[str, Any] = {
            "workload": args.workload.upper(),
            "duration_ms": int(args.duration * 1000),
            "rate_bps": args.rate,
        }
        if args.target_host:
            body["host"] = args.target_host
        if args.port is not None:
            body["port"] = args.port
        return client.request("POST", "/api/v1/runs", body)
    if args.command == "abort":
        return client.request("POST", f"/api/v1/runs/{args.run_id}/abort", {})
    if args.command == "events":
        return client.request("GET", "/api/v1/events")
    if args.command == "export":
        run_id = args.run_id
        if run_id == "latest":
            status = client.request("GET", "/api/v1/status")
            run_id = status.get("last_run_id") or status.get("run_id")
            if not run_id:
                raise ClientError("device has no run to export")
        payload = {
            "source": "dut_reported",
            "run": client.request("GET", f"/api/v1/runs/{run_id}"),
            "events": client.request("GET", "/api/v1/events"),
            "external_measurements": None,
        }
        if args.output:
            args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
            return {"written": str(args.output), "run_id": run_id}
        return payload
    raise ClientError("unknown command")


def render(value: Any, machine: bool) -> str:
    if machine:
        return json.dumps(value, separators=(",", ":"), sort_keys=True)
    return json.dumps(value, indent=2, sort_keys=True)


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        result = execute(args, Client(args.host, args.timeout))
    except ClientError as exc:
        print(json.dumps({"error": str(exc)}) if args.machine else f"error: {exc}", file=sys.stderr)
        return 2
    print(render(result, args.machine))
    return 0
