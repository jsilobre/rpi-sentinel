// ── Configuration panel (poll interval + per-sensor thresholds) ─────────────────

function renderConfigPanel() {
  renderPollInterval();
  renderThresholdRows();
}

// Hidden for a daemon too old to report its poll interval in config/current.
function renderPollInterval() {
  const wrap  = document.getElementById('config-general');
  const input = document.getElementById('cfg-poll');
  wrap.style.display = pollIntervalMs == null ? 'none' : '';
  if (pollIntervalMs != null && document.activeElement !== input)
    input.value = String(pollIntervalMs / 1000);
}

function savePollInterval() {
  const input = document.getElementById('cfg-poll');
  const fb    = document.getElementById('cfg-poll-fb');
  const ms    = parsePollIntervalSeconds(input.value);
  if (ms == null) {
    fb.textContent = 'Invalid'; fb.className = 'save-feedback save-err';
    fb.title = 'Enter a number of seconds between 1 and 3600';
    return;
  }
  fb.title = '';
  if (!client || !client.connected) {
    fb.textContent = 'Offline'; fb.className = 'save-feedback save-err';
    return;
  }
  if (pendingPollSave && pendingPollSave.timer) clearTimeout(pendingPollSave.timer);
  fb.textContent = 'Sending…'; fb.className = 'save-feedback';

  client.publish(
    `${TOPIC_PREFIX}/config/set`,
    JSON.stringify({ poll_interval_ms: ms }),
    { qos: 1, retain: false },
    err => {
      if (err) {
        fb.textContent = 'Error'; fb.className = 'save-feedback save-err';
        pendingPollSave = null;
        return;
      }
      fb.textContent = 'Sent'; fb.className = 'save-feedback';
      pendingPollSave = {
        ms,
        timer: setTimeout(() => {
          if (pendingPollSave) {
            fb.textContent = 'No ack'; fb.className = 'save-feedback save-err';
            pendingPollSave = null;
          }
        }, 5000),
      };
    }
  );
}

document.getElementById('cfg-poll-save').addEventListener('click', savePollInterval);
document.getElementById('cfg-poll').addEventListener('keydown', e => {
  if (e.key === 'Enter') savePollInterval();
});

function renderThresholdRows() {
  const container = document.getElementById('config-rows');
  const ids = Object.keys(sensorThresholds);
  if (!ids.length) {
    container.innerHTML = '<span class="empty">Waiting for RPi…</span>';
    return;
  }

  // Track currently-focused input so we don't clobber an in-progress edit.
  const active   = document.activeElement;
  const activeId = active && active.id ? active.id : null;

  // Build a stable index of existing rows keyed by sensor id.
  const existing = new Map();
  container.querySelectorAll('.config-row[data-sensor-id]').forEach(r => {
    existing.set(r.dataset.sensorId, r);
  });

  // Drop rows whose sensors disappeared.
  for (const [id, row] of existing) {
    if (!(id in sensorThresholds)) row.remove();
  }
  // Clear any non-row content (e.g. the "Waiting for RPi…" placeholder).
  Array.from(container.children).forEach(c => {
    if (!c.classList || !c.classList.contains('config-row')) c.remove();
  });

  for (const id of ids) {
    const thr = sensorThresholds[id];
    const sid = domId(id);
    let row = existing.get(id);
    if (!row) {
      row = document.createElement('div');
      row.className = 'config-row';
      row.dataset.sensorId = id;
      row.innerHTML = `
        <span class="config-label"></span>
        <span class="config-field">Warn <input class="config-input" type="number" step="0.5" id="cfg-warn-${sid}"></span>
        <span class="config-field">Crit <input class="config-input" type="number" step="0.5" id="cfg-crit-${sid}"></span>
        <button class="config-save-btn" type="button">Save</button>
        <span class="save-feedback" id="cfg-fb-${sid}"></span>`;
      row.querySelector('.config-label').textContent = id;
      row.querySelector('.config-save-btn').addEventListener('click', () => saveThreshold(id, sid));
      container.appendChild(row);
    }
    const warnInput = row.querySelector('#cfg-warn-' + sid);
    const critInput = row.querySelector('#cfg-crit-' + sid);
    if (warnInput && warnInput.id !== activeId) warnInput.value = thr.warn.toFixed(1);
    if (critInput && critInput.id !== activeId) critInput.value = thr.crit.toFixed(1);
  }
}

