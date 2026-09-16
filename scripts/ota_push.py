"""Push the built application image to one ESP32 over ArduinoOTA.

The dashboard calls this once per selected IP through OTA_RUNNER. The password
is read from OTA_PASSWORD or the private .env.ota file and is never printed.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ENV_OTA = ROOT / ".env.ota"
DEFAULT_PIO_PYTHON = Path.home() / ".platformio" / "penv" / "Scripts" / "python.exe"
DEFAULT_ESPOTA = (
    Path.home()
    / ".platformio"
    / "packages"
    / "framework-arduinoespressif32"
    / "tools"
    / "espota.py"
)


def read_env_value(path: Path, key: str) -> str:
    if not path.is_file():
        return ""
    pattern = re.compile(r"^(?:export\s+)?" + re.escape(key) + r"\s*=\s*(.*?)\s*$")
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line.strip())
        if not match or not match.group(1):
            continue
        value = match.group(1).strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        return value
    return ""


def password() -> str:
    value = os.environ.get("OTA_PASSWORD", "") or read_env_value(ENV_OTA, "OTA_PASSWORD")
    if not value:
        raise RuntimeError("OTA password not found; set OTA_PASSWORD or create .env.ota")
    return value


def find_file(explicit: str, candidates: list[Path], label: str) -> Path:
    if explicit:
        path = Path(explicit).expanduser()
        if not path.is_absolute():
            path = ROOT / path
        if path.is_file():
            return path
        raise RuntimeError(f"{label} not found: {path}")
    for path in candidates:
        if path.is_file():
            return path
    raise RuntimeError(f"{label} not found; build the firmware first")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, help="device IP address or hostname")
    parser.add_argument("--environment", choices=("stick", "sticks3"), default="stick")
    parser.add_argument("--file", help="application image; defaults to the selected PlatformIO build")
    parser.add_argument("--espota", help="path to espota.py")
    parser.add_argument("--python", help="Python interpreter used to run espota.py")
    parser.add_argument("--port", type=int, default=3232)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--dry-run", action="store_true", help="print the command without uploading")
    args = parser.parse_args()

    image = find_file(
        args.file,
        [ROOT / ".pio" / "build" / args.environment / "firmware.bin"],
        "firmware image",
    )
    espota = find_file(args.espota, [DEFAULT_ESPOTA], "espota.py")
    python = find_file(args.python, [DEFAULT_PIO_PYTHON, Path(sys.executable)], "Python interpreter")
    auth = password()
    command = [
        str(python),
        str(espota),
        "--ip",
        args.target,
        "--port",
        str(args.port),
        "--auth",
        auth,
        "--file",
        str(image),
        "--timeout",
        str(args.timeout),
        "--progress",
    ]
    if args.dry_run:
        display = command.copy()
        display[display.index("--auth") + 1] = "<redacted>"
        print(" ".join(display))
        return 0

    result = subprocess.run(command, cwd=ROOT)
    return result.returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"ota_push: {error}", file=sys.stderr)
        raise SystemExit(2)
