// ── Reading & alert handlers + alert timeline ───────────────────────────────────

function handleReading(sensorId, data) {
  if (knownSensorIds.size > 0 && !knownSensorIds.has(sensorId)) return;
  if (clearedAt && new Date(data.timestamp).getTime() < clearedAt) return;
  ensureCard(sensorId, data.metric ?? '');
  if (data.metric) sensorMetric[sensorId] = data.metric;

  if (!history[sensorId]) history[sensorId] = [];
  history[sensorId].push({ value: data.value, timestamp: data.timestamp });
  if (history[sensorId].length > MAX_HISTORY) history[sensorId].shift();

  const sid = domId(sensorId);
  document.getElementById('val-' + sid).textContent = data.value.toFixed(1);

  refreshStatus(sensorId, data.level);

  const chart = charts[sensorId];
  if (currentWindow === 'live') {
    chart.data.labels           = history[sensorId].map(h => fmt(h.timestamp));
    chart.data.datasets[0].data = history[sensorId].map(h => h.value);
    chart.update('none');
  } else if (aggregatedSensors.has(sensorId)) {
    // The chart holds a server-rendered avg + min/max band; a single raw live
    // point doesn't match the bucket granularity, so don't append it. Keyed on
    // what was actually returned rather than on the window, because 7d is
    // banded on the Cloudflare path but raw over MQTT.
  } else {
    // In a historical window, append the live point so the chart keeps progressing.
    const ts = new Date(data.timestamp).getTime();
    if (!chartTimestamps[sensorId]) chartTimestamps[sensorId] = [];
    chartTimestamps[sensorId].push(ts);
    chart.data.labels.push(fmt(data.timestamp));
    chart.data.datasets[0].data.push(data.value);

    // Prune points that have slid outside the selected time window.
    const windowMs = (WINDOWS[currentWindow] || {}).ms;
    if (windowMs) {
      const cutoff = Date.now() - windowMs;
      while (chartTimestamps[sensorId].length && chartTimestamps[sensorId][0] < cutoff) {
        chartTimestamps[sensorId].shift();
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
      }
    }
    chart.update('none');
  }

  updateStats(sensorId);
  syncCombined();
  document.getElementById('updated').textContent = 'Updated ' + new Date().toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit', hour12: false});
}

// Show the alert level the daemon attaches to each reading. Readings are
// retained on the broker, so a freshly opened dashboard gets the current
// state straight away instead of waiting for the next one-shot alert event.
function refreshStatus(sensorId, level) {
  const s = document.getElementById('status-' + domId(sensorId));
  if (!s) return;
  const badge = statusBadge(level);
  // An older daemon sends no level: keep whatever the badge already shows.
  if (!badge) {
    if (s.textContent === '--') { s.textContent = 'OK'; s.className = 'status ok'; }
    return;
  }
  s.textContent   = badge.text;
  s.className     = 'status ' + badge.cls;
  s.dataset.level = level;
}

function handleAlert(sensorId, data) {
  // The badge follows the level carried by readings (refreshStatus). Only a
  // daemon too old to send that level still drives the badge from alerts.
  const s = document.getElementById('status-' + domId(sensorId));
  if (s && !s.dataset.level) {
    const isExceeded = data.type === 'EXCEEDED';
    s.textContent = isExceeded ? 'Alert' : 'OK';
    s.className   = 'status ' + (isExceeded ? 'alert' : 'ok');
  }

  events.unshift({ ...data, sensor_id: sensorId });
  if (events.length > MAX_EVENTS) events.pop();
  renderEvents();
}

function renderEvents() {
  const ul = document.getElementById('events');
  document.getElementById('alert-count').textContent = events.length;
  if (!events.length) {
    ul.innerHTML = '<li><span class="empty">No alerts yet</span></li>';
    return;
  }
  ul.innerHTML = events.map(e => {
    const exceeded = e.type === 'EXCEEDED';
    const value     = Number(e.value);
    const threshold = Number(e.threshold);
    const valStr    = Number.isFinite(value)     ? value.toFixed(1)     : '--';
    const thrStr    = Number.isFinite(threshold) ? threshold.toFixed(1) : '--';
    return `
      <li>
        <span class="badge ${exceeded ? 'exceeded' : 'recovered'}">
          ${exceeded ? '▲' : '▼'}
        </span>
        <span class="event-sensor">${escapeHtml(e.sensor_id)}</span>
        <span class="ts">${escapeHtml(fmt(e.timestamp))}</span>
        <span class="det">${escapeHtml(e.metric ?? '')}=${valStr} · threshold ${thrStr}</span>
      </li>`;
  }).join('');
}
