// ============================================================
// TLP-Klappen API & CAN Communication
// ============================================================

async function waitForCommandResult(requestId, timeoutMs = 1500) {
  const start = Date.now();

  while ((Date.now() - start) < timeoutMs) {
    const res = await fetch('/api/command-result?id=' + encodeURIComponent(requestId));
    if (!res.ok) {
      throw new Error('command result lookup failed');
    }

    const result = await res.json();
    if (Number(result.done) === 1) {
      return result;
    }

    await new Promise((resolve) => setTimeout(resolve, 80));
  }

  throw new Error('command timeout');
}

async function postFlapAction(flapId, action) {
  const flap = state.flaps.find((f) => f.id === flapId);
  const nodeId = flap ? flap.nodeId : -1;

  setFlapCommandState(flapId, 'pending', 'Sende Befehl...');
  try {
    if (!flap || nodeId < 0) {
      throw new Error('node not found');
    }

    if (!flap.online) {
      throw new Error('node offline');
    }

    const res = await fetch('/api/flap?action=' + encodeURIComponent(action) + '&node=' + encodeURIComponent(nodeId), { method: 'POST' });
    if (!res.ok) throw new Error('flap action send failed');

    const accepted = await res.json();
    if (!accepted || Number(accepted.pending) !== 1 || !accepted.requestId) {
      throw new Error('invalid command accept response');
    }

    const result = await waitForCommandResult(accepted.requestId, 2000);
    if (result.state !== 'ack') {
      throw new Error('flap action rejected: ' + result.state);
    }

    setFlapCommandState(flapId, 'ok', 'Aktion bestaetigt');
    updateStatus();
    text('connectionStatus', 'Aktion fuer Node ' + nodeId + ' ausgefuehrt');
  } catch (err) {
    console.error(err);
    setFlapCommandState(flapId, 'error', 'Aktion fehlgeschlagen');
    text('connectionStatus', 'Aktion fehlgeschlagen');
  }
}

async function postThreshold(flapId, value) {
  const flap = state.flaps.find((f) => f.id === flapId);
  const nodeId = flap ? flap.nodeId : -1;

  setFlapCommandState(flapId, 'pending', 'Setze Schwellwert...');
  try {
    if (!flap || nodeId < 0) {
      throw new Error('node not found');
    }

    if (!flap.online) {
      throw new Error('node offline');
    }

    const v = Math.max(0, Math.min(1023, Number(value) || 0));
    const res = await fetch('/api/threshold?value=' + encodeURIComponent(v) + '&node=' + encodeURIComponent(nodeId), { method: 'POST' });
    if (!res.ok) throw new Error('threshold update send failed');

    const accepted = await res.json();
    if (!accepted || Number(accepted.pending) !== 1 || !accepted.requestId) {
      throw new Error('invalid command accept response');
    }

    const result = await waitForCommandResult(accepted.requestId, 2000);
    if (result.state !== 'ack') {
      throw new Error('threshold rejected: ' + result.state);
    }

    setFlapCommandState(flapId, 'ok', 'Schwellwert bestaetigt');
    updateStatus();
    text('connectionStatus', 'Schwellwert fuer Node ' + nodeId + ' gesetzt');
  } catch (err) {
    console.error(err);
    setFlapCommandState(flapId, 'error', 'Schwellwert fehlgeschlagen');
    text('connectionStatus', 'Schwellwert fehlgeschlagen');
  }
}

function updateStatus() {
  fetch('/api/status')
    .then((r) => {
      if (!r.ok) throw new Error(r.status);
      return r.json();
    })
    .then((data) => {
      state.connected = true;
      applyStatus(data);
      text('connectionStatus', 'Status OK');
    })
    .catch(() => {
      state.connected = false;
      text('connectionStatus', 'Verbindung verloren');
    });
}

let sseBackoffMs = 1000;
let sseSource = null;

function connectSSE() {
  if (sseSource) {
    sseSource.close();
    sseSource = null;
  }

  const es = new EventSource('/events');
  sseSource = es;

  es.addEventListener('status', (e) => {
    try {
      sseBackoffMs = 1000;
      state.connected = true;
      applyStatus(JSON.parse(e.data));
      text('connectionStatus', 'Live');
    } catch (_) {}
  });

  es.onerror = () => {
    state.connected = false;
    text('connectionStatus', 'SSE offline - Retry in ' + (sseBackoffMs / 1000).toFixed(0) + 's');
    es.close();
    sseSource = null;

    const delay = sseBackoffMs;
    sseBackoffMs = Math.min(sseBackoffMs * 2, 30000);
    setTimeout(connectSSE, delay);
  };
}

// Auto-start
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', () => {
    updateStatus();
    connectSSE();
    setInterval(() => {
      if (!state.connected) {
        updateStatus();
      }
    }, 5000);
  });
} else {
  updateStatus();
  connectSSE();
  setInterval(() => {
    if (!state.connected) {
      updateStatus();
    }
  }, 5000);
}
