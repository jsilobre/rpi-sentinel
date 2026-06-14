// ── Wiring & startup ────────────────────────────────────────────────────────────
// Loaded last: every function and constant from the other scripts is defined by
// the time this file runs, and the DOM is fully parsed (scripts sit at end of body).

// ── Connection status indicator ─────────────────────────────────────────────────
function setConnStatus(state) {
  const dot   = document.getElementById('conn-dot');
  const label = document.getElementById('conn-label');
  dot.className = 'dot ' + state;
  label.textContent = {
    connected:    'RPi online',
    rpi_offline:  'RPi offline',
    disconnected: 'Disconnected',
    error:        'Connection error',
    connecting:   'Connecting…',
  }[state] ?? state;
  updateRefreshButtonState();
}

function updateRefreshButtonState() {
  const btn = document.getElementById('refresh-btn');
  if (!btn) return;
  // `client` is declared later as a `const` and we may be called before init.
  let online = false;
  try { online = !!(client && client.connected); } catch { online = false; }
  if (btn.dataset.busy === '1') return;
  btn.disabled = !online;
  btn.title = online ? 'Force immediate sensor poll' : 'Disconnected — refresh unavailable';
}

// ── Organize sensors alphabetically ─────────────────────────────────────────────
document.getElementById('organize-btn').addEventListener('click', organizeSensors);

// ── Refresh (force-poll) ─────────────────────────────────────────────────────────
document.getElementById('refresh-btn').addEventListener('click', () => {
  const btn = document.getElementById('refresh-btn');
  if (!client || !client.connected) return;
  btn.dataset.busy = '1';
  btn.disabled = true;
  btn.textContent = '↻ Refreshing…';
  client.publish(`${TOPIC_PREFIX}/cmd/refresh`, '{}', { qos: 1, retain: false });
  setTimeout(() => {
    btn.dataset.busy = '';
    btn.textContent = '↻ Refresh';
    updateRefreshButtonState();
  }, 2000);
});

// ── Export full database (CSV) ───────────────────────────────────────────────────
// Streams the entire Cloudflare D1 readings table as a CSV file. Enabled only when
// cloud storage is configured (the local 7-day SQLite store is not exported here).
(function initExportButton() {
  const btn = document.getElementById('export-btn');
  if (!CLOUD_ENABLED) return;  // leave disabled, with the "cloud required" tooltip
  btn.disabled = false;
  btn.title = 'Export all stored data as CSV';
  btn.addEventListener('click', () => {
    const a = document.createElement('a');
    a.href = CLOUD_WORKER_URL + '/export';
    a.download = `rpi-sentinel-export-${new Date().toISOString().slice(0, 10)}.csv`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    const prev = btn.textContent;
    btn.textContent = '⬇ Exporting…';
    setTimeout(() => { btn.textContent = prev; }, 2000);
  });
})();

// ── Clear all stored history ─────────────────────────────────────────────────────
document.getElementById('clear-btn').addEventListener('click', () => {
  if (!confirm('Supprimer toutes les données enregistrées ? Cette action est irréversible.')) return;
  const btn = document.getElementById('clear-btn');
  if (!client || !client.connected) {
    alert('Non connecté au broker MQTT.');
    return;
  }
  btn.disabled = true;
  btn.textContent = '⏳ Clearing…';
  client.publish(`${TOPIC_PREFIX}/cmd/clear`, '{}', { qos: 1, retain: false });

  // Drop in-memory state immediately so charts go blank without waiting for MQTT.
  // Stale history responses already in flight are filtered by clearedAt below.
  clearedAt = Date.now();
  for (const sensorId of Object.keys(history)) history[sensorId] = [];
  for (const sensorId of Object.keys(chartTimestamps)) chartTimestamps[sensorId] = [];
  hydratedSensors.clear();
  for (const sensorId of Object.keys(charts)) {
    const chart = charts[sensorId];
    chart.data.labels           = [];
    chart.data.datasets[0].data = [];
    chart.update('none');
    const sid = domId(sensorId);
    const statsEl = document.getElementById('stats-' + sid);
    if (statsEl) statsEl.innerHTML = '';
    const trendEl = document.getElementById('trend-' + sid);
    if (trendEl) { trendEl.className = 'trend'; trendEl.textContent = ''; }
  }
  events.length = 0;
  renderEvents();
  syncCombined();

  setTimeout(() => {
    btn.disabled = false;
    btn.textContent = '🗑 Clear Data';
  }, 2000);
});

// ── Time-window buttons (event delegation) ───────────────────────────────────────
document.getElementById('time-bar').addEventListener('click', e => {
  const btn = e.target.closest('.time-btn[data-w]');
  if (!btn) return;
  setWindow(btn.dataset.w);
});

