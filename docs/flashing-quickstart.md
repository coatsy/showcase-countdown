# Flashing checklist - Chris and Coatsey

**Event: 17 September 2026, 14:00 UTC+10. One stick at a time.**
This recipe is for **ESP32-PICO-D4 M5StickC / Plus / Plus SE, 4 MB**.
Stop for an S3 or different flash size. Close serial monitors before flashing.

## 1. Identify the connected stick

Run PowerShell from this repository, using the branch containing this guide:

```powershell
$Pio = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
$Py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$Esp = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$Framework = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32"
& $Pio device list
$Port = "COMx" # Replace with the attached USB port; do not assume COM4.
& $Py $Esp --port $Port --baud 115200 flash_id
```

Record the MAC, chip and flash size. The device ID is the **last six MAC hex
digits**, lowercase, without colons. Match it to the physical stick.

## 2. Prepare its private configuration

Keep a separate ignored `.env.<device-id>` file for each stick, starting from
`config\event-stick.env`. For a public-connected stick, set:

| Setting | Value |
| --- | --- |
| `EVENT_DATETIME` | `"2026-09-17T14:00:00+10:00"` |
| `FIRMWARE_VERSION` | A recognizable label for this build/device |
| `WIFI_SSID`, `WIFI_PASSWORD` | `""` - provision through Improv afterwards |
| `MQTT_URI` | `"wss://mqtt.cauldnz.org/mqtt"` |
| `MQTT_USERNAME` | This stick's six-digit device ID |
| `MQTT_PASSWORD` | Its own private, random 64-hex-character password |
| `NTP_SERVER_1`, `NTP_SERVER_2` | `"0.pool.ntp.org"`, `"1.pool.ntp.org"` |
| `ESPNOW_RELAY_ENABLED` | `false` for the public-MQTT acceptance run |
| `OTA_PASSWORD_HASH` | Matching private OTA hash, or `""` to disable OTA |

**Before flashing:** Chris must add the same device/password pair to the AX
server's private `MQTT_WS_USERS` map. Keep existing entries; batch additions
into one coordinated server restart (runtime locks/mutes/cooldowns reset).
Keep passwords in private configuration, never shared notes or Git.

Check the fitted speaker and LEDs: our SPK2/seven-pixel setup uses `HAT_SPK2`,
GPIO32, seven GRB/800 kHz pixels. Do not enable hardware that is not fitted.
**Never reuse another stick's private binary or MQTT password.**

## 3. Build and install the two-slot firmware

```powershell
$Id = "replace-with-device-id"
$env:SHOWCASE_ENV_FILE = (Resolve-Path ".env.$Id").Path
& $Pio run -e stick
if ($LASTEXITCODE -ne 0) { throw "Build failed; do not flash" }
if ((Get-Item ".pio\build\stick\firmware.bin").Length -gt 1966080) {
    throw "Application exceeds an OTA slot"
}
& $Py $Esp --chip esp32 --port $Port --baud 115200 write_flash `
  --flash_mode dio --flash_freq 40m --flash_size 4MB `
  0x1000 ".pio\build\stick\bootloader.bin" `
  0x8000 ".pio\build\stick\partitions.bin" `
  0xe000 "$Framework\tools\partitions\boot_app0.bin" `
  0x10000 ".pio\build\stick\firmware.bin"
```

Require **successful exit and hash verification**. This deliberately installs
`min_spiffs` and selects application slot 0, even after a previous OTA update.
It leaves NVS at `0x9000-0xDFFF` untouched. Do **not** substitute
`merged-firmware.bin`, erase the chip, or select `stick-relay` (no second OTA slot).

## 4. Join Wi-Fi and check the portal

Use [the installer](https://coatsy.github.io/showcase-countdown/) in a browser
supporting Web Serial: **Connect / configure Wi-Fi only, not Install**, which
would replace the private firmware. Provision the exact SSID **`MSFT Hack`**.
For a LAN-only build using `MQTT_HOST=192.168.8.1`, use **`Countdown`** instead.
If initial provisioning leaves networking idle, send serial `s` at 115200 baud.
Close the monitor before reconnecting the browser or flashing again.

Open [the portal](https://showcase.cauldnz.org): username **showcase**, password
is the event/Countdown Wi-Fi password. Check the correct device is online,
its firmware label matches, and NTP is synchronized. Send it a short display
message and tune; verify the **physical** result. Label the stick with its team.

## Later OTA updates

Only after two-slot USB installation, with matching `.env.ota` credentials:

```powershell
python scripts\upload_ota.py --ip <stick-ip> --host <pc-lan-ip>
```

Use the current device's matching `firmware.bin`. **Trusted LAN only: ArduinoOTA
authenticates but does not encrypt the image.** Do not upload private firmware
over shared venue Wi-Fi or publish it. Verify the new version/image hash after
reboot; a successful upload message alone is not acceptance.
