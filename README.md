# DragonBench

> **DRAGONBENCH CHARACTERIZATION IMAGE — NO PRODUCT ACTUATOR SUPPORT**

DragonBench is a Dragon-family lab instrument that generates deterministic
ESP32-S3 module workloads while external equipment measures current, voltage,
rail stability, and reset/brownout behavior.

It is not JumpJet or DragonBreath firmware, a performance benchmark, a generic
HAL, a device-control application, or an authority for product safety limits.
The firmware contains no heater or fan GPIOs and exposes no actuator API.

Version 0.1.0 targets the ESP32-S3 Super Mini class only. Browser and CLI clients
use the same versioned HTTP/JSON API. DUT events identify workload boundaries;
all voltage/current evidence remains owned by external instruments.

## Quick start

ESP-IDF 5.4 or newer is expected. Configure Wi-Fi without committing credentials:

```text
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

Set `DragonBench -> Wi-Fi SSID` and `Wi-Fi password`, or supply the corresponding
Kconfig values in a local, ignored `sdkconfig`. The device advertises
`dragonbench.local` over mDNS and serves the UI at `http://dragonbench.local/`.

Host CLI, with Python 3.10+ and no third-party dependencies:

```text
python -m cli.dragonbench --host dragonbench.local status
python -m cli.dragonbench --host dragonbench.local run CPU_STRESS --duration 60
python -m cli.dragonbench --json --host dragonbench.local events
```

Network workloads use an explicit external TCP peer. Start one of these before
the matching DUT run: `sink` for `NET_TX`, `source` for `NET_RX`, or `echo` for
`NET_BIDIRECTIONAL`:

```text
python -m cli.dragonbench traffic-peer --port 5001 --mode echo --duration 60
```

See [bench workflow](docs/BENCH_WORKFLOW.md), [target profile](docs/TARGET_ESP32S3.md),
and [protocol contract](protocol/openapi.yaml).

## Scope rule

A workload belongs here only when it answers a concrete electrical
characterization question for a real target. Unsupported behavior is reported
explicitly; it is never silently skipped.
