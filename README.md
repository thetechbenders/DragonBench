# DragonBench

> **DRAGONBENCH CHARACTERIZATION IMAGE — NO PRODUCT ACTUATOR SUPPORT**

DragonBench is a Dragon-family lab instrument that generates deterministic
ESP32-S3 module workloads while external equipment measures current, voltage,
rail stability, and reset/brownout behavior.

It is not JumpJet or DragonBreath firmware, a performance benchmark, a generic
HAL, a device-control application, or an authority for product safety limits.
The firmware contains no heater or fan GPIOs and exposes no actuator API.

Version 0.1.0 targets ESP32-S3 N8R8 module hardware: 8 MB embedded flash and
8 MB embedded Octal SPI PSRAM. Browser and CLI clients use the same versioned
HTTP/JSON API. DUT events identify workload boundaries; all voltage/current
evidence remains owned by external instruments.

## Quick start

The ESP32-S3 target is validated with ESP-IDF 5.3.5 and xtensa-esp-elf GCC
13.2.0. That is the current validated component floor, not evidence that older
ESP-IDF releases are incompatible; the minimum supported version has not yet
been established. The external `espressif/mdns` component is pinned to the
validated 1.12.0 release. Configure Wi-Fi without committing credentials:

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

## Validation status

Implemented are the standalone ESP32-S3 characterization firmware, versioned
API, browser UI, host CLI, deterministic workloads, and native host
state-machine tests. The ESP32-S3 image builds with ESP-IDF 5.3.5, and the host
contract and native C tests pass.

Flashing (via an external CH343P USB-UART bridge) and normal application boot
have been validated on the observed N8R8 module. The image boots automatically
from SPI flash, detects and tests the 8 MB Octal SPI PSRAM, and reaches the
DragonBench ready state without manual BOOT-button intervention. On-device
Wi-Fi/mDNS, workload execution, and electrical current or rail
characterization remain unvalidated. A successful build, flash, or boot does
not establish module current demand, rail limits, brownout margin, or product
safety thresholds; those remain external bench evidence.

## Scope rule

A workload belongs here only when it answers a concrete electrical
characterization question for a real target. Unsupported behavior is reported
explicitly; it is never silently skipped.

## Support

If DragonBench is useful to you, you can [support development on
Ko-fi](https://ko-fi.com/opinion_panda).

## License

DragonBench is available under the [MIT License](LICENSE), matching DragonBreath and the wider Dragon-family tooling.
