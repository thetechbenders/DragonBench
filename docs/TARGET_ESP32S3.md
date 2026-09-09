# ESP32-S3 target profile

- Target: ESP32-S3, Super Mini class
- Validated framework: ESP-IDF 5.3.5
- Validated compiler: xtensa-esp-elf GCC 13.2.0
- Current target-component constraint: ESP-IDF >=5.3.5; this records the
  validated floor and does not prove lower releases are incompatible
- Minimum historically compatible ESP-IDF version: not established
- External component: espressif/mdns 1.12.0, exact pin
- Hostname: `dragonbench`
- Actuators: absent
- On-device electrical measurement: absent
- Available DUT signals: uptime, reset reason, network state, Wi-Fi RSSI when
  associated, workload/run state, and on-die temperature when the ESP-IDF
  temperature-sensor driver initializes successfully
- External evidence: voltage and current measurements from bench instruments

Wi-Fi credentials are local configuration. The target does not claim supply
voltage/current sensing. Brownout evidence is limited to the reset reason
reported after boot; it is not a calibrated voltage measurement.

DragonBench creates one Wi-Fi station interface and no AP or Ethernet netif.
Accordingly, `sdkconfig.defaults` enables only the predefined mDNS STA interface.
It allocates two mDNS entries because mDNS 1.12.0's duplicate-interface logic
requires a two-entry array even for one predefined netif; this is implementation
capacity, not a second DragonBench interface. Generated `sdkconfig` and
`managed_components/` remain local; the Component Manager lockfile is tracked
to preserve the dependency graph used by the validated build.

Native build validation does not constitute a hardware flash or electrical
characterization result. Neither has been performed for v0.1.0.
