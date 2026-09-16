# ESP-NOW relay v2: bench candidate

This is a **single-hop payload relay**, not MQTT/TCP over radio, a Wi-Fi extender,
or a multi-hop mesh. It carries the existing display, audio, LED and configuration
JSON down to sticks, and state/button JSON back to the existing MQTT broker.
MCP tool names and claim codes do not change.

The event candidate targets ESP32-PICO-D4 M5StickC/Plus/Plus SE, the event at
**2026-09-17 14:00 UTC+10 (1789617600)**, SPK2 and seven Grove pixels.
The private relay build uses `stick-relay` and keeps `huge_app`, with OTA disabled
in its event profile. Upstream's normal `stick` build now uses the two-slot
`min_spiffs` layout and includes authenticated OTA; that is a separate migration,
not permission to change the existing fleet's partitions.
Build success and host tests are not physical range, timing or delivery acceptance.

## Topology and automatic recovery

```
MCP/dashboard -> local MQTT broker -> table stick (preferred)
                       |
                  Go relay server
                       |
                  USB, 115200 baud
                       |
                  bridge stick -> ESP-NOW -> table stick (fallback)
```

Both bridge and table firmware must contain the same private 32-byte event key.
The relay bridge uses USB for backhaul and parks on `ESPNOW_CHANNEL`; it does
not join Wi-Fi or use stored Wi-Fi credentials. This leaves the phone USB tether
and Countdown AP configuration unchanged.

Table sticks start the radio even without provisioned Wi-Fi. When MQTT is
unavailable they send state/buttons through the bridge and accept its commands.
MQTT is preferred as soon as connected. After disconnection the radio is parked
on the configured channel; a recovery probe is allowed every 60 seconds outside
the finale. An unsuccessful probe is capped at 8 seconds; a connection arriving
near that deadline can take another 3 seconds to meet the stability threshold.
Arduino's channel argument is a scan **hint**, not a hard restriction: RF service
can be interrupted during a probe. Do not promise uninterrupted coverage.

The bridge sends authenticated discovery/time beacons every 10 seconds. A stick
can bootstrap time without Wi-Fi; this is reported as `time_source: "espnow"`,
**not** `ntp: true`. Beacons do not move an NTP-trusted clock or move an already
radio-synced clock backwards. Their second-resolution timestamp is not an
NTP-equivalent synchronisation guarantee. Test the audible finale alignment.
Bridge discovery and server radio presence expire after 45 seconds without
fresh traffic. The server ignores a late direct MQTT LWT while radio state is
fresh, and rejects expired retained radio states after restart.

## Building without changing `.env`

Run from the repository, substituting a private output directory outside it:

```powershell
python scripts\build_event_relay.py --out C:\private\event-relay --pio pio
```

The helper generates `event-relay.private.env`, builds both roles and the Linux
ARM64 Go server, and copies **app-only** images plus a redacted manifest with
hashes into that directory. Use `--go` to name Go explicitly if it is not on PATH.
Reusing the directory reuses the key; it does not silently rotate a deployed
fleet's key. The original `.env` is untouched. Wi-Fi credentials are not compiled
into these candidates; existing NVS provisioning is retained by an app-only update.
`SHOWCASE_ENV_FILE` also selects an explicit profile for ordinary PlatformIO builds.
Default builds leave `ESPNOW_RELAY_ENABLED=false`.

**Keep the private profile, binaries, generated headers, ELF files and merged
images private:** each enabled image contains the event group key. Never attach
them to a public release, issue or pull request. The manifest does not contain it.

## Safe rollout, only after approval

Use a spare bridge and one spare table stick first, away from the live event
server. A USB data cable must enumerate the FTDI serial adapter; enumeration
alone does not establish reliable flashing.

Read and preserve the device's current flash/partition information before
changing it. Confirm ESP32 hardware, 4 MB flash, and the existing application
partition at `0x10000` with size `0x300000`. A full backup contains credentials
and must remain private. Do not assume an unknown device has this layout.
Once approved, write the corresponding **app-only** image at `0x10000`.
Do not use `merged-firmware.bin` or assume a generic PlatformIO upload writes
only the app: a contiguous merged image starting at zero erases the NVS region
with its padding, losing provisioning.

The router also needs the updated Go server. Cross-compile with `GOOS=linux`,
`GOARCH=arm64`, `CGO_ENABLED=0`. Configure `BRIDGE_PORT` to the actual serial port,
or `auto` only when there is exactly one FTDI `0403:6001` adapter. The server
reconnects after unplug/re-enumeration and discards stale queued effects instead
of replaying them. Multiple matching adapters make auto-discovery fail explicitly.
Changing the live server or bridge requires an agreed maintenance window;
server restarts reset runtime policy. The known live port mismatch was
`/dev/ttyUSB0` configured versus `/dev/ttyUSB1` detected: it has **not** been
changed by this development work.

For mixed fleets, the enabled bridge still transmits explicitly requested
legacy `lock`, `unlock`, and `fire` serial commands to old firmware. New relay
sticks use authenticated configuration/time instead of accepting unsigned
legacy packets.

## Delivery bounds and trust

- Frames are 240 bytes, including HMAC-SHA256; at most five 172-byte fragments
  carry a 767-byte JSON payload. Six-hex device IDs are checked against source
  MACs, with a fixed event identifier and destination/topic validation.
- The radio engine has 16 pending sends, eight assemblies, 32 bridge peers and
  64 recent receipts. It retries at 350 ms intervals, at most four attempts,
  with a 2.5-second expiry. Busy/invalid/direct-MQTT replies are explicit failures,
  not proof of execution. Traffic bursts can exceed these bounds.
- Server-generated commands carry a unique `_delivery_id`; identical JSON goes
  to both paths. Firmware remembers the latest 32 IDs for up to 120 seconds.
  This is bounded duplicate suppression, not persistent exactly-once delivery.
  Third-party MQTT commands without IDs cannot obtain cross-path deduplication.
- Radio commands are forwarded only to known online radio-backed physical
  devices; broadcasts expand to those devices. Virtual devices stay on MQTT.
  On registration, the server restores the team and current policy lock,
  never historical display/audio/LED effects. Retained radio replay is config-only.
- A downlink ACK means **admitted to the stick's queue**, not that a speaker,
  display or LED worked, nor that policy permitted the effect. An uplink RF ACK
  means admitted by the bridge and written to serial, not broker persistence.
  Broker/serial errors are surfaced; lost button events are not replayed forever.
- Application HMAC provides integrity and group-key authentication, not
  confidentiality. Any key holder can impersonate another group member.
  Duplicate caches are bounded and lost on reboot; this is not durable replay
  protection against an attacker recording radio traffic. Use only for the
  trusted event fleet, not safety-critical or security-sensitive control.
- The legacy broadcast protocol remains unauthenticated for old firmware.
  Neither version relays attendee internet access.

## Acceptance before fleet flashing

Host regression commands:

```powershell
python tests\test_espnow_relay.py
Set-Location server
go test ./...
go test -race ./...
```

On the spare pair, verify direct MQTT, offline cold boot, radio clock acquisition,
display/audio/LED/config and button round trips, Wi-Fi loss and recovery without
duplicate effects, wrong-key rejection, USB unplug/replug, stale presence expiry,
and team/lock restoration. Test actual speaker/LED output, a 12-stick traffic
burst, memory/sprite stability, and range at the furthest intended table.
Do not change the real event time or broadcast effects to the live room to do
this; use an isolated broker and a separately approved test build where needed.
No physical acceptance or live rollout is implied by these host checks.
