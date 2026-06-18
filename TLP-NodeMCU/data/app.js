const points = 180;
const rawData = [];
const envData = [];
const thrData = [];

const chart = document.getElementById('chart');
const ctx = chart.getContext('2d');

const kpiFlap = document.getElementById('kpiFlap');
const kpiMode = document.getElementById('kpiMode');
const kpiEnvelope = document.getElementById('kpiEnvelope');
const kpiThreshold = document.getElementById('kpiThreshold');
const kpiRaw = document.getElementById('kpiRaw');
const kpiLatch = document.getElementById('kpiLatch');
const thresholdRange = document.getElementById('thresholdRange');
const thresholdNumber = document.getElementById('thresholdNumber');
const sseDot = document.getElementById('sseDot');
const sseState = document.getElementById('sseState');

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

function pushValue(arr, value) {
  arr.push(value);
  if (arr.length > points) arr.shift();
}

function updateKpis(s) {
  kpiFlap.textContent = s.flap ? 'UP' : 'DOWN';
  kpiFlap.className = 'value ' + (s.flap ? 'state-up' : 'state-down');

  kpiMode.textContent = s.mode.toUpperCase();
  kpiMode.className = 'value ' + (s.mode === 'manual' ? 'state-manual' : 'state-auto');

  kpiEnvelope.textContent = s.envelope;
  kpiThreshold.textContent = s.threshold;
  kpiRaw.textContent = s.raw;
  kpiLatch.textContent = s.latched ? '1' : '0';

  thresholdRange.value = s.threshold;
  thresholdNumber.value = s.threshold;
}

function drawChart() {
  const w = chart.clientWidth;
  const h = chart.clientHeight;
  if (chart.width !== w || chart.height !== h) {
    chart.width = w;
    chart.height = h;
  }

  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = '#101a2b';
  ctx.fillRect(0, 0, w, h);

  const maxY = 1023;
  const minY = 0;
  const toY = (v) => h - ((clamp(v, minY, maxY) - minY) / (maxY - minY)) * h;

  ctx.strokeStyle = 'rgba(255,255,255,0.10)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const gy = (h / 4) * i;
    ctx.beginPath();
    ctx.moveTo(0, gy);
    ctx.lineTo(w, gy);
    ctx.stroke();
  }

  function line(data, color, width = 2) {
    if (data.length < 2) return;
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.beginPath();
    for (let i = 0; i < data.length; i++) {
      const x = (i / (points - 1)) * w;
      const y = toY(data[i]);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
  }

  line(rawData, '#4dd6ff', 1.6);
  line(envData, '#8cff8a', 2);
  line(thrData, '#ffcb73', 1.6);
}

function consumeStatus(s) {
  pushValue(rawData, Number(s.raw) || 0);
  pushValue(envData, Number(s.envelope) || 0);
  pushValue(thrData, Number(s.threshold) || 0);
  updateKpis(s);
  drawChart();
}

async function refreshStatus() {
  const resp = await fetch('/status');
  if (!resp.ok) throw new Error('status fetch failed');
  const s = await resp.json();
  consumeStatus(s);
}

async function sendAction(action) {
  const resp = await fetch('/api/flap?action=' + encodeURIComponent(action), { method: 'POST' });
  if (!resp.ok) throw new Error('flap command failed');
  const s = await resp.json();
  consumeStatus(s);
}

async function sendThreshold(value) {
  const v = clamp(Number(value) || 0, 0, 1023);
  const resp = await fetch('/api/threshold?value=' + encodeURIComponent(v), { method: 'POST' });
  if (!resp.ok) throw new Error('threshold command failed');
  const s = await resp.json();
  consumeStatus(s);
}

function connectEvents() {
  const es = new EventSource('/events');
  es.onopen = () => {
    sseDot.className = 'dot ok';
    sseState.textContent = 'SSE connected';
  };

  es.onerror = () => {
    sseDot.className = 'dot bad';
    sseState.textContent = 'SSE reconnecting...';
  };

  es.addEventListener('state', (evt) => {
    try {
      consumeStatus(JSON.parse(evt.data));
    } catch (_) {
    }
  });
}

thresholdRange.addEventListener('input', () => {
  thresholdNumber.value = thresholdRange.value;
});

thresholdNumber.addEventListener('input', () => {
  thresholdRange.value = clamp(Number(thresholdNumber.value) || 0, 0, 1023);
});

document.querySelectorAll('[data-action]').forEach((btn) => {
  btn.addEventListener('click', async () => {
    try {
      await sendAction(btn.dataset.action);
    } catch (err) {
      console.error(err);
    }
  });
});

document.getElementById('setThresholdBtn').addEventListener('click', async () => {
  try {
    await sendThreshold(thresholdNumber.value);
  } catch (err) {
    console.error(err);
  }
});

(async () => {
  try {
    await refreshStatus();
  } catch (err) {
    console.error(err);
  }
  connectEvents();
  drawChart();
})();
