# Claude usage publisher (companion)

Publishes your Claude Code usage (5-hour block + weekly) to MQTT so the
`rpi-sentinel-display` ESP32 can show it on its **Claude usage** page.

This runs on the **Raspberry Pi where Claude Code is already logged in** — the
ESP32 display never receives any Claude credential, it only subscribes to MQTT.

## How it works

1. Every `POLL_SECONDS`, the script runs [`ccusage`](https://github.com/ryoppippi/ccusage)
   to read this machine's local Claude Code logs.
2. It builds a small JSON payload (5h block + last-7-days totals).
3. It publishes (retained) to `"<MQTT_PREFIX>/claude/usage"`, on the same broker
   the display already uses.

Payload:

```json
{
  "five_hour": { "used": 1234567, "limit": 0, "percent": 42, "resets_in_seconds": 7800 },
  "weekly":    { "used": 9876543, "limit": 0 }
}
```

`percent` is included only when a limit is configured; `resets_in_seconds` only
when ccusage exposes the block end time.

## Prerequisites

- Node.js (for `npx ccusage`) and a logged-in Claude Code on this machine.
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
export POLL_SECONDS=300
# Optional: set caps to display a percentage instead of raw tokens
# export FIVE_HOUR_LIMIT=...
# export WEEKLY_LIMIT=...

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

| Env var           | Default               | Description                                        |
|-------------------|-----------------------|----------------------------------------------------|
| `MQTT_HOST`       | (required)            | Broker hostname                                    |
| `MQTT_PORT`       | `8883`                | Broker port                                        |
| `MQTT_USER`/`PASS`| —                     | Broker credentials                                 |
| `MQTT_PREFIX`     | `rpi`                 | Topic prefix; must match the display config        |
| `MQTT_TLS`        | `true`                | Use TLS (insecure verify, like the display)        |
| `POLL_SECONDS`    | `300`                 | Publish interval                                    |
| `FIVE_HOUR_LIMIT` | `0` (off)             | Token cap for the 5h window -> enables `percent`   |
| `WEEKLY_LIMIT`    | `0` (off)             | Token cap for the weekly window -> enables `percent`|
| `CCUSAGE_CMD`     | `npx -y ccusage@latest` | How to invoke ccusage                            |

## Caveats / honesty

- Numbers are an **estimate** from this machine's local Claude Code logs, not the
  account-wide subscription counters. There is no stable public API for the
  Pro/Max 5h/weekly windows.
- They reflect usage **on this machine only** (where the companion runs).
- The weekly value is the sum of the last 7 daily entries; the rolling reset is
  not exposed by ccusage, so no weekly countdown is sent.
- ccusage's JSON keys (`blocks`, `isActive`, `totalTokens`, `endTime`, `daily`)
  may change between versions; parsing degrades gracefully (falls back to 0).
- TLS verification is disabled to mirror the display's `setInsecure()`. Pin the
  broker CA on both sides for production.
