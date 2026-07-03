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

// ============================================================
// Reaction Test API Functions (Browser-based logic)
// ============================================================

let reactionTestState = {
  active: false,
  targetNodeId: -1,
  hitsCollected: 0,
  reactionTimes: [],
  lastUpTimeMs: 0,
};

async function findRandomDownFlap() {
  try {
    const res = await fetch('/api/status');
    if (!res.ok) throw new Error('status fetch failed');
    
    const status = await res.json();
    const nodes = status.nodes || [];
    
    // Filter nodes that are online and flap is down (statusFlags bit 0 = 0 means down)
    const downFlaps = nodes.filter((n) => {
      const isOnline = Number(n.online) === 1;
      const isFlapDown = (Number(n.flags) & 0x01) === 0;
      return isOnline && isFlapDown;
    });
    
    if (downFlaps.length === 0) return null;
    
    // Pick random flap from down flaps
    return downFlaps[Math.floor(Math.random() * downFlaps.length)];
  } catch (err) {
    console.error('findRandomDownFlap error:', err);
    return null;
  }
}

async function checkFlapStatus(nodeId) {
  try {
    const res = await fetch('/api/status');
    if (!res.ok) throw new Error('status fetch failed');
    
    const status = await res.json();
    const nodes = status.nodes || [];
    const flap = nodes.find((n) => n.id === nodeId);
    
    return flap || null;
  } catch (err) {
    console.error('checkFlapStatus error:', err);
    return null;
  }
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
