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
    .grid{display:grid;grid-template-columns:260px 1fr;grid-template-rows:auto auto;gap:16px}
    @media(max-width:700px){.grid{grid-template-columns:1fr}}
    .card{background:#1e293b;border-radius:12px;padding:20px}
    .card h2{font-size:.75rem;color:#64748b;text-transform:uppercase;letter-spacing:.07em;margin-bottom:16px}
    .temp-value{font-size:5rem;font-weight:700;line-height:1;letter-spacing:-2px}
    .temp-unit{font-size:1.5rem;color:#94a3b8;vertical-align:super}
    .status{display:inline-block;padding:4px 14px;border-radius:9999px;font-size:.75rem;font-weight:700;margin-top:12px;text-transform:uppercase}
    .ok   {background:#064e3b;color:#34d399}
    .alert{background:#7f1d1d;color:#f87171}
    .sensor-id{margin-top:8px;font-size:.75rem;color:#475569}
    .chart-card{grid-column:2;grid-row:1/3}
    @media(max-width:700px){.chart-card{grid-column:1;grid-row:auto}}
    canvas{width:100%!important}
    .event-list{list-style:none}
    .event-list li{padding:9px 0;border-bottom:1px solid #1e293b;font-size:.825rem;display:flex;align-items:baseline;gap:8px}
    .event-list li:last-child{border-bottom:none}
    .badge{padding:2px 8px;border-radius:4px;font-size:.7rem;font-weight:700;white-space:nowrap}
    .exceeded {background:#7f1d1d;color:#f87171}
    .recovered{background:#064e3b;color:#34d399}
    .ts{color:#475569;font-size:.75rem;margin-left:auto;white-space:nowrap}
    .empty{color:#475569;font-size:.875rem}
    .updated{font-size:.7rem;color:#334155;margin-top:16px;text-align:right}
  </style>
</head>
<body>
  <header>
    <div class="dot"></div>
    <h1>RPi Sentinel — Dashboard</h1>
  </header>
  <div class="grid">

    <div class="card">
      <h2 id="metric-label">Current Reading</h2>
      <div>
        <span class="temp-value" id="value">--</span>
      </div>
      <div><span class="status ok" id="status">--</span></div>
      <div class="sensor-id" id="sensor-id"></div>
    </div>

    <div class="card chart-card">
      <h2>History</h2>
      <canvas id="chart" height="220"></canvas>
    </div>

    <div class="card">
      <h2>Recent Alerts</h2>
      <ul class="event-list" id="events">
        <li><span class="empty">No alerts</span></li>
      </ul>
    </div>

  </div>
  <div class="updated" id="updated"></div>

<script>
const chart = new Chart(document.getElementById('chart').getContext('2d'), {
  type: 'line',
  data: {
    labels: [],
    datasets: [{
      label: 'Value',
      data: [],
      borderColor: '#38bdf8',
      backgroundColor: 'rgba(56,189,248,0.08)',
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
    scales: {
      x: { ticks: { color:'#475569', maxTicksLimit:8, maxRotation:0 }, grid: { color:'#1e293b' } },
      y: { ticks: { color:'#475569' }, grid: { color:'#334155' } }
    },
    plugins: { legend:{ display:false }, tooltip:{ callbacks:{ label: ctx => ctx.parsed.y.toFixed(2) } } }
  }
});

function fmt(iso) {
  return new Date(iso).toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit'});
}

async function refresh() {
  try {
    const d = await fetch('/api/state').then(r => r.json());

    if (d.has_reading) {
      document.getElementById('value').textContent = d.current_value.toFixed(1);
      document.getElementById('metric-label').textContent = d.current_metric || 'Current Reading';
      document.getElementById('sensor-id').textContent = d.current_sensor_id;
      const s = document.getElementById('status');
      s.textContent = d.status === 'ok' ? 'OK' : 'Alert';
      s.className = 'status ' + d.status;
    }

    chart.data.labels           = d.history.map(h => fmt(h.timestamp));
    chart.data.datasets[0].data = d.history.map(h => h.value);
    chart.update('none');

    const ul = document.getElementById('events');
    if (!d.recent_events.length) {
      ul.innerHTML = '<li><span class="empty">No alerts</span></li>';
    } else {
      ul.innerHTML = d.recent_events.map(e => `
        <li>
          <span class="badge ${e.type==='EXCEEDED'?'exceeded':'recovered'}">
            ${e.type==='EXCEEDED'?'▲ Exceeded':'▼ Recovered'}
          </span>
          ${e.sensor_id} &nbsp;${e.metric}=${e.value.toFixed(1)} &nbsp;/&nbsp; threshold ${e.threshold.toFixed(1)}
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

std::string HttpServer::compute_status(const WebState::Snapshot& snap) const
{
    for (const auto& e : snap.recent_events) {
        if (e.type == SensorEvent::Type::ThresholdExceeded)  return "alert";
        if (e.type == SensorEvent::Type::ThresholdRecovered) return "ok";
    }
    return "ok";
}

std::string HttpServer::build_state_json(const WebState::Snapshot& snap) const
{
    std::ostringstream out;
    out << std::boolalpha;
    out << "{\n";
    out << std::format("  \"has_reading\": {},\n",        snap.has_reading);
    out << std::format("  \"current_value\": {:.2f},\n",  snap.current_value);
    out << std::format("  \"current_sensor_id\": \"{}\",\n", snap.current_sensor_id);
    out << std::format("  \"current_metric\": \"{}\",\n",    snap.current_metric);
    out << std::format("  \"status\": \"{}\",\n",         compute_status(snap));

    // history
    out << "  \"history\": [";
    for (size_t i = 0; i < snap.history.size(); ++i) {
        const auto& h = snap.history[i];
        out << std::format(
            "\n    {{\"sensor_id\": \"{}\", \"metric\": \"{}\", \"value\": {:.2f}, \"timestamp\": \"{}\"}}",
            h.sensor_id, h.metric, h.value, format_iso8601(h.timestamp));
        if (i + 1 < snap.history.size()) out << ',';
    }
    out << "\n  ],\n";

    // recent_events
    out << "  \"recent_events\": [";
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
