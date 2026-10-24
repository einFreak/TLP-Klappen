import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { startLocalServer } from '../server.mjs';

async function start(context, port = 0) {
  const server = await startLocalServer(port);
  context.after(() => { server.closeAllConnections(); return new Promise((resolve) => server.close(resolve)); });
  return { server, url: 'http://127.0.0.1:' + server.address().port };
}

test('serves shared UI unchanged, local transport override, assets and HEAD', async (context) => {
  const { url } = await start(context);
  const html = await (await fetch(url)).text();
  assert.equal(html, await readFile(new URL('../../TLP-NodeMCU/data/index.html', import.meta.url), 'utf8'));
  for (const path of new Set(Array.from(html.matchAll(/(?:src|href)="(\/[^\"]+)"/g), (match) => match[1]))) {
    const response = await fetch(url + path);
    assert.equal(response.status, 200, path);
  }
  const response = await fetch(url + '/transport.js');
  assert.match(await response.text(), /class SerialTransport/);
  assert.equal(response.headers.get('permissions-policy'), 'serial=(self)');
  const head = await fetch(url + '/style.css', { method: 'HEAD' });
  assert.equal(head.status, 200);
  assert.equal(await head.text(), '');
});

test('rejects arbitrary files, traversal, API bridge and writes', async (context) => {
  const { url } = await start(context);
  for (const path of ['/server.mjs', '/package.json', '/api/status', '/%2e%2e%2fREADME.md', '/..%5cREADME.md']) {
    assert.equal((await fetch(url + path)).status, 404, path);
  }
  assert.equal((await fetch(url, { method: 'POST' })).status, 405);
});

test('occupied port selects a free port and remains loopback-only', async (context) => {
  const first = await start(context);
  const second = await start(context, first.server.address().port);
  assert.notEqual(second.server.address().port, first.server.address().port);
  assert.equal(second.server.address().address, '127.0.0.1');
  await assert.rejects(startLocalServer(-1), /Invalid PORT/);
});