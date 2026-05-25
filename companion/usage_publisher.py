#!/usr/bin/env python3
"""Publish Claude Code usage (5h block + weekly) to MQTT for rpi-sentinel-display.

Runs on the Raspberry Pi where Claude Code is already logged in. It reads local
usage with `ccusage` (https://github.com/ryoppippi/ccusage) and publishes a small
JSON payload to `<prefix>/claude/usage`. No Claude credentials ever touch the
ESP32 display: the display only subscribes to MQTT.

Payload shape (percent / resets_in_seconds are optional):
    {
      "five_hour": {"used": 1234567, "limit": 0, "percent": 42, "resets_in_seconds": 7800},
      "weekly":    {"used": 9876543, "limit": 0}
    }

NOTE: the numbers are an ESTIMATE derived from this machine's local Claude Code
logs; they are not the account-wide subscription counters. A configured limit
(FIVE_HOUR_LIMIT / WEEKLY_LIMIT) turns the raw token count into a percentage.
"""
import json
import os
import ssl
import subprocess
import sys
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt


def env(name, default=None):
    v = os.environ.get(name)
    return v if v not in (None, "") else default


MQTT_HOST = env("MQTT_HOST")
MQTT_PORT = int(env("MQTT_PORT", "8883"))
MQTT_USER = env("MQTT_USER")
MQTT_PASS = env("MQTT_PASS")
MQTT_PREFIX = env("MQTT_PREFIX", "rpi")
MQTT_TLS = str(env("MQTT_TLS", "true")).lower() in ("1", "true", "yes")
POLL_SECONDS = int(env("POLL_SECONDS", "300"))
FIVE_HOUR_LIMIT = float(env("FIVE_HOUR_LIMIT", "0") or 0)
WEEKLY_LIMIT = float(env("WEEKLY_LIMIT", "0") or 0)
# Override if ccusage is installed differently, e.g. CCUSAGE_CMD="ccusage".
CCUSAGE_CMD = env("CCUSAGE_CMD", "npx -y ccusage@latest")

TOPIC = f"{MQTT_PREFIX}/claude/usage"


def run_ccusage(args):
    cmd = CCUSAGE_CMD.split() + args
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if out.returncode != 0:
        raise RuntimeError(f"ccusage failed ({' '.join(cmd)}): {out.stderr.strip()}")
    return json.loads(out.stdout)


def parse_iso(s):
    if not s:
        return None
    try:
        return datetime.fromisoformat(str(s).replace("Z", "+00:00"))
    except ValueError:
        return None


def entry_tokens(entry):
    """Total tokens for a ccusage block / daily entry, robust to schema changes."""
    if entry.get("totalTokens") is not None:
        return float(entry["totalTokens"])
    counts = entry.get("tokenCounts") or {}
    return float(sum(v for v in counts.values() if isinstance(v, (int, float))))


def five_hour_window():
    """(used_tokens, resets_in_seconds | None) for the active 5h block."""
    data = run_ccusage(["blocks", "--json"])
    blocks = data.get("blocks", data if isinstance(data, list) else [])
    active = next((b for b in blocks if b.get("isActive") and not b.get("isGap")), None)
    if not active:
        return 0.0, None

    used = entry_tokens(active)
    end = parse_iso(active.get("endTime"))
    if end is None:
        start = parse_iso(active.get("startTime"))
        if start is not None:
            end = datetime.fromtimestamp(start.timestamp() + 5 * 3600, tz=timezone.utc)
    resets_in = None
    if end is not None:
        resets_in = max(0, int((end - datetime.now(timezone.utc)).total_seconds()))
    return used, resets_in


def weekly_window():
    """(used_tokens, None) summed over the last 7 daily entries.

    The subscription weekly window is a rolling reset that ccusage does not
    expose directly, so we report consumption only and leave the countdown off.
    """
    data = run_ccusage(["daily", "--json"])
    days = data.get("daily", data if isinstance(data, list) else [])
    last7 = days[-7:]
    used = float(sum(entry_tokens(d) for d in last7))
    return used, None


def make_window(used, limit, resets_in):
    w = {"used": round(used), "limit": round(limit)}
    if limit and limit > 0:
        w["percent"] = max(0, min(100, round(100.0 * used / limit)))
    if resets_in is not None:
        w["resets_in_seconds"] = resets_in
    return w


def build_payload():
    fh_used, fh_reset = five_hour_window()
    wk_used, wk_reset = weekly_window()
    return {
        "five_hour": make_window(fh_used, FIVE_HOUR_LIMIT, fh_reset),
        "weekly": make_window(wk_used, WEEKLY_LIMIT, wk_reset),
    }


def make_client():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"claude-usage-{os.getpid()}")
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS or "")
    if MQTT_TLS:
        # Matches the display's WiFiClientSecure.setInsecure(); pin the CA later.
        client.tls_set(cert_reqs=ssl.CERT_NONE)
        client.tls_insecure_set(True)
    return client


def main():
    if not MQTT_HOST:
        print("MQTT_HOST is required", file=sys.stderr)
        sys.exit(1)

    client = make_client()
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_start()
    print(f"connected to {MQTT_HOST}:{MQTT_PORT}, publishing to {TOPIC} every {POLL_SECONDS}s",
          flush=True)
    try:
        while True:
            try:
                payload = build_payload()
                client.publish(TOPIC, json.dumps(payload), qos=1, retain=True)
                print(f"published {TOPIC}: {payload}", flush=True)
            except Exception as exc:  # keep the loop alive on transient ccusage/MQTT errors
                print(f"error: {exc}", file=sys.stderr, flush=True)
            time.sleep(POLL_SECONDS)
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
