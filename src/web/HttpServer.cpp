#include "HttpServer.hpp"

#include <format>
#include <nlohmann/json.hpp>
#include <print>
#include <sstream>

// httplib included as SYSTEM to suppress warnings from its headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#include <httplib.h>
#pragma GCC diagnostic pop

namespace rpi {

// ─── Embedded dashboard HTML ──────────────────────────────────────────────────

static constexpr std::string_view DASHBOARD_HTML = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>RPi Sentinel</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh;padding:24px}
    header{display:flex;align-items:center;gap:10px;margin-bottom:24px}
    header h1{font-size:1.4rem;color:#94a3b8}
    .dot{width:10px;height:10px;border-radius:50%;background:#22c55e;animation:pulse 2s infinite}
    @keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}

    /* Sensor cards grid — fills columns dynamically */
    .sensors-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:16px;margin-bottom:16px}

    .card{background:#1e293b;border-radius:12px;padding:20px}
    .card h2{font-size:.75rem;color:#64748b;text-transform:uppercase;letter-spacing:.07em;margin-bottom:12px;display:flex;align-items:center;gap:8px}
    .metric-tag{background:#0f172a;color:#475569;border-radius:4px;padding:2px 6px;font-size:.65rem;font-weight:600;text-transform:lowercase;letter-spacing:0}

    .sensor-value{font-size:3.5rem;font-weight:700;line-height:1;letter-spacing:-2px}
    .status{display:inline-block;padding:3px 12px;border-radius:9999px;font-size:.7rem;font-weight:700;margin-top:8px;text-transform:uppercase}
    .ok   {background:#064e3b;color:#34d399}
    .alert{background:#7f1d1d;color:#f87171}

    /* Recent alerts */
    .alerts-card{margin-top:0}
    .event-list{list-style:none}
    .event-list li{padding:9px 0;border-bottom:1px solid #0f172a;font-size:.825rem;display:flex;align-items:baseline;gap:8px;flex-wrap:wrap}
    .event-list li:last-child{border-bottom:none}
    .badge{padding:2px 8px;border-radius:4px;font-size:.7rem;font-weight:700;white-space:nowrap}
    .exceeded {background:#7f1d1d;color:#f87171}
    .recovered{background:#064e3b;color:#34d399}
    .event-sensor{color:#94a3b8;font-weight:600;font-size:.75rem}
    .ts{color:#475569;font-size:.75rem;margin-left:auto;white-space:nowrap}
    .empty{color:#475569;font-size:.875rem}
    .updated{font-size:.7rem;color:#334155;margin-top:12px;text-align:right}

    /* Config card */
    .config-row{display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid #0f172a;flex-wrap:wrap}
    .config-row:last-child{border-bottom:none}
    .config-label{color:#94a3b8;font-size:.8rem;font-weight:600;min-width:100px}
    .config-field{display:flex;align-items:center;gap:6px;font-size:.75rem;color:#64748b}
    .config-input{width:72px;background:#0f172a;border:1px solid #334155;border-radius:6px;color:#e2e8f0;font-size:.8rem;padding:4px 8px;text-align:right}
    .config-input:focus{outline:none;border-color:#38bdf8}
    .config-save-btn{background:#1d4ed8;color:#e2e8f0;border:none;border-radius:6px;padding:4px 12px;font-size:.75rem;font-weight:600;cursor:pointer;margin-left:auto}
    .config-save-btn:hover{background:#2563eb}
    .save-feedback{font-size:.7rem;min-width:60px}
    .save-ok{color:#34d399}.save-err{color:#f87171}
  </style>
</head>
<body>
  <header>
    <div class="dot"></div>
    <h1>RPi Sentinel — Dashboard</h1>
  </header>

  <div class="sensors-grid" id="sensors-grid"></div>

  <div class="card alerts-card">
    <h2>Recent Alerts</h2>
    <ul class="event-list" id="events">
      <li><span class="empty">No alerts</span></li>
    </ul>
  </div>

  <div class="card" style="margin-top:16px">
    <h2>Configuration</h2>
    <div id="config-rows"><span class="empty">Loading…</span></div>
  </div>

  <div class="updated" id="updated"></div>

<script>
const charts = {};
const sensorMeta = {};
const palette = [
  { border: '#38bdf8', bg: 'rgba(56,189,248,0.08)'  },
  { border: '#a78bfa', bg: 'rgba(167,139,250,0.08)' },
  { border: '#34d399', bg: 'rgba(52,211,153,0.08)'  },
  { border: '#fb923c', bg: 'rgba(251,146,60,0.08)'  },
  { border: '#f472b6', bg: 'rgba(244,114,182,0.08)' },
];

function domId(sensorId) {
  return sensorId.replace(/[^a-zA-Z0-9_-]/g, '_');
}

function formatValue(metric, value) {
  if (metric === 'motion')      return value >= 0.5 ? 'Detected' : 'Clear';
  if (metric === 'pressure')    return value.toFixed(0) + ' hPa';
  if (metric === 'humidity')    return value.toFixed(1) + '%';
  if (metric === 'temperature') return value.toFixed(1) + '°C';
  return value.toFixed(1);
}

function ensureCard(sensor) {
  sensorMeta[sensor.id] = sensor.metric;
  const sid = domId(sensor.id);
  if (document.getElementById('card-' + sid)) return;

  const col     = palette[Object.keys(charts).length % palette.length];
  const isMotion = sensor.metric === 'motion';

  const card = document.createElement('div');
  card.id        = 'card-' + sid;
  card.className = 'card';
  card.innerHTML = `
    <h2>${sensor.id}<span class="metric-tag">${sensor.metric}</span></h2>
    <div><span class="sensor-value" id="val-${sid}">--</span></div>
    <span class="status ok" id="status-${sid}">--</span>
    <canvas id="chart-${sid}" height="140" style="margin-top:14px"></canvas>
  `;
  document.getElementById('sensors-grid').appendChild(card);

  charts[sensor.id] = new Chart(
    document.getElementById('chart-' + sid).getContext('2d'), {
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        data: [],
        borderColor: col.border,
        backgroundColor: col.bg,
        borderWidth: 2,
        tension: isMotion ? 0 : 0.3,
        stepped: isMotion ? 'before' : false,
        fill: true,
        pointRadius: 0,
      }]
    },
    options: {
      responsive: true,
      animation: false,
      interaction: { intersect: false, mode: 'index' },
      plugins: {
        legend: { display: false },
        tooltip: { callbacks: { label: ctx => {
          const m = sensorMeta[sensor.id];
          if (m === 'motion') return ctx.parsed.y >= 0.5 ? 'Detected' : 'Clear';
          return formatValue(m, ctx.parsed.y);
        }}}
      },
      scales: {
        x: { ticks: { color:'#475569', maxTicksLimit:6, maxRotation:0 }, grid: { color:'#1e293b' } },
        y: isMotion
          ? { min: -0.1, max: 1.5, ticks: { color:'#475569', callback: v => v === 0 ? 'Clear' : v === 1 ? 'On' : '' }, grid: { color:'#334155' } }
          : { ticks: { color:'#475569', callback: v => formatValue(sensorMeta[sensor.id], v) }, grid: { color:'#334155' } }
      }
    }
  });
}

function fmt(iso) {
  return new Date(iso).toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit'});
}

async function refresh() {
  try {
    const d = await fetch('/api/state').then(r => r.json());

    for (const sensor of d.sensors) {
      ensureCard(sensor);
      const sid = domId(sensor.id);

      if (sensor.has_reading) {
        document.getElementById('val-' + sid).textContent = formatValue(sensor.metric, sensor.current_value);
        const s = document.getElementById('status-' + sid);
        s.textContent = sensor.status === 'ok' ? 'OK' : 'Alert';
        s.className   = 'status ' + sensor.status;
      }

      const chart = charts[sensor.id];
      chart.data.labels           = sensor.history.map(h => fmt(h.timestamp));
      chart.data.datasets[0].data = sensor.history.map(h => h.value);
      chart.update('none');
    }

    const ul = document.getElementById('events');
    if (!d.recent_events.length) {
      ul.innerHTML = '<li><span class="empty">No alerts</span></li>';
    } else {
      ul.innerHTML = d.recent_events.map(e => `
        <li>
          <span class="badge ${e.type==='EXCEEDED'?'exceeded':'recovered'}">
            ${e.type==='EXCEEDED'?'▲ Exceeded':'▼ Recovered'}
          </span>
          <span class="event-sensor">${e.sensor_id}</span>
          ${e.metric}=${e.value.toFixed(1)} &nbsp;/&nbsp; threshold&nbsp;${e.threshold.toFixed(1)}
          <span class="ts">${fmt(e.timestamp)}</span>
        </li>`).join('');
    }

    document.getElementById('updated').textContent =
      'Updated: ' + new Date().toLocaleTimeString();
  } catch(e) { console.warn('fetch error', e); }
}

async function loadConfig() {
  try {
    const d = await fetch('/api/config').then(r => r.json());
    const container = document.getElementById('config-rows');
    container.innerHTML = '';
    for (const s of d.sensors) {
      const sid = s.id.replace(/[^a-zA-Z0-9_-]/g, '_');
      const row = document.createElement('div');
      row.className = 'config-row';
      row.innerHTML = `
        <span class="config-label">${s.id}</span>
        <span class="config-field">Warn <input class="config-input" type="number" step="0.5" id="cfg-warn-${sid}" value="${s.threshold_warn.toFixed(1)}"></span>
        <span class="config-field">Crit <input class="config-input" type="number" step="0.5" id="cfg-crit-${sid}" value="${s.threshold_crit.toFixed(1)}"></span>
        <button class="config-save-btn" onclick="saveThreshold('${s.id}','${sid}')">Save</button>
        <span class="save-feedback" id="cfg-fb-${sid}"></span>`;
      container.appendChild(row);
    }
  } catch(e) { console.warn('config fetch error', e); }
}

async function saveThreshold(sensorId, sid) {
  const warn = parseFloat(document.getElementById('cfg-warn-' + sid).value);
  const crit = parseFloat(document.getElementById('cfg-crit-' + sid).value);
  const fb   = document.getElementById('cfg-fb-' + sid);
  if (isNaN(warn) || isNaN(crit) || warn <= 0 || crit <= 0 || warn >= crit) {
    fb.textContent = 'Invalid'; fb.className = 'save-feedback save-err'; return;
  }
  try {
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({sensors:[{id: sensorId, threshold_warn: warn, threshold_crit: crit}]})
    });
    const body = await res.json();
    if (res.ok) {
      fb.textContent = 'Saved'; fb.className = 'save-feedback save-ok';
    } else {
      fb.textContent = body.error ?? 'Error'; fb.className = 'save-feedback save-err';
    }
  } catch(e) { fb.textContent = 'Error'; fb.className = 'save-feedback save-err'; }
  setTimeout(() => { fb.textContent = ''; }, 3000);
}

refresh();
setInterval(refresh, 2000);
loadConfig();
</script>
</body>
</html>
)HTML";

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string format_iso8601(std::chrono::system_clock::time_point tp)
{
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ─── HttpServer ────────────────────────────────────────────────────────────────

HttpServer::HttpServer(uint16_t port, const WebState& state,
                       ConfigGetter config_getter, ConfigUpdater config_updater)
    : state_(state)
    , port_(port)
    , config_getter_(std::move(config_getter))
    , config_updater_(std::move(config_updater))
    , server_(std::make_unique<httplib::Server>())
{}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start()
{
    server_->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(std::string(DASHBOARD_HTML), "text/html; charset=utf-8");
    });

    server_->Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        auto snap = state_.snapshot();
        res.set_content(build_state_json(snap), "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    server_->Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(config_getter_(), "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    server_->Options("/api/config", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    server_->Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }
        if (!body.contains("sensors") || !body["sensors"].is_array()) {
            res.status = 400;
            res.set_content(R"({"error":"missing sensors array"})", "application/json");
            return;
        }
        for (const auto& s : body["sensors"]) {
            if (!s.contains("id") || !s.contains("threshold_warn") || !s.contains("threshold_crit")) {
                res.status = 400;
                res.set_content(R"({"error":"each sensor needs id, threshold_warn, threshold_crit"})", "application/json");
                return;
            }
            const auto id   = s["id"].get<std::string>();
            const float warn = s["threshold_warn"].get<float>();
            const float crit = s["threshold_crit"].get<float>();
            if (warn <= 0.0f || crit <= 0.0f || warn >= crit) {
                res.status = 400;
                res.set_content(std::format(
                    "{{\"error\":\"sensor '{}': warn and crit must be > 0 and warn < crit\"}}", id),
                    "application/json");
                return;
            }
            if (auto r = config_updater_(id, warn, crit); !r) {
                res.status = 400;
                res.set_content(std::format("{{\"error\":\"{}\"}}", r.error()), "application/json");
                return;
            }
        }
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    thread_ = std::thread([this] {
        std::println("[HttpServer] Listening on http://0.0.0.0:{}", port_);
        server_->listen("0.0.0.0", static_cast<int>(port_));
    });
}

void HttpServer::stop()
{
    server_->stop();
    if (thread_.joinable()) thread_.join();
}

std::string HttpServer::build_state_json(const WebState::Snapshot& snap) const
{
    std::ostringstream out;
    out << std::boolalpha;
    out << "{\n  \"sensors\": [";

    for (size_t i = 0; i < snap.sensors.size(); ++i) {
        const auto& s = snap.sensors[i];
        out << "\n    {\n";
        out << std::format("      \"id\": \"{}\",\n",            s.id);
        out << std::format("      \"metric\": \"{}\",\n",        s.metric);
        out << std::format("      \"has_reading\": {},\n",       s.has_reading);
        out << std::format("      \"current_value\": {:.2f},\n", s.current_value);
        out << std::format("      \"status\": \"{}\",\n",        s.status);
        out << "      \"history\": [";
        for (size_t j = 0; j < s.history.size(); ++j) {
            const auto& h = s.history[j];
            out << std::format("\n        {{\"value\": {:.2f}, \"timestamp\": \"{}\"}}",
                h.value, format_iso8601(h.timestamp));
            if (j + 1 < s.history.size()) out << ',';
        }
        out << "\n      ]\n    }";
        if (i + 1 < snap.sensors.size()) out << ',';
    }

    out << "\n  ],\n  \"recent_events\": [";
    for (size_t i = 0; i < snap.recent_events.size(); ++i) {
        const auto& e = snap.recent_events[i];
        std::string_view type = (e.type == SensorEvent::Type::ThresholdExceeded)
            ? "EXCEEDED" : "RECOVERED";
        out << std::format(
            "\n    {{\"type\": \"{}\", \"metric\": \"{}\", \"value\": {:.2f}, "
            "\"threshold\": {:.2f}, \"sensor_id\": \"{}\", \"timestamp\": \"{}\"}}",
            type, e.metric, e.value, e.threshold, e.sensor_id, format_iso8601(e.timestamp));
        if (i + 1 < snap.recent_events.size()) out << ',';
    }
    out << "\n  ]\n}";

    return out.str();
}

} // namespace rpi
