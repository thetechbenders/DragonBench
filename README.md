# DragonBench

> **DRAGONBENCH CHARACTERIZATION IMAGE — NO PRODUCT ACTUATOR SUPPORT**

DragonBench is a Dragon-family lab instrument that generates deterministic
ESP32-S3 module workloads while external equipment measures current, voltage,
rail stability, and reset/brownout behavior.

It is not JumpJet or DragonBreath firmware, a performance benchmark, a generic
HAL, a device-control application, or an authority for product safety limits.
The firmware contains no heater or fan GPIOs and exposes no actuator API.

Version 0.1.0 builds for two ESP32-S3 board profiles, both with 8 MB flash and
8 MB PSRAM: the N8R8 module (Octal SPI PSRAM, CH343P USB-UART bridge; the
default) and the Unexpected Maker TinyS3[D] (Quad SPI PSRAM, native USB
Serial/JTAG, onboard/U.FL RF switch). Browser and CLI clients use the same
versioned HTTP/JSON API. DUT events identify workload boundaries; all
voltage/current evidence remains owned by external instruments.

## Quick start

The ESP32-S3 target is validated with ESP-IDF 5.3.5 and xtensa-esp-elf GCC
13.2.0. That is the current validated component floor, not evidence that older
ESP-IDF releases are incompatible; the minimum supported version has not yet
been established. The external `espressif/mdns` component is pinned to the
validated 1.12.0 release.

The default profile is the N8R8 module:

```text
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

For the TinyS3[D], layer its overlay on the shared defaults. The PSRAM line
mode differs between the boards, and the wrong one aborts at boot with
`octal_psram: PSRAM chip is not connected, or wrong PSRAM line mode`:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tinys3d" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tinys3d" build
```

Delete a generated `sdkconfig` before switching profiles; existing values take
precedence over the defaults files. The TinyS3[D] has no auto-reset circuit on
its native USB port: hold BOOT, tap RESET, release BOOT, then flash with
`python -m esptool --chip esp32s3 -p COMx --before no_reset write_flash "@flash_args"`
from the `build` directory, and unplug/replug USB to boot the new image.

Every board brings up its own WPA2 access point, `DragonBench-XXXXXX` (from the
MAC), at `http://192.168.4.1/`; its password is `DragonBench -> Direct
access-point password`. To also join a 2.4 GHz network, open
`http://192.168.4.1/setup` or set `DragonBench -> Optional station Wi-Fi SSID`
and password in a local, ignored `sdkconfig`. Credentials entered on the setup
page persist in NVS and take precedence over Kconfig.

Each board advertises its own mDNS name on both interfaces, derived from the
same MAC suffix as its access point: `DragonBench-D685F0` answers as
`http://dragonbench-d685f0.local/`. Several boards can share one network
without a client silently reaching the wrong one. `DragonBench -> mDNS
hostname` overrides the name; a generated `sdkconfig` from before this change
keeps the old shared `dragonbench` name until that value is cleared.
`/api/v1/status` reports the name mDNS actually holds as
`network.mdns_hostname`.

Host CLI, with Python 3.10+ and no third-party dependencies. `--host` defaults
to `192.168.4.1`, the access point of whichever board you are joined to; on a
shared network, name the board:

```text
python -m cli.dragonbench --host dragonbench-d685f0.local status
python -m cli.dragonbench --host dragonbench-d685f0.local run CPU_STRESS --duration 60
python -m cli.dragonbench --json --host dragonbench-d685f0.local events
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

On the TinyS3[D], flash, boot, the 8 MB Quad SPI PSRAM test, the direct access
point, station association and DHCP on a home network, station credentials
saved through `/api/v1/network/sta` persisting in NVS across reflashes, and
mDNS on both interfaces have been validated. The `/setup` page itself has not
yet been exercised on hardware, and workload execution and electrical
characterization remain unvalidated there too. See
[TinyS3[D] profile](docs/TARGET_TINYS3D.md).

## Scope rule

A workload belongs here only when it answers a concrete electrical
characterization question for a real target. Unsupported behavior is reported
explicitly; it is never silently skipped.

## Support

If DragonBench is useful to you, you can [support development on
Ko-fi](https://ko-fi.com/opinion_panda).

## License

DragonBench is available under the [MIT License](LICENSE), matching DragonBreath and the wider Dragon-family tooling.
