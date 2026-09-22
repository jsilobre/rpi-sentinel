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

  refreshStatus(sensorId, data.value);

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

// Recompute a card's status badge from a value (defaults to the latest live
// reading) against the current thresholds. Called on every reading and
// whenever the thresholds change, so the badge never depends on having seen
// the one-shot EXCEEDED/RECOVERED alert live.
function refreshStatus(sensorId, value) {
  const s = document.getElementById('status-' + domId(sensorId));
  if (!s) return;
  if (value === undefined) {
    const h = history[sensorId];
    if (!h || !h.length) return;
    value = h[h.length - 1].value;
  }
  const level = levelFor(value, sensorThresholds[sensorId], sensorLevel[sensorId], thresholdHysteresis);
  sensorLevel[sensorId] = level;
  s.textContent = level === 'crit' ? 'Crit' : level === 'warn' ? 'Warn' : 'OK';
  s.className   = 'status ' + (level === 'crit' ? 'alert' : level);
}

function handleAlert(sensorId, data) {
  // The badge is driven by readings (refreshStatus); the alert only feeds the timeline.
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
