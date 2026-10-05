# Dashboard

The dashboard (`dashboard/`) is a static single-page app served by GitHub Pages.
It has no build step and no server of its own: the browser talks MQTT over
WebSocket to the broker the daemon publishes to (HiveMQ Cloud), and, when
configured, HTTPS to the Cloudflare Worker for long-term history and CSV export.

```
                     MQTT over WSS                     MQTT
 Browser (Pages) ◄──────────────────► HiveMQ Cloud ◄──────────► rpi-sentinel daemon
       │                                                          (MqttPublisher)
       │  HTTPS  GET /history, GET /export   (optional)
       └────────────────────────────────► Cloudflare Worker + D1
```

The history protocols are specified in [persistence.md](persistence.md) (MQTT
history-on-demand, alert snapshot) and [cloudflare-setup.md](cloudflare-setup.md)
(Worker endpoints, deployment). This page covers the front-end itself.

---

## 1. Files and script loading

| File | Role |
|---|---|
| `index.html` | Markup, CDN libraries, the inline deploy-time config block, script tags |
| `styles.css` | All styles: light/dark theme tokens, layout, breakpoints, touch rules |
| `js/state.js` | Shared state and constants (`WINDOWS`, `MAX_HISTORY`, storage keys, layout constants) |
| `js/utils.js` | Pure helpers (`fmt`, `escapeHtml`, `statusBadge`, `allowPanStart`, …) |
| `js/layout.js` | Free-form card layout: placement, drag, save/restore, Organize, stacked-mode checks |
| `js/charts.js` | Per-sensor cards and their Chart.js charts, stats, threshold annotations |
| `js/history.js` | Initial hydration, time-window switching, Cloudflare `/history` fetches |
| `js/combined.js` | Combined (all-sensors) chart and the view-mode switch |
| `js/config-panel.js` | ⚙ Config modal: poll interval, per-sensor on/off, thresholds, ack handling |
| `js/handlers.js` | Reading and alert handlers, alert timeline rendering |
| `js/main.js` | Wiring: buttons, header menu, theme, modal, MQTT client and message routing |
| `tests/dashboard.test.mjs` | Node tests (see [§8](#8-tests)) |

The scripts are **classic scripts sharing one global scope**, loaded at the end
of `<body>` in the order above; `main.js` comes last because it creates the MQTT
client and wires every handler. Every top-level `const`, `let` and `function` is
therefore global: two files declaring the same name is a `SyntaxError` for the
whole page, which the tests catch.

Third-party libraries come from jsDelivr with pinned versions: Chart.js 4.4.0,
chartjs-plugin-zoom 2.0.1 (with Hammer.js 2.0.8 for touch gestures),
chartjs-plugin-annotation 3.0.1 and MQTT.js 5.10.1.

---

## 2. Configuration and deployment

The inline `<script>` block in `index.html` holds the deploy-time settings:

| Constant | Source | Meaning |
|---|---|---|
| `BROKER_WSS` | `__MQTT_BROKER_WSS__` → secret `MQTT_BROKER_WSS` | Broker WebSocket URL, e.g. `wss://<cluster>.hivemq.cloud:8884/mqtt` |
| `MQTT_USER` / `MQTT_PASS` | `__MQTT_USER__` / `__MQTT_PASS__` → secrets | Dashboard broker credentials (see ACL below) |
| `CLOUD_WORKER_URL` | `__CLOUD_WORKER_URL__` → secret `CLOUD_WORKER_URL` | Worker base URL; empty or left as placeholder disables cloud features |
| `TOPIC_PREFIX` | hard-coded `'rpi'` | Must match `mqtt.topic_prefix` in the daemon's `config.json` |
| `CLOUD_ENABLED` | derived | `true` once a real Worker URL has been injected |

`.github/workflows/deploy-dashboard.yml` runs on pushes to `main` that touch
`dashboard/**` (or manually): it replaces the four placeholders with `sed` and
publishes `dashboard/` to GitHub Pages. The placeholders must stay inline in
`index.html` for that substitution to work — a test fails if one disappears.
Never commit real credentials in their place.

When `CLOUD_ENABLED` is false, the 1mo/6mo/1y windows, the custom date range
and ⬇ Export CSV stay hidden or disabled, and every other window falls back to
MQTT history-on-demand.

### Broker ACL for the dashboard user

| Pattern | Permission | Used for |
|---|---|---|
| `rpi/+/reading`, `rpi/+/alert` | subscribe | Live readings and alerts |
| `rpi/status`, `rpi/config/current`, `rpi/alerts/recent` | subscribe | Pi status, config, alert timeline |
| `rpi/history/resp/+` | subscribe | History responses |
| `rpi/history/req` | publish | History requests |
| `rpi/config/set` | publish | ⚙ Config changes |
| `rpi/cmd/refresh`, `rpi/cmd/clear`, `rpi/cmd/clear_alerts` | publish | ↻ Refresh, 🗑 Clear Data, ✕ Clear alerts |

A missing permission fails silently: the broker drops the message and the
dashboard shows no error.

---

## 3. What the dashboard shows

**Header.** Connection status (*Connecting…*, *RPi online*, *RPi offline*,
*Disconnected*, *Connection error*), the time of the last reading, then the
actions: ↻ Refresh (forces an immediate poll of every sensor), ▦ Combined /
⊞ Per-sensor, ⊞ Organize, ⚙ Config, ⬇ Export CSV, 🗑 Clear Data and the theme
toggle. *RPi online/offline* comes from the retained `rpi/status` message (the
daemon's MQTT last will); if none arrives within 5 s of the first connection
the dashboard assumes the Pi is offline.

**Sensor cards** (per-sensor view). One card per enabled sensor, created on its
first reading:

- current value and trend (last point vs. the point four readings earlier;
  within ±0.5 % it shows *stable*);
- min / avg / max over the points currently on the chart;
- status badge **OK / Warn / Crit** from the `level` carried by each reading;
- the chart, with dashed *Warn* and *Crit* lines from the configured
  thresholds. On long cloud windows it shows the bucket average with a
  min/max band.

**Combined view.** One chart with every sensor, one Y axis per metric (the
first on the left, the others on the right). Clicking a legend entry hides
or shows that sensor; the choice is remembered.

**Time windows.**

| Window | Without Worker (MQTT, Pi online) | With Worker (`GET /history`) |
|---|---|---|
| Live | Last 120 readings: hydrated from the Pi's SQLite, then live | same (Live always uses MQTT) |
| 1h / 6h / 12h / 24h | Raw points, up to 500 | Raw points, up to 500 |
| 7d | Raw points, up to 500 | Hourly buckets (avg + min/max band) |
| 1mo / 6mo / 1y | hidden | 1 h / 6 h / 1 day buckets |
| Custom (From/To) | hidden | Raw up to 2000 points for ≤ 24 h, otherwise buckets of ≥ 1 h (~1000 points) |

**Custom** opens the From/To fields (they stay folded away otherwise, to save
room, especially on a phone). **Apply** loads the range, highlights Custom and
folds the fields again; reopening Custom shows the last values.

In a historical window, new live readings are appended and points older than
the window are dropped; banded views are not appended to. Zoom and pan reset
when the window changes.

**Alert timeline.** The last 50 threshold crossings (▲ exceeded, ▼ recovered,
with the warn/crit level). It is replaced by each retained `rpi/alerts/recent`
snapshot, so it survives page reloads and daemon restarts, and stays available
while the Pi is offline. ✕ Clear alerts empties it on the Pi (readings are
kept).

**⚙ Config.** Poll interval (1 s to 1 h, global), then one row per sensor: an
*enabled* checkbox and the Warn/Crit thresholds (Warn must be below Crit). Each
change is published on `rpi/config/set`. The panel shows *Sending…*, then
*Sent*, then *Saved* once the daemon's republished `rpi/config/current`
matches, *Mismatch* if it differs, or *No ack* after 5 s. A disabled sensor
loses its card and its combined-view series. See
[workflow.md](workflow.md) for what the daemon does with each change.

**🗑 Clear Data** asks for confirmation, then publishes `rpi/cmd/clear`: the Pi
empties its SQLite `readings` and `alerts` tables, and the dashboard clears its
charts at once, ignoring any older data still in flight. Data already stored in
Cloudflare D1 is not deleted. **⬇ Export CSV** downloads the
whole D1 `readings` table from `GET /export` (Worker only).

---

## 4. Layout and responsive behaviour

On wide screens the cards use a **free-form layout**: drag a card by its header
to move it, drag its bottom-right corner to resize it, or press ⊞ Organize to
lay them out alphabetically in a grid. Positions and sizes are saved per
browser (`localStorage`), so each device keeps its own arrangement.

The page adapts to the viewport width:

| Width | Behaviour |
|---|---|
| > ~1030 px | Header on one line; alert timeline in a right-hand column |
| 901 – ~1030 px | Header actions wrap onto a second row |
| ≤ 900 px | Compact header: ↻ becomes icon-only, the other actions move into a **⋯** menu; the alert timeline moves below the sensors |
| ≤ 700 px | **Stacked layout**: cards fill the width in alphabetical order, with no drag or resize; ⊞ Organize is hidden |
| ≤ 460 px | Only the logo is kept from the brand name |

The 700 px breakpoint is defined twice and the two must stay in sync:
`STACKED_LAYOUT_QUERY` in `js/state.js` turns off dragging and layout saving,
and the matching `@media` block in `styles.css` does the stacking (a test
checks they agree). While stacked, saved positions are neither overwritten nor
clamped, so a desktop arrangement is intact when the window widens again.

**Touch screens** (`@media (pointer: coarse)`):

- buttons are at least 40 px high, inputs use a 16 px font (smaller ones make
  iOS Safari zoom in when focused), and the chart hints use touch wording;
- a vertical swipe over a chart scrolls the page (`touch-action: pan-y` on the
  canvas, overriding the `none` that Hammer.js sets);
- two-finger pinch zooms the chart's time axis; ↻ Reset appears on the card;
- one-finger chart pans are refused (`allowPanStart`), so a horizontal drag
  moves the tooltip instead. Mouse drag still pans on desktop.

---

## 5. MQTT messages

All topics use `TOPIC_PREFIX` (`rpi`). Everything the dashboard publishes uses
QoS 1 and is not retained.

### Received

| Topic | Retained | Payload |
|---|---|---|
| `rpi/status` | yes | `{"status":"online"}` or `{"status":"offline"}` (last will) |
| `rpi/<sensor>/reading` | yes | `{"value":24.30,"metric":"temperature","level":"ok","timestamp":"2026-09-29T09:41:11Z"}` |
| `rpi/<sensor>/alert` | no | `{"type":"EXCEEDED","level":"warn","value":30.40,"threshold":30.00,"metric":"temperature","timestamp":"…"}` (`type` is `EXCEEDED` or `RECOVERED`) |
| `rpi/alerts/recent` | yes | `{"alerts":[{"sensor_id":…, "type":…, "level":…, "value":…, "threshold":…, "metric":…, "timestamp":…}]}`, newest first — see [persistence.md §3b](persistence.md#3b-alert-timeline-snapshot) |
| `rpi/config/current` | yes | `{"poll_interval_ms":5000,"sensors":[{"id":"temp1","metric":"temperature","threshold_warn":30,"threshold_crit":40,"enabled":true}]}` |
| `rpi/history/resp/<request_id>` | no | `{"request_id":…, "sensor_id":…, "metric":…, "points":[{"ts":…,"value":…}], "truncated":false}` — see [persistence.md §3](persistence.md#3-mqtt-history-on-demand) |

Because readings are retained, a freshly opened dashboard immediately gets the
last value and level of every sensor. Once `rpi/config/current` has arrived,
only sensors listed there as enabled are displayed; a disabled sensor's
retained reading is ignored.

### Sent

| Topic | Payload | Sent when |
|---|---|---|
| `rpi/history/req` | `{"request_id":"<uuid>","sensor_id":"temp1","limit":120}` (plus `since_ts` for a window) | A card is created (Live hydration, 5 s timeout) or a window is selected without the Worker |
| `rpi/cmd/refresh` | `{}` | ↻ Refresh |
| `rpi/cmd/clear` | `{}` | 🗑 Clear Data |
| `rpi/cmd/clear_alerts` | `{}` | ✕ Clear alerts |
| `rpi/config/set` | `{"poll_interval_ms":5000}` | Poll interval saved |
| `rpi/config/set` | `{"sensor_id":"temp1","enabled":false}` | Sensor checkbox toggled |
| `rpi/config/set` | `{"sensor_id":"temp1","threshold_warn":30,"threshold_crit":40}` | Thresholds saved |

---

## 6. Browser storage

Preferences are kept in `localStorage`, per browser and device:

| Key | Content |
|---|---|
| `rpi-sentinel-theme` | `light` or `dark` |
| `rpi-sentinel-window` | Selected time window (`live`, `1h`, …, `365d`); a cloud-only window falls back to `live` without the Worker |
| `rpi-sentinel-viewmode` | `multi` (per-sensor cards) or `combined` |
| `rpi-sentinel-combined-hidden` | Sensor ids hidden from the combined chart |
| `rpi-sentinel-layout` | `{ "<sensor id>": { left, top, width, height } }` for the free-form layout |

Clearing site data resets all of them; nothing else is stored client-side.

---

## 7. Running it locally

No install is needed: serve the directory with any static server.

```bash
python3 -m http.server --directory dashboard 8000   # then open http://localhost:8000
```

With the placeholders left in place the page loads but stays on
*Connecting…*: MQTT.js rejects the placeholder broker URL (`Missing protocol`
in the console), so nothing is received. To try it against a real broker, put the broker URL and
credentials into a scratch copy of `index.html` (or edit it locally and revert
before committing): the dashboard user's credentials, never the daemon's.
Setting a Worker URL the same way enables the cloud windows and CSV export.

Browser devtools device emulation is enough to check the narrow and touch
layouts described in [§4](#4-layout-and-responsive-behaviour).

---

## 8. Tests

```bash
node --test dashboard/tests/*.test.mjs
```

They run on plain Node (no `npm install`) and in CI's `dashboard` job:

- **Static checks**: every script parses; the scripts concatenated in load order
  have no duplicate global declarations; `index.html` keeps the four deploy
  placeholders; `styles.css` stacks cards at `STACKED_LAYOUT_QUERY`.
- **Behavioural checks**: `state.js`, `utils.js`, `layout.js` and `combined.js`
  are run in a `vm` sandbox that mimics the page's shared scope, with stubs for
  `localStorage`, `matchMedia`, `ResizeObserver` and `crypto` but **no DOM**.
  Pure helpers (`fmt`, `escapeHtml`, `gridColumns`, `nextCardInOrder`,
  `allowPanStart`, …) are tested there; test cases can toggle a media query
  through `H.media[query] = true`.

Add cases to `dashboard/tests/dashboard.test.mjs` and expose the new function
in the sandbox epilogue (`globalThis.__T__`).

---

## 9. Conventions for changes

- **New script file**: add its `<script>` tag in the right place in
  `index.html` and the same path to `SCRIPTS` in the test file (and to the
  sandbox file list if it holds pure helpers). A file the sandbox loads must
  not touch the DOM, or any browser API the sandbox does not stub, at load time.
- **Keep helpers pure** where possible and put them in `utils.js`, so they can
  be tested without a DOM.
- **Retained topics** arrive in no particular order: handlers must cope with a
  reading before `rpi/config/current`, and with snapshots that replace state
  rather than add to it.
- **User-provided strings** (sensor ids, metrics) go into the page through
  `textContent` or `escapeHtml()`, never raw `innerHTML`.
- **Breakpoints**: a width that also changes JS behaviour must be declared on
  both sides, like `STACKED_LAYOUT_QUERY`.
- **Theme**: use the CSS custom properties from `:root` / `[data-theme="dark"]`;
  chart colours read them through `cssVar()` and are refreshed by
  `applyTheme()`.