function saveThreshold(sensorId, sid) {
  const warnEl = document.getElementById('cfg-warn-' + sid);
  const critEl = document.getElementById('cfg-crit-' + sid);
  const fb     = document.getElementById('cfg-fb-' + sid);
  const warn   = parseFloat(warnEl.value);
  const crit   = parseFloat(critEl.value);
  if (!Number.isFinite(warn) || !Number.isFinite(crit) || warn >= crit) {
    fb.textContent = 'Invalid'; fb.className = 'save-feedback save-err';
    fb.title = 'Warn must be a number strictly less than Crit';
    return;
  }
  fb.title = '';
  if (!client || !client.connected) {
    fb.textContent = 'Offline'; fb.className = 'save-feedback save-err';
    return;
  }

  // Cancel any previous pending save for this sensor.
  const prev = pendingSaves[sensorId];
  if (prev && prev.timer) clearTimeout(prev.timer);

  fb.textContent = 'Sending…'; fb.className = 'save-feedback';

  // Note: the MQTT subscriber user must have write permission on rpi/config/set in HiveMQ ACLs
  client.publish(
    `${TOPIC_PREFIX}/config/set`,
    JSON.stringify({ sensor_id: sensorId, threshold_warn: warn, threshold_crit: crit }),
    { qos: 1, retain: false },
    err => {
      if (err) {
        fb.textContent = 'Error'; fb.className = 'save-feedback save-err';
        delete pendingSaves[sensorId];
        return;
      }
      fb.textContent = 'Sent'; fb.className = 'save-feedback';
      pendingSaves[sensorId] = {
        warn, crit, sid, fb,
        timer: setTimeout(() => {
          if (pendingSaves[sensorId]) {
            fb.textContent = 'No ack'; fb.className = 'save-feedback save-err';
            delete pendingSaves[sensorId];
          }
        }, 5000),
      };
    }
  );
}

function reconcilePendingSaves() {
  if (pendingPollSave && pollIntervalMs != null) {
    const fb = document.getElementById('cfg-poll-fb');
    if (pollIntervalMs === pendingPollSave.ms) {
      fb.textContent = 'Saved'; fb.className = 'save-feedback save-ok';
    } else {
      fb.textContent = 'Mismatch'; fb.className = 'save-feedback save-err';
      fb.title = `Server applied ${pollIntervalMs / 1000} s`;
    }
    clearTimeout(pendingPollSave.timer);
    setTimeout(() => { fb.textContent = ''; fb.title = ''; }, 3000);
    pendingPollSave = null;
  }

  for (const [sensorId, p] of Object.entries(pendingSaves)) {
    const thr = sensorThresholds[sensorId];
    if (!thr) continue;
    const warnMatch = Math.abs(thr.warn - p.warn) < 1e-6;
    const critMatch = Math.abs(thr.crit - p.crit) < 1e-6;
    if (warnMatch && critMatch) {
      p.fb.textContent = 'Saved'; p.fb.className = 'save-feedback save-ok';
    } else {
      p.fb.textContent = 'Mismatch'; p.fb.className = 'save-feedback save-err';
      p.fb.title = `Server applied warn=${thr.warn}, crit=${thr.crit}`;
    }
    if (p.timer) clearTimeout(p.timer);
    setTimeout(() => { p.fb.textContent = ''; p.fb.title = ''; }, 3000);
    delete pendingSaves[sensorId];
  }
}
