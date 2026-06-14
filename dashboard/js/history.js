// ── History hydration & time-window handling ────────────────────────────────────
// Note: the MQTT subscriber user needs publish on rpi/history/req
// and subscribe on rpi/history/resp/+ in the broker ACL.

// Fetch a windowed/aggregated history slice from the Cloudflare Worker and feed
// it to the chart. `params` are extra query-string fields (since_ts, until_ts,
// bucket_ms, limit, …).
function fetchCloudHistory(sensorId, params, warnLabel) {
  const url = new URL(CLOUD_WORKER_URL + '/history');
  url.searchParams.set('sensor_id', sensorId);
  for (const [k, v] of Object.entries(params)) url.searchParams.set(k, String(v));
  return fetch(url.toString())
    .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
    .then(data => { if (Array.isArray(data.points)) applyWindowHydration(sensorId, data.points); })
    .catch(err => console.warn(warnLabel || '[CloudStorage] history fetch failed:', err));
}

function requestSingleWindowHydration(sensorId, w) {
  const cfg = WINDOWS[w];
  if (!cfg) return;
  const sinceTs = Date.now() - cfg.ms;

  if (CLOUD_ENABLED) {
    const params = { since_ts: sinceTs };
    if (cfg.bucketMs) params.bucket_ms = cfg.bucketMs;
    else              params.limit     = 500;
    fetchCloudHistory(sensorId, params);
    return;
  }

  // Long (aggregated) windows have no MQTT equivalent — Cloudflare only.
  if (cfg.bucketMs) return;

  // Fallback: MQTT history-on-demand (RPi must be online).
  if (!client || !client.connected) return;
  const reqId = newRequestId();
  pendingWindowHydrations[reqId] = sensorId;
  client.publish(
    `${TOPIC_PREFIX}/history/req`,
    JSON.stringify({ request_id: reqId, sensor_id: sensorId, limit: 500, since_ts: sinceTs }),
    { qos: 1, retain: false }
  );
  setTimeout(() => { delete pendingWindowHydrations[reqId]; }, 10_000);
}

function requestHydration(sensorId) {
  if (hydratedSensors.has(sensorId)) return;
  if (pendingHydrationSet.has(sensorId)) return;
  if (!client || !client.connected) return;

  const reqId = newRequestId();
  pendingHydrations[reqId] = sensorId;
  pendingHydrationSet.add(sensorId);

  client.publish(
    `${TOPIC_PREFIX}/history/req`,
    JSON.stringify({ request_id: reqId, sensor_id: sensorId, limit: MAX_HISTORY }),
    { qos: 1, retain: false }
  );

  setTimeout(() => {
    if (pendingHydrations[reqId]) {
      delete pendingHydrations[reqId];
      pendingHydrationSet.delete(sensorId);
    }
  }, 5000);
}

function applyHydration(sensorId, points) {
  if (hydratedSensors.has(sensorId)) return;
  hydratedSensors.add(sensorId);

  if (!charts[sensorId]) return;
  if (!history[sensorId]) history[sensorId] = [];

  const live = history[sensorId];
  const liveOldestMs = live.length ? new Date(live[0].timestamp).getTime() : Infinity;

  const historical = points
    .filter(p => p.ts < liveOldestMs && p.ts >= clearedAt)
    .map(p => ({ value: p.value, timestamp: new Date(p.ts).toISOString() }));

  const merged = historical.concat(live);
  history[sensorId] = merged.length > MAX_HISTORY
    ? merged.slice(merged.length - MAX_HISTORY)
    : merged;

  const chart = charts[sensorId];
  chart.data.labels           = history[sensorId].map(h => fmt(h.timestamp));
  chart.data.datasets[0].data = history[sensorId].map(h => h.value);
  chart.update('none');
  updateStats(sensorId);
  syncCombined();
}

function applyWindowHydration(sensorId, points) {
  // Prevent a late-arriving initial-hydration response from overwriting these results.
  hydratedSensors.add(sensorId);
  pendingHydrationSet.delete(sensorId);
  if (!charts[sensorId]) return;
  const fresh = clearedAt ? points.filter(p => p.ts >= clearedAt) : points;
  const chart = charts[sensorId];
  const aggregated = fresh.length > 0 && typeof fresh[0].avg === 'number';
  chartTimestamps[sensorId]   = fresh.map(p => p.ts);
  chart.data.labels           = fresh.map(p => fmt(p.ts));
  if (aggregated) {
    // Down-sampled view: avg line + min/max band.
    chart.data.datasets[0].fill = false;
    chart.data.datasets[0].data = fresh.map(p => p.avg);
    chart.data.datasets[1].data = fresh.map(p => p.max);
    chart.data.datasets[2].data = fresh.map(p => p.min);
  } else {
    chart.data.datasets[0].fill = true;
    chart.data.datasets[0].data = fresh.map(p => p.value);
    chart.data.datasets[1].data = [];
    chart.data.datasets[2].data = [];
  }
  chart.resetZoom();
  chart.update('none');
  hideResetZoom(sensorId);
  updateStats(sensorId);
  syncCombined();
}

function setWindow(w) {
  currentWindow = w;
  try { localStorage.setItem(WINDOW_KEY, w); } catch {}
  document.querySelectorAll('.time-btn').forEach(b =>
    b.classList.toggle('active', b.dataset.w === w));

  for (const id of Object.keys(charts)) {
    charts[id].resetZoom();
    hideResetZoom(id);
  }

  if (w === 'live') {
    for (const sensorId of Object.keys(charts)) {
      const h     = history[sensorId] || [];
      const chart = charts[sensorId];
      chart.data.datasets[0].fill = true;
      chart.data.datasets[1].data = [];
      chart.data.datasets[2].data = [];
      chart.data.labels           = h.map(p => fmt(p.timestamp));
      chart.data.datasets[0].data = h.map(p => p.value);
      chart.update('none');
    }
    syncCombined();
    return;
  }

  for (const sensorId of Object.keys(charts)) {
    requestSingleWindowHydration(sensorId, w);
  }
  syncCombined();
}
