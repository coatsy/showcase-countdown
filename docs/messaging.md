# Messaging: the room as an instrument

About a dozen sticks sit on team tables at a coding event. Besides the countdown
they can show messages, play audio, and drive their LED. Teams control their own
stick through an MCP server with their agents, can shout to the whole room, and
can message other teams.

Status: protocol reference. Decisions are recorded in [plan.md](plan.md).

## Architecture

```
team agent (Claude Code / Copilot / Cursor / ...)
        |  MCP over streamable HTTP; the claim code identifies the team
        v
MCP server (Go, on the GL-MT3000)  -- all social rules live here
        |  MQTT
        v
Mosquitto on the GL-MT3000        -- also the LAN NTP server
        |  MQTT over 2.4 GHz WiFi
        v
12 x M5StickC Plus SE
```

Principles:

- **Devices are dumb.** A stick knows five things: display, audio, LED, its own
  state, and button events. It never knows what a team, a shout, or a DM is.
  Every social feature is a display command with a `from` field.
- **The MCP server owns policy.** Team identity, device binding, cooldowns, rate
  limits, the room lock, and the organiser override are all one process.
- **The router hosts what the room depends on.** Broker, NTP, MCP server, and
  optionally an ESP-NOW bridge. See issue #1 for the router work.

## Device identity and binding

- Device id is the last three bytes of the eFuse MAC in lower-case hex, e.g.
  `52f940`. It is printed in the boot banner and on the diagnostics screen.
- Every stick shows a four-digit claim code in a strip under the countdown, all
  day. **The code is the credential.** Everyone at a table configures their
  agent with the same code; the server treats the code as the team. There are
  no tokens and no registration.
- `claim(code, team_name)` names the team and pushes the name to the stick.
  Moving a stick between tables is a re-claim. Mischief between tables is
  accepted; the organiser has `mute`.
- The code is derived from the device id and a per-build salt (`CLAIM_SALT` in
  `.env`), so it is stable across reboots and not guessable from the id.
- Organiser tools need the organiser secret, not a code.

## MQTT topics

Prefix `showcase/`. Payloads are JSON. Device ids as above.

| Topic | Direction | Retained | Purpose |
| --- | --- | --- | --- |
| `showcase/dev/<id>/cmd/display` | to device | no | Show a message |
| `showcase/dev/<id>/cmd/audio` | to device | no | Play a jingle or note sequence |
| `showcase/dev/<id>/cmd/led` | to device | no | Set the Grove pixel |
| `showcase/dev/<id>/cmd/config` | to device | yes | Team name, brightness, lock |
| `showcase/dev/<id>/state` | from device | yes | Online/offline (LWT), battery, voice, screen |
| `showcase/dev/<id>/event/button` | from device | no | Button A/B presses |
| `showcase/all/cmd/<kind>` | to all | no | Broadcast, organiser only |

Devices subscribe to `showcase/dev/<id>/cmd/#` and `showcase/all/cmd/#`. The
retained `config` message means a rebooted stick immediately knows its team
name and whether the room is locked.

## Command payloads

Display:

```json
{"text": "[red]Ship it[/] before 2pm", "from": "Team 7", "kind": "shout",
 "ttl_s": 15, "priority": 1}
```

- `kind` is one of `self`, `dm`, `shout`, `organiser`. It selects the header
  the stick draws above the text ("Team 7 shouts", "Team 3 to you", nothing for
  `self`, a distinct colour for `organiser`).
- `ttl_s` bounds how long the overlay stays before the countdown returns.
- `priority` lets an organiser message replace a team message but not the other
  way round.
- Existing title colour markup applies to `text`.

Audio, one of:

```json
{"jingle": "tada", "volume": 200}
{"notes": "C5:200 G5:200 R:100 C6:400", "volume": 200}
```

- Jingles are a small built-in set. Notes use the fanfare's note names plus `R`
  for rest, with a duration in ms. Not capped.

LED:

```json
{"color": "#FF8800", "mode": "blink", "period_ms": 400, "ttl_s": 10}
```

- `mode` is `solid`, `blink`, `breathe`, `off`, `snake`, `ping`, `rainbow`, or
  `rolling_rainbow`. After `ttl_s` the pixel returns to the schedule.