// ── Custom time range (only shown when CLOUD_ENABLED) ────────────────────────────
if (CLOUD_ENABLED) {
  document.getElementById('custom-range-wrap').style.display = 'flex';
  // The 1mo / 6mo / 1y windows are Cloudflare-only (no MQTT equivalent).
  document.querySelectorAll('.time-btn[data-cloud]').forEach(b => { b.style.display = ''; });

  document.getElementById('custom-apply-btn').addEventListener('click', () => {
    const fromVal = document.getElementById('custom-from').value;
    const toVal   = document.getElementById('custom-to').value;
    if (!fromVal || !toVal) return;
    const fromMs = new Date(fromVal).getTime();
    const toMs   = new Date(toVal).getTime();
    if (isNaN(fromMs) || isNaN(toMs) || fromMs >= toMs) return;

    // Deactivate fixed-window buttons for visual clarity.
    document.querySelectorAll('.time-btn[data-w]').forEach(b => b.classList.remove('active'));
    document.getElementById('custom-apply-btn').classList.add('active');

    // Ranges wider than 7d are down-sampled (banded) so the response stays
    // small; pick a bucket (>= 1h) that keeps it to roughly 1000 points.
    const spanMs   = toMs - fromMs;
    const bucketMs = spanMs > WINDOWS['7d'].ms
      ? Math.max(3_600_000, Math.ceil(spanMs / 1000 / 3_600_000) * 3_600_000)
      : 0;

    for (const sensorId of Object.keys(charts)) {
      const params = { since_ts: fromMs, until_ts: toMs };
      if (bucketMs) params.bucket_ms = bucketMs;
      else          params.limit     = 2000;
      fetchCloudHistory(sensorId, params, '[CloudStorage] custom range fetch failed:');
    }
  });
}

// ── Theme toggle ─────────────────────────────────────────────────────────────────
function applyTheme(t) {
  document.documentElement.setAttribute('data-theme', t);
  document.getElementById('theme-btn').textContent = t === 'dark' ? '☀' : '☾';
  for (const id in charts) {
    const c = charts[id];
    c.options.scales.x.ticks.color = cssVar('--muted-2');
    c.options.scales.x.grid.color  = cssVar('--grid');
    c.options.scales.y.ticks.color = cssVar('--muted-2');
    c.options.scales.y.grid.color  = cssVar('--grid');
    c.update('none');
    // Annotation label colours read CSS vars at draw time, so refresh them.
    updateAnnotations(id);
  }
  if (combinedChart) {
    if (viewMode === 'combined') {
      syncCombined();  // rebuilds axes/legend with fresh CSS-var colours
    } else {
      combinedChart.options.scales.x.ticks.color = cssVar('--muted-2');
      combinedChart.options.scales.x.grid.color  = cssVar('--grid');
      combinedChart.options.plugins.legend.labels.color = cssVar('--muted');
      combinedChart.update('none');
    }
  }
}
document.getElementById('theme-btn').addEventListener('click', () => {
  const current = document.documentElement.getAttribute('data-theme') || 'light';
  const next = current === 'dark' ? 'light' : 'dark';
  try { localStorage.setItem('rpi-sentinel-theme', next); } catch {}
  applyTheme(next);
});
applyTheme(localStorage.getItem('rpi-sentinel-theme') || 'light');

// Restore persisted time window selection.
document.querySelectorAll('.time-btn').forEach(b =>
  b.classList.toggle('active', b.dataset.w === currentWindow));

// ── Config modal ─────────────────────────────────────────────────────────────────
const modal           = document.getElementById('config-modal');
const modalDialog     = modal.querySelector('.modal');
modal.setAttribute('role', 'dialog');
modal.setAttribute('aria-modal', 'true');
modal.setAttribute('aria-hidden', 'true');
modalDialog.setAttribute('aria-labelledby', 'config-modal-title');
modal.querySelector('.modal-head h3').id = 'config-modal-title';

let lastFocusedBeforeModal = null;
function openModal() {
  lastFocusedBeforeModal = document.activeElement;
  modal.classList.add('open');
  modal.setAttribute('aria-hidden', 'false');
  // Focus the close button by default.
  setTimeout(() => document.getElementById('modal-close').focus(), 0);
}
function closeModal() {
  modal.classList.remove('open');
  modal.setAttribute('aria-hidden', 'true');
  if (lastFocusedBeforeModal && lastFocusedBeforeModal.focus) {
    lastFocusedBeforeModal.focus();
  }
}
document.getElementById('config-btn').addEventListener('click', openModal);
document.getElementById('modal-close').addEventListener('click', closeModal);
modal.addEventListener('click', e => { if (e.target === modal) closeModal(); });
document.addEventListener('keydown', e => {
  if (!modal.classList.contains('open')) return;
  if (e.key === 'Escape') { closeModal(); return; }
  if (e.key !== 'Tab') return;
  // Simple focus trap inside the dialog.
  const focusables = modalDialog.querySelectorAll(
    'a[href], button:not([disabled]), input:not([disabled]), select, textarea, [tabindex]:not([tabindex="-1"])'
  );
  if (!focusables.length) return;
  const first = focusables[0];
  const last  = focusables[focusables.length - 1];
  if (e.shiftKey && document.activeElement === first) { last.focus(); e.preventDefault(); }
  else if (!e.shiftKey && document.activeElement === last) { first.focus(); e.preventDefault(); }
});

