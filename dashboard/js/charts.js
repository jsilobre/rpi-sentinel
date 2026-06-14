// ── Per-sensor cards & charts ───────────────────────────────────────────────────

function updateAnnotations(sensorId) {
  const chart = charts[sensorId];
  if (!chart) return;
  const t = sensorThresholds[sensorId];
  if (!t) { chart.options.plugins.annotation.annotations = {}; chart.update('none'); return; }
  chart.options.plugins.annotation.annotations = {
    warnLine: {
      type:'line', yMin:t.warn, yMax:t.warn,
      borderColor:'rgba(245,158,11,0.55)', borderWidth:1, borderDash:[4,4],
      label:{display:true,content:'Warn',position:'end',
             backgroundColor:'rgba(245,158,11,0.12)',color:cssVar('--warn-fg'),font:{size:10}}
    },
    critLine: {
      type:'line', yMin:t.crit, yMax:t.crit,
      borderColor:'rgba(239,68,68,0.55)', borderWidth:1, borderDash:[4,4],
      label:{display:true,content:'Crit',position:'end',
             backgroundColor:'rgba(239,68,68,0.12)',color:cssVar('--err-fg'),font:{size:10}}
    }
  };
  chart.update('none');
}

function resetZoom(sensorId) {
  if (charts[sensorId]) charts[sensorId].resetZoom();
  hideResetZoom(sensorId);
}

function updateStats(sensorId) {
  const sid   = domId(sensorId);
  const chart = charts[sensorId];
  const data  = chart ? chart.data.datasets[0].data : [];
  if (!data.length) return;
  let min = Infinity, max = -Infinity, sum = 0;
  for (const v of data) { if (v < min) min = v; if (v > max) max = v; sum += v; }
  const avg = sum / data.length;
  document.getElementById('stats-' + sid).innerHTML =
    `<span>min <b>${min.toFixed(1)}</b></span><span>avg <b>${avg.toFixed(1)}</b></span><span>max <b>${max.toFixed(1)}</b></span>`;

  const trendEl = document.getElementById('trend-' + sid);
  if (data.length >= 5) {
    const last = data[data.length - 1];
    const prev = data[data.length - 5];
    const diff = last - prev;
    const pct  = Math.abs(prev) > 1e-9 ? (diff / Math.abs(prev)) * 100 : 0;
    if (Math.abs(pct) < 0.5) {
      trendEl.className = 'trend'; trendEl.textContent = '→ stable';
    } else if (diff > 0) {
      trendEl.className = 'trend up'; trendEl.textContent = `↑ ${diff >= 0 ? '+' : ''}${diff.toFixed(1)}`;
    } else {
      trendEl.className = 'trend down'; trendEl.textContent = `↓ ${diff.toFixed(1)}`;
    }
  }
}

function ensureCard(sensorId, metric) {
  if (knownSensorIds.size > 0 && !knownSensorIds.has(sensorId)) return;
  const sid = domId(sensorId);
  if (document.getElementById('card-' + sid)) return;

  const ph = document.getElementById('placeholder-card');
  if (ph) ph.remove();

  const col = palette[Object.keys(charts).length % palette.length];
  sensorColor[sensorId]  = col;
  if (metric) sensorMetric[sensorId] = metric;
  const card = document.createElement('div');
  card.id        = 'card-' + sid;
  card.className = 'card';
  card.dataset.sensorId = sensorId;
  card.innerHTML = `
    <div class="sensor-head">
      <span class="sensor-name"></span>
      <span class="metric-tag"></span>
      <span class="status ok" id="status-${sid}">--</span>
      <button class="reset-zoom-btn" id="reset-zoom-${sid}" type="button">&#8635; Reset</button>
    </div>
    <div class="sensor-row">
      <span class="sensor-value" id="val-${sid}">--</span>
      <span class="trend" id="trend-${sid}"></span>
    </div>
    <div class="stats" id="stats-${sid}">
      <span>min <b>--</b></span><span>avg <b>--</b></span><span>max <b>--</b></span>
    </div>
    <div class="chart-wrap"><canvas id="chart-${sid}"></canvas></div>
    <div class="chart-hint">Scroll to zoom &middot; Drag chart to pan &middot; Drag header to move &middot; Drag corner to resize</div>
  `;
  card.querySelector('.sensor-name').textContent = sensorId;
  card.querySelector('.metric-tag').textContent  = metric;
  card.querySelector('.reset-zoom-btn').addEventListener('click', () => resetZoom(sensorId));
  const grid = document.getElementById('sensors-grid');
  const existingBefore = grid.querySelectorAll('.card[data-sensor-id]').length;
  grid.appendChild(card);
  placeCard(card, sensorId, existingBefore);
  makeDraggable(card);
  cardResizeObserver.observe(card);
  updateGridHeight();
  document.getElementById('sensor-count').textContent = Object.keys(charts).length + 1;

  charts[sensorId] = new Chart(
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
      }, {
        // Band upper (max) — fills down to the lower (min) dataset. Empty
        // unless an aggregated long-window/banded view is active.
        data: [],
        borderColor: 'transparent',
        backgroundColor: col.bg,
        borderWidth: 0,
        pointRadius: 0,
        tension: 0.3,
        fill: '+1',
        order: 1,
      }, {
        // Band lower (min).
        data: [],
        borderColor: 'transparent',
        backgroundColor: 'transparent',
        borderWidth: 0,
        pointRadius: 0,
        tension: 0.3,
        fill: false,
        order: 1,
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { intersect: false, mode: 'index' },
      plugins: {
        legend: { display: false },
        tooltip: {
          filter: item => item.datasetIndex === 0,  // hide band datasets
          callbacks: {
            label: ctx => {
              const v    = ctx.parsed.y;
              const ds   = ctx.chart.data.datasets;
              const bMax = ds[1] && ds[1].data[ctx.dataIndex];
              const bMin = ds[2] && ds[2].data[ctx.dataIndex];
              if (typeof bMin === 'number' && typeof bMax === 'number')
                return `avg ${v.toFixed(2)} (min ${bMin.toFixed(2)} / max ${bMax.toFixed(2)})`;
              return v.toFixed(2);
            }
          }
        },
        zoom: {
          zoom: {
            wheel: { enabled: true }, pinch: { enabled: true }, mode: 'x',
            onZoomComplete: () => {
              const btn = document.getElementById('reset-zoom-' + domId(sensorId));
              if (btn) btn.style.display = 'inline-block';
            }
          },
          pan: { enabled: true, mode: 'x' }
        },
        annotation: { annotations: {} }
      },
      scales: {
        x: { ticks: { color:cssVar('--muted-2'), maxTicksLimit:5, maxRotation:0, font:{size:10} }, grid: { color:cssVar('--grid') } },
        y: { ticks: { color:cssVar('--muted-2'), callback: v => v.toFixed(1), font:{size:10} }, grid: { color:cssVar('--grid') } }
      }
    }
  });

  updateAnnotations(sensorId);
  requestHydration(sensorId);
  // If a persisted historical window is active, also fetch the windowed view.
  if (currentWindow !== 'live') requestSingleWindowHydration(sensorId, currentWindow);
}
