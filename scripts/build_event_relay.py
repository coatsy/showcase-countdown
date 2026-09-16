"""Build private app-only event candidates and router server; never flash hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="private output directory OUTSIDE the repository")
    parser.add_argument("--pio", default="pio", help="PlatformIO executable")
    parser.add_argument("--go", default="go", help="Go executable for the router server")
    args = parser.parse_args()
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error("--out must be outside the repository: images contain the shared event key")
    out.mkdir(parents=True, exist_ok=True)
    profile = out / "event-relay.private.env"
    revision = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True).strip()
    source_paths = [ROOT / "platformio.ini", ROOT / "scripts" / "load_env.py",
                    ROOT / "scripts" / "merge_firmware.py", ROOT / "config" / "event-stick.env"]
    source_paths += sorted(p for p in (ROOT / "src").rglob("*") if p.is_file() and "cable_test" not in p.parts)
    sources = {p.relative_to(ROOT).as_posix(): sha256(p) for p in source_paths}
    server_paths = list((ROOT / "server").glob("*.go"))
    server_paths += [ROOT / "server" / "go.mod", ROOT / "server" / "go.sum"]
    server_paths += sorted(p for p in (ROOT / "server" / "static").rglob("*") if p.is_file())
    server_sources = {p.relative_to(ROOT).as_posix(): sha256(p) for p in server_paths}
    source_id = hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest()[:12]
    version = f"{revision}-relay-{source_id}"
    # Rebuilding into the same private directory preserves the existing fleet key.
    # Never silently rotate it and strand already-flashed devices.
    if profile.exists():
        entries = {}
        for line in profile.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                entries[k] = v.strip().strip('"')
        key = entries.get("ESPNOW_RELAY_KEY", "")
        if len(key) != 64 or any(c not in "0123456789abcdefABCDEF" for c in key):
            raise SystemExit("Existing private profile has no valid relay key; refusing to rotate it")
    else:
        key = secrets.token_hex(32)
    base = (ROOT / "config" / "event-stick.env").read_text(encoding="utf-8")
    base = "\n".join(line for line in base.splitlines()
                     if not line.startswith(("FIRMWARE_VERSION=", "ESPNOW_RELAY_ENABLED=", "ESPNOW_RELAY_KEY=")))
    profile.write_text(base + f'\nFIRMWARE_VERSION="{version}"\nESPNOW_RELAY_ENABLED=true\n'
                       f'ESPNOW_RELAY_KEY="{key}"\n', encoding="utf-8")
    env = dict(os.environ, SHOWCASE_ENV_FILE=str(profile))
    manifest = {
        "built_at": datetime.now(timezone.utc).isoformat(),
        "status": "bench candidate; physical RF/failover/flash acceptance outstanding",
        "git_revision": revision, "firmware_version": version, "source_sha256": sources,
        "target": "ESP32-PICO-D4 M5StickC/Plus/Plus SE, 4MB",
        "event_epoch": 1789617600, "channel": 6,
        "partition_scheme": "huge_app (unchanged; no OTA slot)",
        "flash_address": "0x10000", "private_key_embedded": True,
        "wifi_credentials_embedded": False, "images": {},
    }
    for role in ("stick", "bridge"):
        environment = "stick-relay" if role == "stick" else role
        with (out / f"{role}-build.log").open("w", encoding="utf-8") as log:
            result = subprocess.run([args.pio, "run", "-e", environment], cwd=ROOT, env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise SystemExit(f"{role} build failed; inspect {out / (role + '-build.log')}")
        build = ROOT / ".pio" / "build" / environment
        image = out / f"{role}-espnow-v2-app.bin"
        shutil.copyfile(build / "firmware.bin", image)
        partitions = (build / "partitions.bin").read_bytes()
        apps = []
        for offset in range(0, len(partitions), 32):
            entry = partitions[offset:offset + 32]
            if len(entry) != 32 or entry[:2] != b"\xaa\x50":
                continue
            if entry[2] == 0:
                apps.append((int.from_bytes(entry[4:8], "little"), int.from_bytes(entry[8:12], "little")))
        if apps != [(0x10000, 3145728)]:
            raise SystemExit(f"Unexpected {role} partition layout; no flash candidate approved")
        if image.stat().st_size > apps[0][1]:
            raise SystemExit(f"{role} app exceeds existing partition")
        manifest["images"][role] = {"file": image.name, "bytes": image.stat().st_size,
                                    "sha256": sha256(image), "partitions_sha256": sha256(build / "partitions.bin")}
        print(f"{role}: {image.stat().st_size} bytes, SHA256 {sha256(image)}", flush=True)
    server = out / "showcase-server-linux-arm64"
    go_env = dict(os.environ, GOOS="linux", GOARCH="arm64", CGO_ENABLED="0")
    with (out / "server-build.log").open("w", encoding="utf-8") as log:
        result = subprocess.run([args.go, "build", "-trimpath", "-o", str(server), "."],
                                cwd=ROOT / "server", env=go_env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"Router build failed; inspect {out / 'server-build.log'}")
    manifest["server"] = {"file": server.name, "bytes": server.stat().st_size, "sha256": sha256(server),
                          "source_sha256": server_sources}
    print(f"router server: {server.stat().st_size} bytes, SHA256 {sha256(server)}", flush=True)
    for name, digest in {**sources, **server_sources}.items():
        if sha256(ROOT / name) != digest:
            raise SystemExit("Source changed during build; rerun before using the candidate")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Private app-only candidates: {out}. Not flashed. Do not publish these files.")


if __name__ == "__main__":
    main()
