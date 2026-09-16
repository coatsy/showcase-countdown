"""Upload an application over trusted-LAN ArduinoOTA without exposing its password."""

import argparse
import importlib.util
import logging
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ip", required=True, help="stick IPv4 address")
    parser.add_argument("--host", required=True, help="PC IPv4 address reachable from the stick")
    parser.add_argument("--port", type=int, default=32320, help="PC TCP callback port")
    parser.add_argument("--image", type=Path, default=Path(".pio/build/stick/firmware.bin"))
    parser.add_argument("--credentials", type=Path, default=Path(".env.ota"))
    parser.add_argument(
        "--espota", type=Path,
        default=Path.home() / ".platformio/packages/framework-arduinoespressif32/tools/espota.py",
    )
    args = parser.parse_args()
    image = args.image.read_bytes()
    if not image or image[0] != 0xE9 or len(image) > 0x1E0000:
        parser.error("Expected an application image fitting a min_spiffs OTA slot (1966080 bytes)")
    if "merged" in args.image.name.lower():
        parser.error("Never upload a merged flash image over OTA")
    credentials = {}
    for line in args.credentials.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            credentials[key.strip()] = value.strip()
    password = credentials.get("OTA_PASSWORD", "")
    if len(password) < 32:
        parser.error("Private credential file must contain OTA_PASSWORD (at least 32 characters)")
    spec = importlib.util.spec_from_file_location("espota", args.espota)
    if spec is None or spec.loader is None:
        parser.error("Unable to load the installed ArduinoOTA uploader")
    espota = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(espota)
    espota.TIMEOUT = 3
    # Retain uploader status/errors without its per-kilobyte progress noise.
    espota.update_progress = lambda _: None
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    print("ArduinoOTA authenticates but does NOT encrypt the image. Use a trusted LAN only.", flush=True)
    return espota.serve(args.ip, args.host, 3232, args.port, password, str(args.image), espota.FLASH)


if __name__ == "__main__":
    raise SystemExit(main())
