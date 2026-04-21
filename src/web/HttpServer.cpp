#include "HttpServer.hpp"

#include <format>
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

  <div class="updated" id="updated"></div>

<script>
const charts = {};
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

function ensureCard(sensor) {
  const sid = domId(sensor.id);
  if (document.getElementById('card-' + sid)) return;

  const col = palette[Object.keys(charts).length % palette.length];

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
        tension: 0.3,
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
        tooltip: { callbacks: { label: ctx => ctx.parsed.y.toFixed(2) } }
      },
      scales: {
        x: { ticks: { color:'#475569', maxTicksLimit:6, maxRotation:0 }, grid: { color:'#1e293b' } },
        y: { ticks: { color:'#475569', callback: v => v.toFixed(1) },   grid: { color:'#334155' } }
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
        document.getElementById('val-' + sid).textContent = sensor.current_value.toFixed(1);
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

refresh();
setInterval(refresh, 2000);
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

HttpServer::HttpServer(uint16_t port, const WebState& state)
    : state_(state)
    , port_(port)
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
