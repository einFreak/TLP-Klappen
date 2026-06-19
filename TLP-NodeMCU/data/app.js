function updateStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => {
      document.getElementById('apSsid').textContent = data.ssid;
      document.getElementById('apIp').textContent = data.ip;
      document.getElementById('apChannel').textContent = data.channel;
      document.getElementById('apClients').textContent = data.clients;
      document.getElementById('apMac').textContent = data.mac;
      document.getElementById('connectionStatus').textContent = 'Status OK';
    })
    .catch(e => {
      console.error('Status fetch failed:', e);
      document.getElementById('connectionStatus').textContent = 'Connection lost';
    });
}

// Initial update
updateStatus();

// Connect to SSE stream for live updates
const eventSource = new EventSource('/events');
eventSource.addEventListener('status', (e) => {
  try {
    const data = JSON.parse(e.data);
    document.getElementById('apClients').textContent = data.clients;
    document.getElementById('connectionStatus').textContent = 'Live sync active';
  } catch (_) {}
});

eventSource.onerror = () => {
  document.getElementById('connectionStatus').textContent = 'SSE disconnected';
};

// Poll every 5 seconds as fallback
setInterval(updateStatus, 5000);
