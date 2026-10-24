// ============================================================
// TLP-Klappen API & CAN Communication
// ============================================================

let localCommandQueue = Promise.resolve();

function runControlCommand(task) {
  if (!window.TLPTransport.local) return task();
  const session = window.TLPTransport.session;
  const result = localCommandQueue.catch(() => {}).then(() => {
    if (window.TLPTransport.session !== session) throw new Error('USB-Sitzung beendet');
    return task();
  });
  localCommandQueue = result;
  return result;
}

async function waitForCommandResult(requestId, timeoutMs = 1500) {
  const start = Date.now();

  while ((Date.now() - start) < timeoutMs) {
    const result = await window.TLPTransport.result(Number(requestId));
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

    if (!state.connected || !flap.online) {
      throw new Error('node offline');
    }

    const result = await runControlCommand(async () => {
      if (!state.connected || !flap.online) throw new Error('node offline');
      const accepted = await window.TLPTransport.flap(nodeId, action);
      if (!accepted || Number(accepted.pending) !== 1 || !accepted.requestId) {
        throw new Error('invalid command accept response');
      }
      const confirmed = await waitForCommandResult(accepted.requestId, 2000);
      if (confirmed.state !== 'ack') throw new Error('flap action rejected: ' + confirmed.state);
      return confirmed;
    });

    setFlapCommandState(flapId, 'ok', 'Aktion bestaetigt');
    updateStatus();
    text('connectionStatus', 'Aktion fuer Node ' + nodeId + ' ausgefuehrt');
    return result;
  } catch (err) {
    console.error(err);
    setFlapCommandState(flapId, 'error', 'Aktion fehlgeschlagen');
    text('connectionStatus', 'Aktion fehlgeschlagen');
    throw err;
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

    if (!state.connected || !flap.online) {
      throw new Error('node offline');
    }

    const threshold = Number(value);
    if (!Number.isInteger(threshold) || threshold < 0 || threshold > 1023) throw new Error('invalid threshold');
    const result = await runControlCommand(async () => {
      if (!state.connected || !flap.online) throw new Error('node offline');
      const accepted = await window.TLPTransport.threshold(nodeId, threshold);
      if (!accepted || Number(accepted.pending) !== 1 || !accepted.requestId) {
        throw new Error('invalid command accept response');
      }
      const confirmed = await waitForCommandResult(accepted.requestId, 2000);
      if (confirmed.state !== 'ack') throw new Error('threshold rejected: ' + confirmed.state);
      return confirmed;
    });

    setFlapCommandState(flapId, 'ok', 'Schwellwert bestaetigt');
    updateStatus();
    text('connectionStatus', 'Schwellwert fuer Node ' + nodeId + ' gesetzt');
    return result;
  } catch (err) {
    console.error(err);
    setFlapCommandState(flapId, 'error', 'Schwellwert fehlgeschlagen');
    text('connectionStatus', 'Schwellwert fehlgeschlagen');
    throw err;
  }
}

function updateStatus() {
  return window.TLPTransport.getStatus()
    .then((data) => {
      state.connected = true;
      applyStatus(data);
      text('connectionStatus', window.TLPTransport.local ? 'USB live; WLAN aus' : 'Status OK');
    })
    .catch(() => {
      setConnectionState(false, 'Verbindung verloren');
    });
}

function setConnectionState(connected, message) {
  state.connected = connected;
  if (!connected) state.flaps.forEach((flap) => { flap.online = false; });
  text('connectionStatus', message);
  text('serialConnectionState', message);
  render();
  renderDetailPanel();
}

function startConnection() {
  const transport = window.TLPTransport;
  transport.subscribe((data) => {
    state.connected = true;
    applyStatus(data);
  }, setConnectionState);
  if (!transport.local) {
    updateStatus();
    setInterval(() => { if (!state.connected) updateStatus(); }, 5000);
    return;
  }
  const controls = document.getElementById('serialControls');
  const connect = document.getElementById('serialConnectBtn');
  const disconnect = document.getElementById('serialDisconnectBtn');
  controls.hidden = false;
  document.body.classList.add('local-mode');
  controls.closest('.network-panel').classList.add('network-panel--local');
  text('networkHeading', 'USB / Serial');
  let busy = false;
  function updateButtons() {
    connect.disabled = busy || Boolean(transport.port);
    disconnect.disabled = busy || !transport.port;
  }
  transport.subscribe(() => {}, () => { queueMicrotask(updateButtons); });
  connect.addEventListener('click', async () => {
    busy = true;
    updateButtons();
    try { await transport.connect(); }
    catch (error) { setConnectionState(false, error.message); }
    finally { busy = false; updateButtons(); }
  });
  disconnect.addEventListener('click', async () => {
    busy = true;
    updateButtons();
    try { await transport.disconnect(); }
    catch (error) { setConnectionState(false, error.message); }
    finally { busy = false; updateButtons(); }
  });
  updateButtons();
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', startConnection);
else startConnection();
