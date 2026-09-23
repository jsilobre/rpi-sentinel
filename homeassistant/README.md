# Home Assistant (Docker) next to rpi-sentinel

Runs [Home Assistant](https://www.home-assistant.io/) and a local Mosquitto
broker as Docker containers on the same Raspberry Pi as the daemon. The
daemon announces its sensors through **MQTT discovery**, so they show up in
Home Assistant (HA) on their own as one **RPi Sentinel** device.

```
 rpi-sentinel ──TLS──► HiveMQ Cloud ◄── dashboard (GitHub Pages), M5 display
 (daemon)                  ▲ │
                           │ │ bridge (MQTT v5, TLS)
                           │ ▼
                      Mosquitto (local, :1883) ◄──► Home Assistant (:8123)
                                               ◄──► future devices (Zigbee2MQTT…)
```

The daemon keeps publishing to HiveMQ exactly as before: the dashboard and the
M5 display are unaffected. The local broker pulls the daemon's topics in over
a bridge, and becomes the broker for everything else you add to HA later.

## What you get in Home Assistant

For each sensor in `config.json`:

| Entity | Type | Source |
|---|---|---|
| `<id>` | `sensor` with device class and unit (°C, %, hPa, ppm, ppb) and long-term statistics | `value` of `rpi/<id>/reading` |
| `<id>` (metric `motion`) | `binary_sensor`, motion class (on when value ≥ 0.5) | same |
| `<id> level` | `sensor` enum: `ok` / `warn` / `crit` (the daemon's thresholds, hysteresis included) | `level` of `rpi/<id>/reading` |

Plus a **Refresh readings** button (`rpi/cmd/refresh`). `rpi/cmd/clear` is
deliberately not exposed: it wipes the history database.

Every entity is marked unavailable when the daemon goes offline (its
`rpi/status` last will).

## Prerequisites

- A **64-bit** Raspberry Pi OS (HA no longer ships 32-bit images), Pi 4 or 5
  with 2 GB+ of RAM. An SSD is strongly advised over an SD card.
- The daemon already publishing to HiveMQ Cloud (`mqtt.enabled: true`).

## Setup

### 1. Docker

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER   # then log out and back in
```

### 2. Local broker user

From this directory (`homeassistant/`):

```bash
docker compose run --rm mosquitto sh -c \
  'mosquitto_passwd -c /mosquitto/config/passwd homeassistant &&
   chown mosquitto:mosquitto /mosquitto/config/passwd'
```

The `chown` matters: the broker drops to the `mosquitto` user before reading
the file, and the image only fixes ownership of `data/` by itself.

### 3. Bridge to HiveMQ Cloud

1. In the HiveMQ Cloud console (**Access Management**), create a credential
   for the bridge with *Publish and Subscribe* permission.
2. Copy and fill in the template (the copy is git-ignored):

   ```bash
   cp mosquitto/config/conf.d/hivemq-bridge.conf.example \
      mosquitto/config/conf.d/hivemq-bridge.conf
   nano mosquitto/config/conf.d/hivemq-bridge.conf
   ```

If you changed `mqtt.topic_prefix` in the daemon, replace `rpi` in the topic
lines accordingly (and `rpi-sentinel` with `<prefix>-sentinel`).

### 4. Turn on discovery in the daemon

In the daemon's `config.json` (`/etc/rpi-sentinel/config.json` with the
systemd package), add the `homeassistant` block inside `mqtt`:

```json
"mqtt": {
    "enabled": true,
    "broker_url": "ssl://YOUR_CLUSTER.hivemq.cloud:8883",
    "...": "...",
    "homeassistant": { "enabled": true }
}
```

Then restart it (`sudo systemctl restart rpi-sentinel`). The log shows
`Home Assistant discovery enabled`.

### 5. Start the containers

```bash
docker compose up -d
docker compose logs -f mosquitto   # expect "Connecting bridge ... hivemq"
```

### 6. First run of Home Assistant

1. Open `http://<pi-address>:8123` and create your account.
2. **Settings → Devices & services → Add integration → MQTT**:
   broker `127.0.0.1`, port `1883`, user `homeassistant` and the password from
   step 2.
3. **Settings → Devices & services → MQTT → Devices → RPi Sentinel**: your sensors.

## Everyday use

**Update**: `docker compose pull && docker compose up -d`.

**Backup**: everything HA knows lives in `ha-config/`. It can also be
restored into Home Assistant OS if you later move to a dedicated machine.

**Watch the traffic** on the local broker:

```bash
docker exec -it mosquitto mosquitto_sub -u homeassistant -P '<password>' -t 'rpi/#' -v
```

**Example automation**: phone notification (HA companion app) when a sensor
reaches its critical threshold. In **Settings → Automations → Create → Edit
in YAML**:

```yaml
alias: RPi Sentinel critical
triggers:
  - trigger: state
    entity_id: sensor.rpi_sentinel_sgp30_tvoc_level
    to: crit
actions:
  - action: notify.notify
    data:
      message: "TVOC critical: {{ states('sensor.rpi_sentinel_sgp30_tvoc') }} ppb"
```

(Check the actual entity ids under the device page; they derive from the
sensor ids.)

**Claude usage** from the companion is bridged too, but it is not announced
by discovery. To show it, add to `ha-config/configuration.yaml` and restart HA:

```yaml
mqtt:
  sensor:
    - name: "Claude usage 5h"
      state_topic: "rpi/claude/usage"
      value_template: "{{ value_json.five_hour.percent }}"
      unit_of_measurement: "%"
    - name: "Claude usage weekly"
      state_topic: "rpi/claude/usage"
      value_template: "{{ value_json.weekly.percent }}"
      unit_of_measurement: "%"
```

## Good to know

- **Database size.** Readings arrive every `poll_interval_ms` (2 s in the
  example config), and HA's recorder stores every state change. With about ten
  sensors that adds up. If `ha-config/home-assistant_v2.db` grows too fast,
  raise the poll interval or lower `recorder: purge_keep_days` (default 10).
- **Removing a sensor** from `config.json` leaves its entities in HA, because
  their discovery config is retained on HiveMQ. Clear it there, not on the
  local broker, or the bridge brings it back:

  ```bash
  for t in sensor/rpi-sentinel/<id> sensor/rpi-sentinel/<id>_level; do
    mosquitto_pub -h YOUR_CLUSTER.hivemq.cloud -p 8883 --capath /etc/ssl/certs \
      -u <user> -P '<password>' -r -n -t "homeassistant/$t/config"
  done
  ```

  (Use `binary_sensor/…` instead of `sensor/…` for a motion sensor.)
- **Internet dependency.** Sensor data reaches HA through HiveMQ, so it pauses
  when the Pi is offline. Devices added directly to the local broker are not
  affected. Pointing the daemon at the local broker and reversing the bridge
  would remove that dependency; it is a possible next step.
