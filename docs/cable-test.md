# Grove soldering bench tester

Standalone firmware for the **M5StickC / Plus / Plus SE**, not StickS3.
Default load: the event's **seven GRB NeoPixels, 800 kHz, data on GPIO32**
(white Grove wire). Red is 5 V and black is ground; follow the actual pin
labels rather than trusting nonstandard cable colours.

1. Inspect the soldering and check for a 5 V-to-ground short with a multimeter
   **before** connecting a new cable. This firmware cannot detect shorts or
   protect the stick against bad wiring.
2. Connect the stick by USB. With Grove power off, connect the cable/module.
3. Press the front **A** button. All seven pixels cycle red, green, blue and
   white every half-second, for eight seconds. Brightness is limited to
   64/255 per channel to avoid a full-power white load.
4. Check every pixel and gently flex the cable: flicker, missing pixels or
   unexpected colours indicate a connection/module problem to investigate.
5. Press **A** again to stop early, or wait for `OFF - swap cable`.
   Grove power is disabled and the data pin is released between tests.
   Remove USB/battery power before rewiring exposed conductors.

There is **no automatic PASS indication**: this is a visual functional test,
not a continuity meter or certification of solder-joint quality.
It uses no Wi-Fi, MQTT, event configuration, speaker or NVS settings.

## Build and flash

```powershell
platformio run -e cable-test
platformio run -e cable-test -t upload --upload-port COM5
```

Replace `COM5` with the port of the stick you intend to use. Uploading replaces
its countdown application; record its identity and keep its event firmware
for restoration. Do not erase the whole flash. This build does not require a
root `.env` or produce a merged image.

Output: `.pio\build\cable-test\firmware.bin`. For an already provisioned event
ESP32-PICO-D4 stick with the same partition layout, an app-only esptool write
at `0x10000` preserves the NVS area. Restore the event application there when
finished.

For different cable assemblies, adjust `CABLE_LED_PIN` (32 or 33) and
`CABLE_LED_COUNT` in the `cable-test` environment in `platformio.ini`.
Do not connect both data pins together. Use a suitable externally powered,
common-ground setup for larger LED loads.
