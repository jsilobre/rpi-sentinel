#!/usr/bin/env python3
"""Publish account-wide Claude usage to MQTT for the rpi-sentinel display.

Polls Anthropic's OAuth usage endpoint -- the same one Claude Code's /usage
screen reads -- so the numbers reflect the whole account (every device),
not just sessions run on this machine. This replaces the ccusage-based
publisher, which only saw local Claude Code logs.

Requires a logged-in Claude Code install: the token is read from
~/.claude/.credentials.json and, once expired, refreshed by invoking the
`claude` CLI so Claude Code rotates it itself. Tokens from
`claude setup-token` are not enough: they lack the user:profile scope the
usage endpoint requires (HTTP 403).

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
    CLAUDE_OAUTH_TOKEN   optional token override with the user:profile scope,
                         tried first; if the endpoint rejects it (401/403) the
                         credentials file is used as a fallback
    CLAUDE_CREDENTIALS   default ~/.claude/.credentials.json
    CLAUDE_BIN           path to the claude CLI used to refresh the token,
                         default: found on PATH, else ~/.local/bin/claude

Dependency: paho-mqtt>=2.0
"""

import json
import logging
import os
import shutil
import ssl
import subprocess
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
# The service's PATH usually lacks ~/.local/bin, where Claude Code installs.
CLAUDE_BIN = os.environ.get("CLAUDE_BIN") or shutil.which("claude") or str(
    Path.home() / ".local" / "bin" / "claude")

log = logging.getLogger("usage-publisher")


class TokenRejected(Exception):
    pass


def read_credentials() -> tuple[str, float | None]:
    """Return (access token, expiry as epoch seconds or None)."""
    oauth = json.loads(CREDENTIALS.read_text())["claudeAiOauth"]
    expires_ms = oauth.get("expiresAt")
    return oauth["accessToken"], expires_ms / 1000 if expires_ms else None


def run_claude(args: list[str], stdin: str | None = None) -> None:
    try:
        subprocess.run([CLAUDE_BIN, *args], input=stdin, capture_output=True,
                       text=True, timeout=120, cwd=Path.home(), check=True)
    except (OSError, subprocess.SubprocessError) as e:
        log.warning("`claude %s` failed: %s", args[0], e)


def refresh_credentials(old_token: str) -> str:
    """Have Claude Code refresh its own OAuth token, then re-read the file.

    Claude Code refreshes the access token when it needs it and writes the
    rotated pair back to the credentials file itself, so this script never
    touches the refresh token (no race with an interactive `claude`).
    `claude auth status` is tried first; if that does not rotate the token,
    a minimal Haiku request in print mode does.
    """
    attempts = (
        ("auth status", ["auth", "status"], None),
        ("print-mode request", ["-p", "--model", "haiku",
                                "--no-session-persistence",
                                "--strict-mcp-config",
                                "--setting-sources", "", "--tools", ""],
         "Reply with just: ok"),
    )
    for label, args, stdin in attempts:
        run_claude(args, stdin)
        token, expires_at = read_credentials()
        if token != old_token:
            log.info("token refreshed via `claude %s`%s", label,
                     f" (expires {datetime.fromtimestamp(expires_at):%Y-%m-%d %H:%M})"
                     if expires_at else "")
            return token
    log.warning("token not refreshed by `claude auth status` nor a print-mode "
                "request; run `claude` once on this machine")
    return old_token


def fetch_usage_any() -> dict:
    """Fetch usage with CLAUDE_OAUTH_TOKEN if set, else (or if it is
    rejected) with Claude Code's credentials, refreshing them when expired."""
    rejected = []
    env_token = os.environ.get("CLAUDE_OAUTH_TOKEN")
    if env_token:
        try:
            return fetch_usage(env_token)
        except urllib.error.HTTPError as e:
            if e.code not in (401, 403):
                raise
            rejected.append(f"CLAUDE_OAUTH_TOKEN ({e.code})")

    # Re-read on every poll: Claude Code rotates the token whenever it runs.
    try:
        token, expires_at = read_credentials()
    except (OSError, ValueError, KeyError) as e:
        raise TokenRejected(", ".join(
            rejected + [f"{CREDENTIALS} (unreadable: {e})"])) from e
    refreshed = False
    if expires_at is not None and expires_at < time.time() + 60:
        token, refreshed = refresh_credentials(token), True
    while True:
        try:
            return fetch_usage(token)
        except urllib.error.HTTPError as e:
            if e.code != 401 or refreshed:
                if e.code in (401, 403):
                    rejected.append(f"{CREDENTIALS} ({e.code})")
                    raise TokenRejected(", ".join(rejected)) from e
                raise
            token, refreshed = refresh_credentials(token), True


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

    backoff = POLL_SECONDS
    while True:
        # Only a 429 backs off; any other outcome returns to the normal
        # cadence so a past rate-limit does not slow polling forever.
        interval = POLL_SECONDS
        try:
            usage = fetch_usage_any()
            payload = build_payload(usage)
            if payload:
                publish_payload(payload)
                log.info("published %s", json.dumps(payload))
            else:
                log.warning("usage response had no five_hour/seven_day windows")
        except TokenRejected as e:
            log.warning("token rejected by %s -- run `claude` once on this "
                        "machine to log in again", e)
        except urllib.error.HTTPError as e:
            if e.code == 429:
                backoff = interval = min(backoff * 2, 3600)
                log.warning("rate-limited (429); next poll in %ds", interval)
            else:
                log.warning("usage endpoint HTTP %d: %s", e.code, e.reason)
        except Exception as e:
            log.warning("poll failed: %s", e)
        if interval == POLL_SECONDS:
            backoff = POLL_SECONDS
        time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main())
