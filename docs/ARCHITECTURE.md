# Architecture

DragonBench has three deliberately narrow boundaries:

- `firmware/common`: workload names, run lifecycle, sequencing, and capability
  semantics. It knows nothing about GPIOs or ESP-IDF drivers.
- `firmware/targets/esp32s3`: direct ESP-IDF implementations for Wi-Fi, mDNS,
  HTTP, reset reason, SoC temperature, NVS, flash, inactive-OTA writes, reboot,
  and TCP traffic. No other target backend exists.
- `cli`, `web`, and `protocol`: clients and the stable public contract. They do
  not have privileged behavior paths.

There is intentionally no universal embedded HAL. A future target may implement
the protocol and workload semantics directly when a concrete need exists.

The v1 firmware has no dependency on dragon-core. Reusing it would currently add
product-oriented surface area without reducing the small target implementation.
This decision can be revisited for a specific neutral service, with its commit
pin and scope documented before adoption.
