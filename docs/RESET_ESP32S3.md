# ESP32-S3 N8R8 reset/RF characterization

Scope: a single characterization pass on the observed ESP32-S3-N8R8 module,
exercising boot/reset/PSRAM/RF behavior across independent reset mechanisms.
Firmware under test: `184ff53-dirty` on ESP-IDF 5.3.1. This is a DragonBench
lab-instrument record, not a JumpJet product conclusion; findings here are not
promoted into JumpJet hardware-selection decisions.

## 1. Test matrix

Reset method is treated as an independent variable. The following four
classes were each exercised as a separate category; none are collapsed into
another class merely because the ROM reported the same reset code.

| Reset class | Trials |
|---|---|
| True cold power-on (full USB unplug, wait, replug) | 3 |
| Physical EN/reset button | 5 |
| RTS/DTR reset (serial tooling, matching esptool's hard-reset sequence) | 5 |
| Software restart (`esp_restart()` via the `DB_CONTROLLED_REBOOT` workload over HTTP) | 2 |

Total: 15 resets.

Six reset events were observed during the five reported EN-button presses.
No other reset source was active during that capture window, so each event
reflects a real chip reset. The cause of the extra event was not
independently established. Mechanical switch bounce is one plausible
explanation, but this pass does not establish a root cause. The extra event
remains part of the raw event count below; it is not discarded or
normalized away. All 6 events in that window showed identical
`rst:`/PSRAM/flash behavior, so this does not change any conclusion in this
document.

## 2. Boot / memory result

Confirmed observations, consistent across every trial in all four classes:

- 8 MB flash detected and configured as expected (`SPI Flash Size: 8MB`, DIO
  mode)
- 8 MB Octal SPI PSRAM detected consistently (`esp_psram: Found 8MB PSRAM
  device`)
- PSRAM memory test (`esp_psram: SPI SRAM memory test OK`) passed on every
  observed boot
- no panic
- no watchdog reset (task or interrupt)
- no brownout reset
- no ROM download-mode entry
- no reset required a subsequent full power cycle to recover

This supports a conclusion of repeatable boot and memory initialization
across all four tested reset classes for this pass.

## 3. Reset-reason limitation

- `esp_restart()` produced ROM `rst:0xc (RTC_SW_CPU_RST)` and app-level
  `reset_reason=software`, in both of the 2 software-restart trials.
- True cold power-on, EN-button reset, and RTS/DTR reset all reported ROM
  `rst:0x1 (POWERON)` and app-level `reset_reason=power_on`, with no
  exceptions across the 13 intended (14 raw) non-software trials.
- Therefore the DUT cannot distinguish cold power-on, EN-button reset, and
  RTS/DTR reset from one another using its own reset-reason reporting alone.
  True cold power-on was distinguished only via external evidence: USB
  disconnect and re-enumeration (COM port dropping to "access denied" and
  reappearing), observed on the host, not on the device.

This is recorded as an observability limitation of the reset-reason signal,
not a hardware failure.

## 4. Boot strap-field variance

Raw observation, preserved without a causal claim:

- Across the 14 raw non-software boot events, the ROM `boot:` field's raw
  value split `0xa` × 10 and `0x8` × 4. (RTS/DTR: `0xa`×3, `0x8`×2 over 5
  trials. EN-button window: `0xa`×5, `0x8`×1 over 6 raw events. Cold
  power-on: `0xa`×2, `0x8`×1 over 3 trials.)
- Both raw values decoded to the same printed name, `SPI_FAST_FLASH_BOOT`.
- The variation occurred across all three non-software reset mechanisms
  (cold power-on, button, RTS/DTR) with no fixed pattern tied to any one of
  them.
- Both software-restart (`esp_restart()`) trials reported `boot:0x8`.

The mechanism-independent `0xa`/`0x8` raw boot-field variance is unexplained
and warrants further characterization. A marginal strap condition is one
hypothesis, but no electrical root cause has been established. A larger-N
run and direct electrical observation of the relevant strap pins would be
appropriate follow-up work.

## 5. RF validation

The originally supplied test protocol assumed SoftAP ("SoftAP start result",
scanning for a visible SSID). DragonBench's firmware implements only a
Wi-Fi STA client interface — no AP or Ethernet netif exists in this image.
The RF validation was correctly adapted to DragonBench's actual STA-only
architecture rather than skipped; no SoftAP result is reported because
DragonBench does not implement SoftAP.

Confirmed evidence, from the software-restart trials (the pass in which
Wi-Fi credentials were configured):

- associated to SSID `pinet` using WPA3-SAE, channel 1, BW20
- RSSI approximately -48 dBm and -44 dBm across the two observed
  associations
- DHCP-assigned address `192.168.1.225` both times
- external host (a separate physical device on the same LAN) `ping` to that
  address succeeded (4/4, then confirmed again after the software-restart
  trials)
- external host `arp -a` resolved `192.168.1.225` to MAC `ac:a7:04:e0:f3:5c`
- that ARP-observed MAC matched exactly the STA MAC the DUT's own serial log
  reported for the same session
- `dragonbench.local` resolved via mDNS and served the HTTP API
  (`/api/v1/device`, `/api/v1/status`)
- the HTTP API also responded correctly when addressed directly by IP
- network and HTTP service recovered after each software restart without
  manual intervention

This is sufficient to call STA-mode RF/network operation verified for this
pass.

## 6. PHY calibration warning

The intermittent bootloader/app warning:

```
saving new calibration data because of checksum failure, mode(0)
```

is recorded as an unresolved observation. It appeared in 4 of the 9 boots
captured during the physical-reset session, with no fixed pattern, and was
not correlated to any specific reset mechanism in this pass. It is not
classified as either benign or defective here. This deserves targeted
follow-up if it recurs during additional characterization.

## 7. Logger artifact

Immediately after each of the 3 true cold power-on reconnects, the first
character of the ROM banner as captured by the test logger was garbled (a
single stray leading character prefixed onto an otherwise clean, readable
banner). This occurred only at the 3 reconnect-after-power-loss events and
never during any continuously-held serial connection (all 12 other resets
in this pass). It is documented only as a likely host-side serial-open
timing artifact of the logging script reopening a freshly re-enumerated
port. It is not treated as UART corruption on the device; that would require
reproducing the same corruption while the serial connection remains
continuously open, which has not been observed.

## 8. Controlled-reboot client failure

One follow-up HTTP POST to `/api/v1/runs` (for a third software-restart
trial) failed client-side with a connection-closed error. The test script
retried only ~2 seconds after the prior reboot; the DUT normally required
roughly 3-5 seconds to reassociate to Wi-Fi and restart its HTTP service
after a software restart. A subsequent status check confirmed the DUT had
recovered normally on its own, with no power cycle required. This is
documented as a test-client pacing issue, not a DUT recovery failure, and is
not counted as a reset trial or a reset failure.

## 9. Credential hygiene

Wi-Fi SSID (`pinet`) is recorded above because it is required to interpret
the RF evidence. The Wi-Fi password is not recorded in this document, in any
log intended for commit, in issue text, or in commit messages. Local
`sdkconfig` (which holds the password for this test session) is gitignored
and was verified not staged before any commit involving this document.

## 10. Conclusion

The ESP32-S3-N8R8 completed 15 resets across cold power-on, physical EN
reset, RTS/DTR reset, and software restart without boot, memory, watchdog,
brownout, download-mode, or recovery failures. 8 MB flash and 8 MB Octal
PSRAM initialized consistently. STA-mode Wi-Fi operation was independently
validated end-to-end. Two unresolved observations remain: raw boot-field
variance between `0xa` and `0x8` on non-software resets, and intermittent
PHY calibration checksum warnings. Neither produced an observed functional
failure during this pass.

## Proposed follow-up (not part of the completed-test record above)

- Larger-N repetition of each reset class, with direct electrical
  observation of the ESP32-S3 boot-strap GPIOs, to characterize the
  `0xa`/`0x8` boot-field variance.
- Track the PHY calibration checksum-failure warning across future
  characterization passes to see whether it correlates with any specific
  condition (fresh flash, cold vs. warm boot, elapsed time since last valid
  calibration write).
- If UART corruption on cold-reconnect needs to be ruled out definitively,
  repeat the cold power-on trials with a continuously-held serial connection
  spanning the power loss (e.g., a USB hub or separate always-on UART tap)
  to isolate host-logger artifacts from device-side behavior.
