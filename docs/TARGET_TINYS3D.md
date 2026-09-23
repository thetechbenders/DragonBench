# TinyS3[D] target profile

- Board: Unexpected Maker TinyS3[D], schematic revision D-P1
- SoC: ESP32-S3FN8 (8 MB embedded flash, no in-package PSRAM), silicon v0.2
- PSRAM: separate 8 MB Quad SPI chip; the board has no SIO4–SIO7 wiring, so
  Octal mode cannot work
- Flash: 8 MB, DIO, XMC vendor as reported by esptool
- USB: the chip's native USB Serial/JTAG peripheral (VID:PID `303A:1001`); there
  is no USB-UART bridge and no auto-reset circuit
- RF: a BGS12 switch selects the onboard antenna or the U.FL connector, driven
  by GPIO38 (low: onboard; high: U.FL)
- Profile overlay: `sdkconfig.defaults.tinys3d`, reported target
  `esp32s3-tinys3d`
- Validated framework: ESP-IDF 5.3.5, xtensa-esp-elf GCC 13.2.0

Build by layering the overlay on the shared defaults:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tinys3d" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tinys3d" build
```

Building the N8R8 profile for this board aborts at every boot with
`octal_psram: PSRAM chip is not connected, or wrong PSRAM line mode`.

To flash, hold BOOT, tap RESET, and release BOOT so the ROM downloader
enumerates, then run from `build/`:

```text
python -m esptool --chip esp32s3 -p COMx --before no_reset write_flash "@flash_args"
```

esptool's post-flash RTS reset has no effect on this port. Unplug and replug
USB to boot the new image. The default `--before default_reset` failed with
`Write timeout` while CircuitPython was running. Boot logs appear on the
same native USB port only during startup, so attach a monitor before
power-cycling.

## RF switch

The firmware drives GPIO38 low before Wi-Fi starts
(`CONFIG_DB_RF_SWITCH_GPIO`). Before it did, every link on this board was
marginal: station association to a home network timed out
(`assoc -> init (0x400)`), phones
on the direct access point cycled through SA Query disassociation and WPA2
handshake failures, and repeatedly re-requested DHCP leases. A scan of the same
networks with each switch position, with nothing attached to the U.FL
connector, measured:

| GPIO38 | Networks seen | Strongest RSSI |
|---|---|---|
| high (U.FL, unterminated) | 4 | -86 dBm |
| low (onboard antenna) | 7 | -56 dBm |

With GPIO38 driven low, the board associated with the same home network and
obtained a DHCP lease on the first attempt. The pin level before the firmware
drove it was not measured, so why the 100 kΩ pull-down did not hold it low is
unconfirmed.

## Validation status

Flash, automatic boot from SPI flash, the ESP-IDF PSRAM memory test (8 MB added
to the heap), the `ready` event, the direct access point, station association
and DHCP on a home network, station credentials saved through
`/api/v1/network/sta` persisting in NVS across reflashes, and mDNS on both
interfaces have been validated. The `/setup` page, workload execution, and
electrical characterization have not.
