function text(id, value) {
  const el = document.getElementById(id);
  if (el) {
    el.textContent = value;
  }
}

function applyStatus(data) {
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

setInterval(updateStatus, 5000);
