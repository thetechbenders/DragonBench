# ESP32-S3 target profile

- Target: ESP32-S3, Super Mini class
- Observed silicon revision: v0.2
- Embedded flash: 4 MB
- Embedded PSRAM: 2 MB, Quad SPI
- Observed USB path: USB Serial/JTAG
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

The first hardware flash completed and its image hashes verified over USB
Serial/JTAG, but that initial attempt entered ROM download mode
(`DOWNLOAD(USB/UART0)`, `waiting for download`). After correcting the flash and
PSRAM configuration and fitting the partition table to the physical device, a
fresh flash hard-reset automatically into `SPI_FAST_FLASH_BOOT`. The application
loaded from `ota_0`, detected and successfully tested 2 MB of PSRAM, and reached
the DragonBench `ready` event without manual BOOT-button intervention. It then
remained stable under observation with no panic, watchdog, brownout, or reset.
On-device Wi-Fi/mDNS connectivity, API calls over Wi-Fi, workloads, and
electrical characterization have not yet been validated.

The 4 MB layout uses two 1.5 MB OTA app slots so the inactive-partition workload
has a real target, plus a 512 KB scratch partition. There is no factory app
partition. The exact layout is maintained in `partitions.csv`.
