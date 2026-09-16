# OTA feasibility, 14 September 2026

## Measured bench update, 16 September

The attached COM4 device was identified from its eFuse MAC as **52f940**,
ESP32-PICO-D4 revision 1.1 with 4 MB flash. Reading its partition table showed
`huge_app`, not an existing two-slot layout. With the user's authorization,
targeted USB writes installed the bootloader, `min_spiffs` partition table,
OTA selector and application, leaving NVS at `0x9000` through `0xDFFF` untouched.
No merged image or full-chip erase was used, and no device backup was made.

Firmware A reported OTA ready at `192.168.8.119`. An invalid authentication
response was rejected before image transfer. The first authenticated transfer
failed; its cause is unknown. A diagnostic retry completed, rebooted, and
fresh MQTT state confirmed firmware `67750ec-ota-bench-b` with MD5
`69573aaef4e93081d3da965157119b1c`, exactly matching the transmitted application.
This is successful authenticated OTA, **not** a proven power-loss/rollback test.

The two OTA slots are `0x10000` and `0x1F0000`, each 1,966,080 bytes.
The OTA test application was 1,237,504 bytes. The subsequent TLS/WebSocket
candidate was 1,303,360 bytes; measure again after each build.

Use `scripts/setup_ota.ps1` with a private `.env` to create/reuse `.env.ota`,
then build `stick`. With the PC and stick on a trusted LAN:

```powershell
python scripts\upload_ota.py --ip <stick-ip> --host <pc-lan-ip>
```

The wrapper reads the password privately instead of putting it in process
arguments, and accepts application images only. ArduinoOTA uses UDP 3232 and
a device-to-PC TCP callback (default 32320). Do not expose either port publicly.
It is authenticated **but unencrypted**: never send credential-bearing firmware
over shared venue Wi-Fi. An interrupted transfer must be followed by a live
version/hash check; an uploader success alone is not acceptance.

After an OTA update, do not assume the active application remains at `0x10000`.
For a deliberate USB reinstall, either establish the active slot or explicitly
reset the OTA selector with `boot_app0.bin` and install slot 0, leaving NVS alone.
The private `stick-relay` profile still uses `huge_app` and cannot accept OTA.

## Earlier investigation (historical)

**16 September rebase note:** this investigation records the pre-OTA baseline.
Upstream now includes authenticated ArduinoOTA and uses `min_spiffs` for normal
stick builds. The private relay helper instead selects `stick-relay`, retaining
`huge_app` and its existing app-only rollout contract. No device migration or
physical OTA acceptance has been performed by this rebase.

**Recommendation: keep tonight's `huge_app` layout and use app-only USB updates.**
Prototype Wi-Fi OTA on a spare after explicit approval for a partition migration.
Do not add an ESP-NOW firmware transfer protocol before the event.

## Current evidence

The PICO targets use ESP32 Arduino 2.0.17 (PlatformIO framework package
3.20017.241212+sha.dcc1105b), based on IDF 4.4.7. The installed `huge_app`
partition table has one application slot of 3,145,728 bytes and no second slot.
Both candidate builds preserve it. Simply adding ArduinoOTA does not create
safe update storage.

The enabled relay candidates measured 1,200,480 bytes for the table app and
1,082,368 bytes for the bridge app. For subsequent rebuilds, use the private
`manifest.json` rather than treating these measurements as fixed sizes.
Older pre-relay artifacts measured 1,180,944 and 1,059,152 bytes respectively.

Installed partition alternatives:

| Scheme | App slots | Bytes per slot |
| --- | --- | --- |
| `huge_app` | 1 | 3,145,728 |
| `default` | 2 | 1,310,720 |
| `min_spiffs` | 2 | 1,966,080 |

`min_spiffs` is the sensible spare-device prototype: materially more room than
`default`, with two update slots. These layouts share NVS at `0x9000`/`0x5000`,
OTA data at `0xE000`/`0x2000`, and the first app at `0x10000`. That does not make
an arbitrary migration credential-preserving: establish what is on each device,
back up privately, and write only the intended regions. No filesystem use was
found during inspection, but existing flash contents must not be assumed disposable.

## Hazards established from the installed sources

- OTA uses `firmware.bin`, **never** a merged image containing bootloader and
  partition data. Observed merged images pad across NVS with `0xFF`; writing
  them contiguously from zero erases credentials even without a full-chip erase.
- Generic PlatformIO upload may include bootloader, partitions and `boot_app0`.
  Use an explicitly reviewed migration/app-only command rather than assuming it
  preserves settings.
- IDF's next-update-partition selection can return the only application slot
  when no alternative exists. Arduino 2.0.17's `Update.begin()` does not itself
  reject the currently running partition. An updater must explicitly require
  a distinct, suitable target slot before beginning.
- Arduino's default weak rollback hooks can validate an update before the
  application proves health (`verifyOta()` returns true and
  `verifyRollbackLater()` returns false). Defer confirmation and require bounded
  application checks plus a watchdog; power-cycle and rollback-test the result.
- SDK rollback options were enabled in the inspected configuration; secure
  boot, signed-app verification, anti-rollback and flash encryption were not.
  Build configuration is not proof of what bootloader is currently on a device.

Inspected sources included installed `tools/partitions/*.csv`,
`libraries/Update/src/Updater.cpp`, `cores/esp32/esp32-hal-misc.c`,
`cores/esp32/main.cpp`, `tools/sdk/esp32/sdkconfig`, and `tools/platformio-build.py`.

## Future implementation requirements

Use authenticated update initiation, trusted HTTPS and certificate validation,
trusted length/hash metadata, hardware/role/version checks, and slot-size bounds.
MD5 is an integrity check, not origin authentication; stock ArduinoOTA examples
do not enable authentication by default. Keep event control and software-update
authority separate.

Start with Wi-Fi download to the inactive app slot, then reboot into a trial
image. Confirm only after display/input, saved configuration and the selected
transport pass a bounded health check; a device intentionally operating offline
must not be rejected solely for lacking Wi-Fi. Prove rollback on bad images,
failed health checks and interrupted updates.

At 200 application bytes per fragment, even the older table image needs roughly
5,905 fragments before retransmissions. ESP-NOW OTA would additionally need
resumable transfer, flash-write scheduling, throttling, metadata verification,
reboot coordination and room-load tests. The command relay's small JSON queue is
not an OTA transport. This is separate work, not a firmware flag.

No updater, partition migration, hardware flash or live networking change was
performed for this investigation.

## Primary references

- [ESP-IDF 4.4.7 OTA API and rollback](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/system/ota.html)
- [ESP-IDF partition tables](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-guides/partition-tables.html)
- [ESP HTTPS OTA](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/system/esp_https_ota.html)
- [ESP-NOW limits](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/network/esp_now.html)
- [IDF 4.4.7 update implementation](https://github.com/espressif/esp-idf/blob/v4.4.7/components/app_update/esp_ota_ops.c)
- [Arduino 2.0.17 BasicOTA example](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/ArduinoOTA/examples/BasicOTA/BasicOTA.ino)
- [esptool flash erase behaviour](https://docs.espressif.com/projects/esptool/en/release-v4/esp32/esptool/basic-commands.html#erasing-flash-before-write)
