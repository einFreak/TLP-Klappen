import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';

const uiRoot = new URL('../TLP-NodeMCU/data/', import.meta.url);
const assets = new Map(['index.html', 'api.js', 'app.js', 'games.js', 'style.css', 'flap-up.svg', 'flap-down.svg']
  .map((name) => ['/' + name, new URL(name, uiRoot)]));
assets.set('/', assets.get('/index.html'));
assets.set('/transport.js', new URL('./serial_transport.js', import.meta.url));
const mime = { html: 'text/html', js: 'text/javascript', css: 'text/css', svg: 'image/svg+xml' };

export function createLocalServer() {
  return createServer(async (request, response) => {
    response.setHeader('X-Content-Type-Options', 'nosniff');
    response.setHeader('Permissions-Policy', 'serial=(self)');
    response.setHeader('Cache-Control', 'no-store');
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      response.writeHead(405, { Allow: 'GET, HEAD' });
      response.end();
      return;
    }
    const pathname = (request.url || '/').split('?')[0];
    const asset = assets.get(pathname);
    if (!asset) { response.writeHead(404); response.end('Not found'); return; }
    try {
      const content = await readFile(asset);
      const extension = asset.pathname.split('.').at(-1);
      response.writeHead(200, { 'Content-Type': mime[extension] + '; charset=utf-8', 'Content-Length': content.length });
      response.end(request.method === 'HEAD' ? undefined : content);
    } catch (error) {
      response.writeHead(error.code === 'ENOENT' ? 404 : 500);
      response.end('Asset unavailable');
    }
  });
}

export async function startLocalServer(port = 8080) {
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new Error('Invalid PORT');
  const server = createLocalServer();
  const listen = (candidate) => new Promise((resolveListen, reject) => {
    const onError = (error) => { server.off('listening', onListening); reject(error); };
    const onListening = () => { server.off('error', onError); resolveListen(); };
    server.once('error', onError);
    server.once('listening', onListening);
    server.listen(candidate, '127.0.0.1');
  });
  try { await listen(port); }
  catch (error) {
    if (error.code !== 'EADDRINUSE' || port === 0) throw error;
    await listen(0);
  }
  return server;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const server = await startLocalServer(Number(process.env.PORT || 8080));
    console.log('TLP-Local: http://localhost:' + server.address().port);
    console.log('Edge/Chrome: USB verbinden. Serial-Monitor vorher schliessen.');
    process.once('SIGINT', () => { server.closeAllConnections(); server.close(); });
  } catch (error) {
    console.error('TLP-Local konnte nicht starten: ' + error.message);
    process.exitCode = 1;
  }
}