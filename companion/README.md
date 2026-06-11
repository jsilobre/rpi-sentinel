# Claude usage publisher (companion)

Publishes your **account-wide** Claude usage (5-hour block + weekly) to MQTT so
the `rpi-sentinel-display` ESP32 can show it on its **Claude usage** page.

This runs on the **Raspberry Pi where Claude Code is already logged in** — the
ESP32 display never receives any Claude credential, it only subscribes to MQTT.

## How it works

1. Every `POLL_SECONDS`, the script calls Anthropic's OAuth usage endpoint
   (`https://api.anthropic.com/api/oauth/usage`) — the same one Claude Code's
   `/usage` screen reads — authenticating with the OAuth token from
   `~/.claude/.credentials.json` (or `CLAUDE_OAUTH_TOKEN`).
2. It maps the response's `five_hour` / `seven_day` windows to the display
   contract.
3. It publishes (retained, QoS 1) to `"<MQTT_PREFIX>/claude/usage"`, on the same
   broker the display already uses.

Payload:

```json
{
  "five_hour": { "percent": 7, "resets_in_seconds": 7800 },
  "weekly":    { "percent": 9, "resets_in_seconds": 280000 }
}
```

Because the numbers come from the account-wide endpoint, they reflect usage from
**every device on the account**, not just this machine.

## Prerequisites

- A logged-in Claude Code on this machine (for `~/.claude/.credentials.json`),
  **or** a long-lived token in `CLAUDE_OAUTH_TOKEN` (from `claude setup-token`,
  valid 1 year).
- Python 3.9+.

## Install & run

```bash
cd companion
pip install -r requirements.txt

export MQTT_HOST=your-broker.example.com
export MQTT_PORT=8883
export MQTT_USER=youruser
export MQTT_PASS=yourpass
export MQTT_PREFIX=rpi        # must match the display's topic prefix
export MQTT_TLS=true
export POLL_SECONDS=600
# Optional: if this Pi never runs `claude`, use a long-lived token instead:
# export CLAUDE_OAUTH_TOKEN=$(claude setup-token)

python3 usage_publisher.py
```

Verify it arrives:

```bash
mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" -u "$MQTT_USER" -P "$MQTT_PASS" \
  --capath /etc/ssl/certs -t "$MQTT_PREFIX/claude/usage"
```

## Run as a service

Edit `usage-publisher.service` (user, paths, env vars), then:

```bash
sudo cp usage-publisher.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now usage-publisher
journalctl -u usage-publisher -f
```

## Configuration

| Env var              | Default                          | Description                                       |
|----------------------|----------------------------------|---------------------------------------------------|
| `MQTT_HOST`          | (required)                       | Broker hostname                                   |
| `MQTT_PORT`          | `8883`                           | Broker port                                       |
| `MQTT_USER`/`PASS`   | —                                | Broker credentials                                |
| `MQTT_PREFIX`        | `rpi`                            | Topic prefix; must match the display config       |
| `MQTT_TLS`           | `true`                           | Use TLS (insecure verify, like the display)       |
| `POLL_SECONDS`       | `600`                            | Poll/publish interval; keep at 10 min or above — the endpoint rate-limits (HTTP 429) |
| `CLAUDE_OAUTH_TOKEN` | — (off)                          | Token override, e.g. from `claude setup-token`    |
| `CLAUDE_CREDENTIALS` | `~/.claude/.credentials.json`    | Path to Claude Code's credentials file            |

## Caveats / honesty

- The OAuth usage endpoint is the one Claude Code's `/usage` screen uses; it is
  not a formally documented public API, so its shape (`five_hour` / `seven_day`,
  `utilization`, `resets_at`) may change. Parsing degrades gracefully (a missing
  window is simply omitted from the payload).
- The access token in `~/.claude/.credentials.json` expires and is rotated by
  Claude Code; the script re-reads the file on every poll to pick up rotations.
  If this Pi never runs `claude`, set `CLAUDE_OAUTH_TOKEN` from
  `claude setup-token` instead.
- On HTTP 429 the script backs off exponentially (up to 1 h) before retrying.
- TLS verification is disabled to mirror the display's `setInsecure()`. Pin the
  broker CA on both sides for production.