- `period_ms` sets the animation speed: the blink or breathe cycle, one snake
  lap, one ping round trip, or one rainbow rotation.
- `snake` chases a bright pixel with a tail that halves in brightness behind it,
  and `ping` bounces a single pixel end to end. Both use `color`.
- `rainbow` spreads red through violet across the strip and `rolling_rainbow`
  rotates that spread. Both ignore `color` and use the configured
  `LED_BRIGHTNESS`. On a single-pixel strip they show red only.

Config (retained):

```json
{"team": "Team 7", "brightness": 200, "locked": false}
```

State (retained, LWT sets `"online": false`):

```json
{"online": true, "team": "Team 7", "voice": "melody", "battery": 87,
 "ntp": true, "uptime_s": 1234, "fw": "0.3.0"}
```

## MCP tools

Team scope (any call carrying a valid claim code):

| Tool | Notes |
| --- | --- |
| `claim(code, team_name)` | Name the team behind `code`; pushes the name to the stick |
| `status()` | This team's device state |
| `show(text, seconds)` | Display on own device |
| `play(jingle \| notes)` | Audio on own device |
| `led(color, mode, seconds)` | Pixel on own device |
| `shout(text)` | Every device shows it with the team's name, plays the repeated `alarm` jingle at volume 200/255, and flashes enabled LEDs amber for 15 seconds. Cooldown applies |
| `message_team(team, text)` | DM another team's device and inbox |
| `list_teams()` | Names and online state, no tokens |
| `inbox()` | Messages received by this team |
| `wait_for_button(timeout_s)` | Blocks until A or B is pressed on own device |

Organiser scope (calls carrying the organiser secret) adds `broadcast`,
`broadcast_audio` (a jingle or tune on every stick), `lock`, `unlock`,
`mute(team)`, `unmute(team)`, `unbind`, and `rename`.

## Policy (all in the MCP server)

- Shout cooldown per team 120 s. Direct messages 5 s. Text at most 120
  characters. Display and LED TTLs at most 60 s.
- An accepted shout sends its 15-second display followed immediately by an
  `alarm` audio command at volume 200/255 and an amber (`#FFAA00`) LED blink
  with a 600 ms period and 15-second TTL. LEDs return to their schedule afterwards. Rejected shouts
  send no commands. If audio or LED publishing fails after earlier commands
  are sent, the tool reports that partial failure.
- Audio is uncapped: any length, any rate. The organiser's `mute` is the brake.
  Richer audio (samples, voice) is tracked in issue #2.
- **Room lock.** From 60 s before the event until the fanfare ends, every
  team command is refused and devices ignore non-organiser commands. Nothing
  steps on the fanfare.
- Organiser messages always win.
- No content filtering beyond length. The organiser has `mute`.

## Firmware implications

- WiFi stays on after NTP sync instead of powering off. Costs ~40-50 KB of RAM.
  The sprite is ~64 KB. Build currently uses ~49 KB of 320 KB, so it fits, but
  the order of allocation needs checking on hardware.
- Sticks must be on USB power at the event. The 120 mAh cell will not hold WiFi
  up for a day.
- MQTT via the ESP-IDF client in the Arduino core, JSON via ArduinoJson.
- Audio needs a non-blocking note scheduler. The celebration loop is blocking
  today and stays that way; team audio must not block the display or MQTT.
- Display gains an overlay layer with a TTL and priority above the countdown.

## ESP-NOW

A second path for the two messages that must land even if a stick's MQTT
session has dropped: `lock` / `unlock`, and `fire <epoch>`. A spare stick on the
router's USB port relays them from the server over serial. All sticks listen on
the AP's pinned 2.4 GHz channel.

`fire` is a last resort for a unit that never got a trusted clock. A unit only
acts on the frame when the epoch matches its built-in target **and it has no
verified time of its own**; a unit that has synced from NTP ignores it. ESP-NOW
is unauthenticated broadcast and the event epoch is public, so a synced unit
must never let a frame move its clock: otherwise anyone in radio range could
fire the room at any hour, and the fired state latches for the rest of the day.

## Build order

See [plan.md](plan.md) section 6.
