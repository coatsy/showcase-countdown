# Event bench handoff - 11 September 2026

Event: **17 September 2026, 14:00 UTC+10** (epoch `1789617600`).
These are measured bench results, not full venue sign-off.

## 16 September: public transport and OTA acceptance

Selected unfinished firmware/server work was preserved with binary patches,
verified before rebasing, then restored onto `coatsy/showcase-countdown` main
at `67750ec`. The parent checkout and QoS deployment were not modified.

The attached COM4 device was identified as **52f940**, ESP32-PICO-D4, 4 MB.
Its original single-slot table was migrated to `min_spiffs` through targeted
USB writes without overwriting NVS. Authenticated OTA A-to-B completed and
rebooted into the exact transmitted image; invalid authentication was rejected.
One initial transfer failed for an undetermined reason before the successful
retry. See [the detailed OTA evidence](ota-feasibility.md).

Public acceptance used the actual **MSFT Hack** SSID:

| Measurement | Observed result |
| --- | --- |
| Physical stick address | `172.22.10.117` |
| PC Wi-Fi address | `172.22.10.102` |
| Stick firmware | `67750ec-public-wss-a` |
| Application size / OTA slot | 1,303,360 / 1,966,080 bytes |
| Reported and built application MD5 | `f5397180584319b08c77c5b326e92584` |
| Direct transport | Native certificate-verified `wss://mqtt.cauldnz.org/mqtt` |
| Public broker authentication | Anonymous and incorrect-password connections rejected |
| Public MCP | `initialize` and `tools/list` succeeded on organiser and team routes (21 tools); device `status` succeeded |
| Actual device interaction | Public MCP `show` delivered an eight-second message; the physical stick's serial overlay receipt confirmed delivery through public MQTT |
| Device time | Fresh state reported NTP synchronization |

The AX server was deployed with exactly one explicitly approved event-server
restart. Runtime locks, mutes and cooldowns reset as disclosed. The public
WebSocket listener is loopback-only, with per-device credentials and topic
permissions. Existing dashboard/MCP ingress routes were preserved; only the
MQTT hostname was added to the tunnel.

Venue DNS initially cached NXDOMAIN for the newly created hostname. With
separate approval, the parent cleared only the USG DNS forwarding cache using
its native operational command. The dnsmasq PID/start time stayed unchanged;
no DNS configuration, routing, firewall or QoS settings changed. Final hardware
acceptance used the venue resolver without a DNS override. QoS remained running.

The former MCP invalid-Host rejection is fixed without dropping rebinding
protection. Cloudflare separately rejected Python urllib's default user agent
with error 1010; the actual curl client passed without changing WAF settings.
These results verify this attached device, not a whole-fleet soak or rollback
under power loss. Private firmware contains credentials and must not be published;
ArduinoOTA remains trusted-LAN-only because its image transfer is unencrypted.

## Reproduce the table-stick configuration

`config/event-stick.env` records the configuration used to enable Stick 2's
seven-pixel Grove module: ESP32-PICO-D4, SPK2 HAT, NeoPixel GRB/800 kHz,
GPIO32, seven pixels, local NTP and MQTT at `192.168.8.1`.

Copy it to the root as `.env`, then run `platformio run -e stick`.
The profile's firmware label records the tested source baseline, `d673f01`;
update the label when building another revision. The exact deployed firmware
was built from that baseline with this configuration.

Provision Wi-Fi through Improv serial. The initial provision required serial
`s` (sync) or a restart before NTP/MQTT started; subsequent boots rejoined
automatically. For an application-only update on an already provisioned stick,
write `.pio/build/stick/firmware.bin` at `0x10000`, after checking the USB device's
MAC. Do not write the merged image over existing NVS merely to change LED settings.
Keep the previous application for rollback.

| Role | Device ID | Claim | Bench hardware |
|---|---|---|---|
| Stick 1 / Team 7 | `52f940` | 8734 | SPK2, seven Grove NeoPixels |
| Stick 2 | `533f0c` | 9028 | SPK2, seven Grove NeoPixels, enabled during this session |
| Router ESP-NOW bridge | `52ea2c` | Not a table stick | Router USB serial |
| Virtual demo | `fake01` | 1398 | Explicitly named `VIRTUAL - demo only` |

