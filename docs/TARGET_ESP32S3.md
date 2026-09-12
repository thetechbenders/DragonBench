# ESP32-S3 target profile

- Target: ESP32-S3, N8R8 module class (8 MB Quad SPI flash, 8 MB Octal SPI
  PSRAM)
- Observed silicon revision: v0.2
- Embedded flash: 8 MB, DIO mode, boya-vendor chip as reported by the ROM
  bootloader
- Embedded PSRAM: 8 MB, Octal SPI, AP vendor, generation 3 die, running at
  80 MHz
- Observed USB path: external WCH CH9102 USB-UART bridge (enumerated as a
  generic "USB Serial Device"), not the chip's native USB Serial/JTAG
  peripheral used by the previous Super Mini board
- Validated framework for this characterization run: ESP-IDF 5.3.1 (build,
  flash, and boot exercised on this module using this version)
- Validated compiler: xtensa-esp-elf GCC 13.2.0
- Current target-component constraint: ESP-IDF >=5.3.5; this is the project's
  declared floor and is unchanged by this migration. A successful build on
  5.3.1 for this specific characterization run does not by itself establish
  5.3.1 as the project-supported minimum.
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

Octal SPI PSRAM shares the SPI0/SPI1 clock domain with flash on ESP32-S3, so
`sdkconfig.defaults` pins flash frequency to 80 MHz alongside
`CONFIG_SPIRAM_SPEED_80M`; a mismatched flash frequency is a documented cause
of boot failure on Octal-PSRAM modules. This configuration has now been
exercised on physical hardware: the device booted automatically into
`SPI_FAST_FLASH_BOOT`, detected the 8 MB Octal PSRAM device, and passed the
ESP-IDF SPI SRAM memory test without manual BOOT-button intervention.

USB flashing (via the external CH9102 bridge) and normal application boot
have been validated on this module. The image flashed with
verified hashes, hard-reset automatically, detected and successfully tested
the 8 MB PSRAM, and reached the DragonBench `ready` event with no panic,
watchdog, or brownout reset observed. On-device Wi-Fi/mDNS connectivity, API
calls over Wi-Fi, workloads, and electrical characterization remain
unvalidated on this module.

The 8 MB layout uses two 3 MB OTA app slots so the inactive-partition workload
has a real target, plus a 1 MB scratch partition. There is no factory app
partition. The exact layout is maintained in `partitions.csv`.
