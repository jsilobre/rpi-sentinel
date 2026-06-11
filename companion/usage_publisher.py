#!/usr/bin/env python3
"""Publish account-wide Claude usage to MQTT for the rpi-sentinel display.

Polls Anthropic's OAuth usage endpoint -- the same one Claude Code's /usage
screen reads -- so the numbers reflect the whole account (every device),
not just sessions run on this machine. This replaces the ccusage-based
publisher, which only saw local Claude Code logs.

Requires a logged-in Claude Code install (token read from
~/.claude/.credentials.json) or a long-lived token in CLAUDE_OAUTH_TOKEN.

Payload published to <MQTT_PREFIX>/claude/usage (QoS 1, retained), matching
the rpi-sentinel-display firmware contract:

    {"five_hour": {"percent": 7, "resets_in_seconds": 7800},
     "weekly":    {"percent": 9, "resets_in_seconds": 280000}}

Environment:
    MQTT_HOST            broker hostname (required)
    MQTT_PORT            default 8883
    MQTT_USER, MQTT_PASS broker credentials
    MQTT_PREFIX          topic prefix, default "rpi" (must match the display)
    MQTT_TLS             default "true" (insecure verify, like the display)
    POLL_SECONDS         default 600; the endpoint rate-limits aggressive
                         polling (HTTP 429), so stay at 10 min or above
    CLAUDE_OAUTH_TOKEN   optional token override (e.g. from `claude setup-token`)
    CLAUDE_CREDENTIALS   default ~/.claude/.credentials.json

Dependency: paho-mqtt>=2.0
"""

import json
import logging
import os
import ssl
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

from paho.mqtt import publish

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"

MQTT_HOST = os.environ.get("MQTT_HOST", "")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "8883"))
MQTT_USER = os.environ.get("MQTT_USER")
MQTT_PASS = os.environ.get("MQTT_PASS")
MQTT_PREFIX = os.environ.get("MQTT_PREFIX", "rpi")
MQTT_TLS = os.environ.get("MQTT_TLS", "true").lower() != "false"
POLL_SECONDS = int(os.environ.get("POLL_SECONDS", "600"))
CREDENTIALS = Path(os.environ.get(
    "CLAUDE_CREDENTIALS", Path.home() / ".claude" / ".credentials.json"))

log = logging.getLogger("usage-publisher")


def oauth_token() -> str:
    token = os.environ.get("CLAUDE_OAUTH_TOKEN")
    if token:
        return token
    # Re-read on every poll: Claude Code rotates the access token whenever
    # it runs on this machine.
    data = json.loads(CREDENTIALS.read_text())
    return data["claudeAiOauth"]["accessToken"]


def fetch_usage(token: str) -> dict:
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
        "User-Agent": "rpi-sentinel-usage-publisher",
    })
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp)


def window(src: dict) -> dict:
    """Map one endpoint window ({utilization, resets_at}) to the display
    contract ({percent, resets_in_seconds})."""
    out = {}
    if not src:
        return out
    if src.get("utilization") is not None:
        out["percent"] = round(src["utilization"])
    resets_at = src.get("resets_at")
    if resets_at:
        dt = datetime.fromisoformat(resets_at)
        secs = int((dt - datetime.now(timezone.utc)).total_seconds())
        if secs > 0:
            out["resets_in_seconds"] = secs
    return out


def build_payload(usage: dict) -> dict:
    payload = {}
    for src_key, dst_key in (("five_hour", "five_hour"), ("seven_day", "weekly")):
        w = window(usage.get(src_key))
        if w:
            payload[dst_key] = w
    return payload


def publish_payload(payload: dict) -> None:
    auth = {"username": MQTT_USER, "password": MQTT_PASS} if MQTT_USER else None
    tls = {"cert_reqs": ssl.CERT_NONE} if MQTT_TLS else None
    publish.single(
        f"{MQTT_PREFIX}/claude/usage",
        json.dumps(payload),
        qos=1,
        retain=True,
        hostname=MQTT_HOST,
        port=MQTT_PORT,
        auth=auth,
        tls=tls,
    )


def main() -> int:
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    if not MQTT_HOST:
        log.error("MQTT_HOST is required")
        return 1

    interval = POLL_SECONDS
    while True:
        try:
            usage = fetch_usage(oauth_token())
            payload = build_payload(usage)
            if payload:
                publish_payload(payload)
                log.info("published %s", json.dumps(payload))
            else:
                log.warning("usage response had no five_hour/seven_day windows")
            interval = POLL_SECONDS
        except urllib.error.HTTPError as e:
            if e.code == 429:
                interval = min(interval * 2, 3600)
                log.warning("rate-limited (429); next poll in %ds", interval)
            elif e.code == 401:
                log.warning("token rejected (401) -- expired? Run claude once on "
                            "this machine to refresh it, or set CLAUDE_OAUTH_TOKEN "
                            "from `claude setup-token`")
            else:
                log.warning("usage endpoint HTTP %d: %s", e.code, e.reason)
        except Exception as e:
            log.warning("poll failed: %s", e)
        time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main())