// ── View mode (per-sensor / combined) ────────────────────────────────────────────
document.getElementById('viewmode-btn').addEventListener('click', () =>
  setViewMode(viewMode === 'combined' ? 'multi' : 'combined'));
setViewMode(viewMode);

// ── MQTT connection ──────────────────────────────────────────────────────────────
const client = mqtt.connect(BROKER_WSS, {
  username:        MQTT_USER,
  password:        MQTT_PASS,
  reconnectPeriod: 3000,
  keepalive:       60,
  clean:           true,
  connectTimeout:  10000,
});

client.on('connect', () => {
  client.subscribe([
    `${TOPIC_PREFIX}/+/reading`,
    `${TOPIC_PREFIX}/+/alert`,
    `${TOPIC_PREFIX}/status`,
    `${TOPIC_PREFIX}/config/current`,
    `${TOPIC_PREFIX}/history/resp/+`,
  ], err => { if (err) console.error('Subscribe error:', err); });

  // Re-request hydration for any sensors that already have a card but
  // haven't been hydrated yet (e.g. after a reconnect).
  for (const sensorId of Object.keys(charts)) {
    requestHydration(sensorId);
    if (currentWindow !== 'live') requestSingleWindowHydration(sensorId, currentWindow);
  }

  updateRefreshButtonState();

  // On first connect only: fall back to rpi_offline if status never arrives.
  if (firstConnect) {
    firstConnect = false;
    setTimeout(() => {
      const label = document.getElementById('conn-label').textContent;
      if (label === 'Connecting…') setConnStatus('rpi_offline');
    }, 5000);
  }
});

client.on('disconnect', () => setConnStatus('disconnected'));
client.on('offline',    () => setConnStatus('disconnected'));
client.on('error',      () => setConnStatus('error'));
// On reconnect, restore last known RPi state immediately — no offline flash.
client.on('reconnect',  () => {
  if      (lastRpiStatus === 'online')  setConnStatus('connected');
  else if (lastRpiStatus === 'offline') setConnStatus('rpi_offline');
  else                                  setConnStatus('connecting');
});

client.on('message', (topic, message) => {
  let data;
  try { data = JSON.parse(message.toString()); }
  catch { console.warn('Malformed MQTT payload on', topic); return; }

  if (topic === `${TOPIC_PREFIX}/status`) {
    lastRpiStatus = data.status === 'online' ? 'online' : 'offline';
    setConnStatus(lastRpiStatus === 'online' ? 'connected' : 'rpi_offline');
    return;
  }

  if (topic.startsWith(`${TOPIC_PREFIX}/history/resp/`)) {
    const reqId = topic.slice((`${TOPIC_PREFIX}/history/resp/`).length);
    if (pendingWindowHydrations[reqId]) {
      const sensorId = pendingWindowHydrations[reqId];
      delete pendingWindowHydrations[reqId];
      if (Array.isArray(data.points)) applyWindowHydration(sensorId, data.points);
      return;
    }
    const sensorId = pendingHydrations[reqId];
    if (!sensorId) return;
    delete pendingHydrations[reqId];
    pendingHydrationSet.delete(sensorId);
    if (Array.isArray(data.points)) applyHydration(sensorId, data.points);
    return;
  }

  if (topic === `${TOPIC_PREFIX}/config/current`) {
    const incoming = data.sensors ?? [];

    knownSensorIds.clear();
    incoming.forEach(s => {
      knownSensorIds.add(s.id);
      sensorThresholds[s.id] = { warn: s.threshold_warn, crit: s.threshold_crit };
    });

    for (const sensorId in charts) {
      if (!knownSensorIds.has(sensorId)) {
        charts[sensorId].destroy();
        delete charts[sensorId];
        delete history[sensorId];
        delete chartTimestamps[sensorId];
        hydratedSensors.delete(sensorId);
        pendingHydrationSet.delete(sensorId);
        delete sensorMetric[sensorId];
        delete sensorColor[sensorId];
        combinedHidden.delete(sensorId);
        const cardEl = document.getElementById('card-' + domId(sensorId));
        if (cardEl) {
          cardResizeObserver.unobserve(cardEl);
          cardEl.remove();
        }
      } else {
        updateAnnotations(sensorId);
      }
    }
    updateGridHeight();

    const grid = document.getElementById('sensors-grid');
    if (!grid.querySelector('.card')) {
      const ph = document.createElement('div');
      ph.id        = 'placeholder-card';
      ph.className = 'card';
      ph.innerHTML = '<p class="placeholder">Waiting for sensor data…</p>';
      grid.appendChild(ph);
    }

    document.getElementById('sensor-count').textContent =
      grid.querySelectorAll('.card:not(#placeholder-card)').length;

    renderConfigPanel();
    reconcilePendingSaves();
    saveCombinedHidden();
    syncCombined();
    return;
  }

  const parts = topic.split('/');
  if (parts.length < 3) return;
  const kind     = parts[parts.length - 1];
  const sensorId = parts[parts.length - 2];

  if (kind === 'reading') handleReading(sensorId, data);
  else if (kind === 'alert') handleAlert(sensorId, data);
});
