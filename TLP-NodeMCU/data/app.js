function text(id, value) {
  const el = document.getElementById(id);
  if (el) {
    el.textContent = value;
  }
}

let lastStatus = null;

function setControlsDisabled(disabled) {
  document.querySelectorAll('[data-action]').forEach((btn) => {
    btn.disabled = disabled;
  });
  const applyBtn = document.getElementById('applyThresholdBtn');
  if (applyBtn) {
    applyBtn.disabled = disabled;
  }
}

async function postFlapAction(action) {
  setControlsDisabled(true);
  try {
    const res = await fetch('/api/flap?action=' + encodeURIComponent(action), { method: 'POST' });
    if (!res.ok) {
      throw new Error('flap action failed');
    }
    applyStatus(await res.json());
  } finally {
    setControlsDisabled(false);
  }
}

async function postThreshold(value) {
  setControlsDisabled(true);
  try {
    const v = Math.max(0, Math.min(1023, Number(value) || 0));
    const res = await fetch('/api/threshold?value=' + encodeURIComponent(v), { method: 'POST' });
    if (!res.ok) {
      throw new Error('threshold update failed');
    }
    applyStatus(await res.json());
  } finally {
    setControlsDisabled(false);
  }
}

function setDetailOpen(isOpen) {
  const panel = document.getElementById('flapDetailPanel');
  if (!panel) {
    return;
  }
  panel.classList.toggle('open', isOpen);
  panel.setAttribute('aria-hidden', isOpen ? 'false' : 'true');
}

function applyStatus(data) {
  lastStatus = data;

  text('apSsid', data.ssid ?? '-');
  text('apIp', data.ip ?? '-');
  text('apChannel', data.channel ?? '-');
  text('apClients', data.clients ?? '-');
  text('apMac', data.mac ?? '-');

  const isUp = Number(data.flap) === 1;
  const flapVisual = document.getElementById('flapVisual');
  if (flapVisual) {
    flapVisual.classList.remove('up', 'down');
    flapVisual.classList.add(isUp ? 'up' : 'down');
  }

  text('flapState', isUp ? 'UP' : 'DOWN');
  text('flapMode', (data.mode || '-').toUpperCase());
  text('flapLatch', Number(data.latched) === 1 ? 'LOCKED' : 'FREE');
  text('flapThreshold', data.threshold ?? '-');

  const triggerRaw = Number(data.lastTriggerRaw);
  text('lastTriggerRaw', triggerRaw >= 0 ? triggerRaw : '-');

  text('detailState', isUp ? 'UP' : 'DOWN');
  text('detailMode', (data.mode || '-').toUpperCase());
  text('detailTriggerRaw', triggerRaw >= 0 ? triggerRaw : '-');
  const triggerEnv = Number(data.lastTriggerEnvelope);
  text('detailTriggerEnvelope', triggerEnv >= 0 ? triggerEnv : '-');
  const triggerAt = Number(data.lastTriggerAt);
  text('detailTriggerAt', triggerAt > 0 ? triggerAt : '-');

  const thresholdValue = Number(data.threshold);
  const thresholdRange = document.getElementById('thresholdRange');
  const thresholdNumber = document.getElementById('thresholdNumber');
  if (thresholdRange && Number.isFinite(thresholdValue)) {
    thresholdRange.value = String(thresholdValue);
  }
  if (thresholdNumber && Number.isFinite(thresholdValue)) {
    thresholdNumber.value = String(thresholdValue);
  }
}

function updateStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => {
      applyStatus(data);
      text('connectionStatus', 'Status OK');
    })
    .catch(e => {
      console.error('Status fetch failed:', e);
      text('connectionStatus', 'Connection lost');
    });
}

updateStatus();

const eventSource = new EventSource('/events');
eventSource.addEventListener('status', (e) => {
  try {
    const data = JSON.parse(e.data);
    applyStatus(data);
    text('connectionStatus', 'Live sync active');
  } catch (_) {
  }
});

eventSource.onerror = () => {
  text('connectionStatus', 'SSE reconnecting...');
};

document.querySelectorAll('[data-action]').forEach((btn) => {
  btn.addEventListener('click', async () => {
    try {
      await postFlapAction(btn.dataset.action);
      text('connectionStatus', 'Action sent');
    } catch (err) {
      console.error(err);
      text('connectionStatus', 'Action failed');
    }
  });
});

const thresholdRange = document.getElementById('thresholdRange');
const thresholdNumber = document.getElementById('thresholdNumber');
if (thresholdRange && thresholdNumber) {
  thresholdRange.addEventListener('input', () => {
    thresholdNumber.value = thresholdRange.value;
  });
  thresholdNumber.addEventListener('input', () => {
    const v = Math.max(0, Math.min(1023, Number(thresholdNumber.value) || 0));
    thresholdRange.value = String(v);
  });
}

const applyThresholdBtn = document.getElementById('applyThresholdBtn');
if (applyThresholdBtn) {
  applyThresholdBtn.addEventListener('click', async () => {
    try {
      await postThreshold(thresholdNumber ? thresholdNumber.value : 0);
      text('connectionStatus', 'Threshold updated');
    } catch (err) {
      console.error(err);
      text('connectionStatus', 'Threshold update failed');
    }
  });
}

const flapVisual = document.getElementById('flapVisual');
if (flapVisual) {
  flapVisual.addEventListener('click', () => {
    const panel = document.getElementById('flapDetailPanel');
    const isOpen = panel && panel.classList.contains('open');
    setDetailOpen(!isOpen);
  });
  flapVisual.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      const panel = document.getElementById('flapDetailPanel');
      const isOpen = panel && panel.classList.contains('open');
      setDetailOpen(!isOpen);
    }
  });
}

const closeDetailBtn = document.getElementById('closeDetailBtn');
if (closeDetailBtn) {
  closeDetailBtn.addEventListener('click', () => {
    setDetailOpen(false);
  });
}

setInterval(updateStatus, 5000);
