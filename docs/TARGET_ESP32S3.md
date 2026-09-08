# ESP32-S3 target profile

- Target: ESP32-S3, Super Mini class
- Framework: ESP-IDF 5.4+
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