## Router and clients

- GL-MT3000 on OpenWrt 24.10.3, LAN `192.168.8.1`.
- Dashboard: `http://192.168.8.1:8090/`.
- MQTT: `192.168.8.1:1883`; NTP: `192.168.8.1:123`.
- Participants use the 2.4 GHz `Countdown` AP, channel 6.
- The 5 GHz radio is dedicated to the phone-hotspot uplink; its event AP is
  deliberately disabled. The uplink is DHCP/NAT, not a bridge to office Wi-Fi.
- Local NTP serving is enabled. WAN loss does not immediately remove the
  router's time service, but a cold boot without a trustworthy clock is untested.
- `/etc/showcase/server.env` has `FAKE_STICKS=1`. The demo name is persisted
  in `/etc/showcase/teams.json`; the two physical entries are preserved.
- On the bench PC, the event Ethernet interface metric is 500, so corporate
  Wi-Fi remains preferred for the PC's internet connection.
- Credentials remain in private router/client configuration, not this repository.

The user-level Copilot MCP setup has one stick connection at
`http://192.168.8.1:8090/mcp/8734` and an organiser connection at
`http://192.168.8.1:8090/mcp`, with the private `X-Organiser-Secret` header.
Use a 130000 ms timeout to cover button waits. Reload MCP or start a fresh
client session after configuration changes.

The server now uses stateless Streamable HTTP. With Go MCP SDK v1.7.0,
stateful transport rejected requests carrying
`_meta.io.modelcontextprotocol/protocolVersion`, even after a successful
legacy-version handshake. Regressions cover requests with/without metadata
and per-request organiser authentication.

## Evidence

- Original Stick 1 bench: 63/63 checks; initial Stick 2 no-LED bench: 51/51
  applicable checks. These counts predate the final LED-enabled firmware.
- Physical B-button events received from both table sticks.
- Dashboard Send, live log and organiser lock/unlock exercised against physical
  serial receipts; room restored to unlocked.
- Actual Copilot client invoked stick `status`, organiser `room` and `show`;
  the resulting display command appeared in Stick 1's serial output.
- Both sticks rejoined after broker restart and received display commands.
- Stick 2's short-target finale rehearsal measured +41 microseconds relative
  to its own clock, and 12005 ms duration for the 12000 ms fanfare. This is not
  an independent wall-clock accuracy measurement. Event firmware was restored.
- Router NTP answered three valid samples during a roughly nine-second uplink
  outage. Extended holdover has not been measured.
- The phone uplink recovered after a dropout. Router DNS/HTTPS and HTTPS from
  the PC explicitly bound to event Ethernet passed.
- Stick 1's dark LED was traced to a loose wire by the user's visual retest.
  Stick 2's firmware reported LEDs disabled; an app-only flash enabled them.
  The user confirmed steady green on both modules.
- Final full-shout run: **both** physical serial logs recorded the text,
  `jingle alarm 7 notes, 900 ms`, and `#FFAA00 mode=1 period=600 ttl=15000 ms`.
  Server regression checks enforce the shout alarm's volume of 200/255.

Shouts now publish display, alarm and amber blinking together from one MCP
call. Display and flashing last 15 seconds; the per-team cooldown is 120
seconds by default. Partial publish failures are reported, not called success.

## Still open before the event

- Coatsey's third table stick has not joined the measured fleet. Do not count
  the router bridge or virtual demo as that device.
- Both physical table sticks currently select melody. Full root/fifth/melody
  ensemble, venue range and long-duration soak remain to be done.
- The earlier MQTT receive stall recovered after reboot; its cause is unknown.
- First-time Improv startup needs the sync/reboot workaround above.
- Phone hotspot reconnection can need intervention.
- Router cold boot offline and extended NTP holdover remain untested.
- Restarting the router server clears runtime locks, mutes and cooldown state;
  do not casually deploy/restart close to the event.
