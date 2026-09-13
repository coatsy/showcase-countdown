---
title: Connect your coding agent to a Showcase Stick
description: MCP connection details, client setup, first commands, and troubleshooting for controlling your team's device.
---

The little screen on your table counts down to the showcase. It is also yours
to control from your coding agent through the Model Context Protocol (MCP).
Your agent can show messages, play tunes, control the light, and message other
tables by calling the server's tools.

## Get your connection details

1. Connect your laptop and stick to the event network. The stick needs the event
   firmware with messaging configured, not just a team name set on the
   configuration page.
2. Ask the organiser for the MCP server's address and port. Use the event
   server or router address, not the stick's IP address or the installer website.
3. Read the four-digit **claim code** at the bottom-left of your stick's screen.
   Everyone at your table can use the same code.

Build your connection URL by adding `/mcp/` and your code to the server address:

```text
http://ROUTER:8090/mcp/1234
```

All examples below use `ROUTER`, port `8090`, and code `1234` as placeholders.
Replace them with the values for your event and device. If the organiser gives
you a complete URL, use it as supplied, including its scheme and port.

The server uses `Streamable HTTP`. This is a network connection: USB is only
needed for installation and device configuration, not for subsequent MCP calls.
The dashboard is at the same server address without `/mcp/1234`, for example
`http://ROUTER:8090/`.

> [!IMPORTANT]
> The claim code identifies the stick and lets its holder act as that team. It
> is not strong authentication. Use the HTTP examples only on the trusted event
> network, keep real code-bearing URLs out of public repositories, and do not
> expose this service to the Internet. A cloud-hosted MCP client cannot reach a
> private event address without an organiser-approved network setup.

## Configure your MCP client

### GitHub Copilot in VS Code

Add this server to `.vscode/mcp.json` in your project. If the file already has
servers, merge the `stick` entry into its existing `servers` object.

```json
{
  "servers": {
    "stick": {
      "type": "http",
      "url": "http://ROUTER:8090/mcp/1234"
    }
  }
}
```

Start `stick` using the MCP controls shown in the configuration editor, and
review the server trust prompt. In Copilot Chat, choose Agent mode and enable
the `stick` tools in the tool picker. Approve tool calls when prompted.

### Claude Code

Run this from your project directory:

```bash
claude mcp add --transport http stick http://ROUTER:8090/mcp/1234
```

Use the `/mcp` menu inside Claude Code to check that `stick` is connected and
its tools are available.

### Cursor

Add this entry to `.cursor/mcp.json`, merging it with any existing servers:

```json
{
  "mcpServers": {
    "stick": {
      "url": "http://ROUTER:8090/mcp/1234"
    }
  }
}
```

Enable `stick` in Cursor's MCP settings, then use its agent chat with the
server's tools enabled.

### Other clients and headers

Choose a remote MCP server with `Streamable HTTP` transport and use the same
code-bearing URL. Do not configure it as a local `stdio` command or a legacy
`/sse` endpoint.

Clients that support custom request headers can alternatively connect to
`http://ROUTER:8090/mcp` and send `X-Claim-Code: 1234` on their requests. The
URL form is usually simpler. Team access does not require an organiser secret.

## Make your first call

Start with a read-only check. Ask your agent:

```text
Use the stick MCP server's status tool. Confirm the device is online and tell
me its team name, battery level, and whether the room is locked.
```

The code in your connection already selects the device. You do not have to
call `claim` before `status` or the other team tools.

If the device is unnamed, ask your agent to call `claim` with your team name,
for example with these arguments:

```json
{
  "team_name": "Atlas"
}
```

Use the same predefined team name as on the configuration page so its team
colour is recognised. If the name is already correct, leave it alone. `claim`
names or renames the device selected by your connection; it does not allocate
a new code, and there is no `code` argument to this tool.

Then try a short message:

```text
Use the stick MCP server's show tool to display "Atlas ready" for 10 seconds.
```

The corresponding `show` arguments are:

```json
{
  "text": "Atlas ready",
  "seconds": 10
}
```

Replace Atlas with your own team name. Call `jingles` before asking the agent
to play a built-in tune; it returns the supported names.

## What it can do

| Tool              | What happens                                           |
| ----------------- | ------------------------------------------------------ |
| `claim`           | Name or rename the team behind your connection's code  |
| `show`            | A temporary screen message; colour markup is supported |
| `play`            | A jingle or a tune composed as `NOTE:MS` pairs         |
| `jingles`         | List the built-in jingles                              |
| `led`             | LED colour and pattern, such as solid or breathe       |
| `shout`           | Room-wide message, sound and lights; cooldowns apply   |
| `message_team`    | Message another table's screen and inbox               |
| `list_teams`      | List teams, online state and fanfare voices            |
| `inbox`           | Read messages and shouts sent to your team             |
| `wait_for_button` | Wait for a button press on your stick                  |
| `status`          | Device state, team name, room lock and mute status     |

Organiser-only tools may also appear in the client. They require separate
organiser access and are not needed to control your own stick.

## Ideas

- Ask your agent to play `merge` after a successful PR merge.
- Show the name of the branch you just pushed.
- Breathe green while tests pass, blink red when they fail.
- Have your agent wait for a button press and then read out your inbox.
- Compose a team anthem in notes. Sharps and flats are fine: `F#4:150 Bb4:150`.

## Troubleshooting

- If the server cannot be reached, open its dashboard address in a browser.
  Check the event network, host and port with the organiser. A VPN or a
  different Wi-Fi network can prevent access to the event LAN.
- If no claim code is visible on the stick, ask the organiser to check its
  messaging configuration. Setting a team name alone does not connect it to
  the event broker.
- If a tool reports "no claim code", check that your URL ends in `/mcp/1234`
  with your actual code, or that your client sends the `X-Claim-Code` header.
- If a tool reports "no stick is showing code", check the screen again and
  wait for the device to appear online in the dashboard.
- If the agent cannot see the tools, check that the server is started, that
  you reviewed its trust prompt, and that the tools are enabled in agent chat.
- If `status` works but a control tool is refused, check its `room_locked`
  and `muted` fields. Wait for the finale to finish or ask the organiser.
- Opening `/mcp/1234` directly in a browser is not a protocol test. It is an
  MCP endpoint, not a web page; use an MCP client to call it.

## House rules

- The organiser can lock the room, and the finale has an automatic lock.
  Device-control tools pause while locked; `status` remains available.
- Room-wide shouts have a cooldown that the organiser can adjust.
- Loud or looping audio gets a table muted. Be a good neighbour.
- Anyone who knows a code can act as that table. Use your own stick and
  respect the other teams.
