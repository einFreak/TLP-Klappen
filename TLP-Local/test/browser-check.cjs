const assert = require('node:assert/strict');
const { readFile } = require('node:fs/promises');
const { delimiter, resolve, join } = require('node:path');
const { tmpdir } = require('node:os');
const { chromium } = require(require.resolve('playwright', {
  paths: [process.cwd(), ...process.env.PATH.split(delimiter).map((entry) => resolve(entry, '..'))],
}));

async function main() {
  const { startLocalServer } = await import('../server.mjs');
  const server = await startLocalServer(0);
  const browser = await chromium.launch();
  const url = 'http://localhost:' + server.address().port;
  const errors = [];
  try {
    const context = await browser.newContext({ viewport: { width: 1280, height: 900 } });
    await context.addInitScript(() => {
      const fake = { commands: [], session: 100, hits: false, sequence: 7, nextCanId: 1, results: new Map(), outstanding: 0, maxOutstanding: 0 };
      window.__tlpFake = fake;
      const nodes = Array.from({ length: 12 }, (_, id) => ({ id, online: 1, flags: 4, hb: 1,
        lastUpEventCounter: 1, lastUpEventSequence: 7, lastUpTimeMs: 0 }));
      const port = {
        async open() {},
        async close() {},
        readable: new ReadableStream({ start(controller) { fake.controller = controller; } }),
        writable: new WritableStream({
          write(chunk) {
            const [prefix, rawId, operation, session, ...args] = new TextDecoder().decode(chunk).trim().split(' ');
            fake.commands.push({ operation, id: Number(rawId), session: Number(session), args });
            const id = Number(rawId);
            const frame = (frameId, type, data) => fake.controller.enqueue(new TextEncoder().encode('TLP1 ' + JSON.stringify({ id: frameId, session: fake.session, type, data }) + '\n'));
            if (operation === 'HELLO') frame(id, 'hello', { device: 'TLP-NodeMCU', version: 1, mode: 'wifi' });
            else if (operation === 'ENTER_LOCAL') {
              frame(id, 'mode', { mode: 'local' });
              frame(0, 'snapshot_begin', { generation: 1 });
              frame(0, 'central', { mode: 'local', canNormal: 1, clients: 0, channel: 0 });
              for (const node of nodes) frame(0, 'node', node);
              frame(0, 'snapshot_end', { generation: 1 });
            } else if (operation === 'PING') frame(id, 'pong', { ok: 1 });
            else if (operation === 'EXIT_LOCAL') frame(id, 'mode', { mode: 'wifi' });
            else if (operation === 'FLAP' || operation === 'THRESHOLD') {
              const canId = fake.nextCanId++;
              fake.outstanding++;
              fake.maxOutstanding = Math.max(fake.maxOutstanding, fake.outstanding);
              fake.results.set(canId, { requestId: canId, done: 1, state: 'ack' });
              frame(id, 'accepted', { pending: 1, requestId: canId });
              if (operation === 'FLAP') {
                const node = nodes[Number(args[0])];
                node.flags = args[1] === 'down' ? 4 : 3;
                frame(0, 'node', node);
                if (fake.hits && args[1] === 'reset') setTimeout(() => {
                  node.flags = 6;
                  node.lastUpEventSequence = ++fake.sequence;
                  node.lastUpEventCounter = 1;
                  node.lastUpTimeMs = 210 + node.id;
                  frame(0, 'node', node);
                }, 150);
              }
            } else if (operation === 'RESULT') {
              fake.outstanding--;
              frame(id, 'result', fake.results.get(Number(args[0])));
            }
          },
        }),
      };
      Object.defineProperty(navigator, 'serial', { value: { requestPort: async () => port }, configurable: true });
    });
    const page = await context.newPage();
    page.on('pageerror', (error) => errors.push(error.message));
    await page.goto(url);
    await page.waitForFunction(() => !document.getElementById('serialControls').hidden);
    assert.equal(await page.locator('#allFlapsDownBtn').isDisabled(), true);
    await page.locator('#serialConnectBtn').click();
    await page.waitForFunction(() => window.TLPTransport.ready);
    assert.equal(await page.locator('.flap-card').count(), 12);
    assert.equal(await page.locator('#allFlapsDownBtn').isEnabled(), true);
    await page.locator('#allFlapsDownBtn').click();
    await page.waitForFunction(() => document.getElementById('connectionStatus').textContent.includes('Sammelaktion abgeschlossen'));
    assert.equal(await page.evaluate(() => window.__tlpFake.maxOutstanding), 1);
    await page.evaluate(async () => { await postThreshold('node_3', 321); window.__tlpFake.hits = true; });
    await page.locator('#reactionTestBtn').click();
    await page.locator('#reactionTestStartBtn').click();
    await page.waitForFunction(() => document.getElementById('reactionTestStats').textContent.includes('Treffer gesamt: 5'), null, { timeout: 15000 });
    await page.locator('#reactionTestCloseBtn2').click();
    await page.screenshot({ path: join(tmpdir(), 'tlp-local-desktop.png'), fullPage: true });
    await page.setViewportSize({ width: 390, height: 844 });
    await page.locator('#serialControls').scrollIntoViewIfNeeded();
    const fits = await page.locator('#serialControls').evaluate((element) => {
      const parent = element.getBoundingClientRect();
      return Array.from(element.children).every((child) => {
        const rect = child.getBoundingClientRect();
        return rect.left >= parent.left - 1 && rect.right <= parent.right + 1;
      });
    });
    assert.ok(fits, 'USB controls fit mobile width');
    await page.screenshot({ path: join(tmpdir(), 'tlp-local-mobile.png'), fullPage: true });
    await page.locator('#serialDisconnectBtn').click();
    await page.waitForFunction(() => !window.TLPTransport.port);
    assert.equal(await page.locator('#allFlapsDownBtn').isDisabled(), true);
    assert.equal(await page.locator('#serialConnectBtn').isEnabled(), true);
    assert.equal(await page.evaluate(() => window.__tlpFake.commands.at(-1).operation), 'EXIT_LOCAL');
    await context.close();

    const wifiContext = await browser.newContext();
    const status = { ssid: 'TLP-Test', nodes: [{ id: 3, online: 1, flags: 4, lastUpTimeMs: 123, lastUpEventCounter: 1 }] };
    await wifiContext.addInitScript((initialStatus) => {
      window.EventSource = class {
        addEventListener(name, callback) { if (name === 'status') queueMicrotask(() => callback({ data: JSON.stringify(initialStatus) })); }
        close() {}
      };
    }, status);
    const wifi = await wifiContext.newPage();
    wifi.on('pageerror', (error) => errors.push(error.message));
    await wifi.route('**/transport.js', async (route) => route.fulfill({ contentType: 'text/javascript', body: await readFile(resolve(__dirname, '../../TLP-NodeMCU/data/transport.js'), 'utf8') }));
    await wifi.route('**/api/status', (route) => route.fulfill({ json: status }));
    await wifi.route('**/api/flap?*', (route) => route.fulfill({ status: 202, json: { pending: 1, requestId: 1 } }));
    await wifi.route('**/api/command-result?*', (route) => route.fulfill({ json: { done: 1, state: 'ack' } }));
    await wifi.goto(url);
    await wifi.waitForFunction(() => state.connected && state.flaps.length === 1);
    assert.equal(await wifi.locator('#serialControls').isHidden(), true);
    await wifi.evaluate(() => postFlapAction('node_3', 'up'));
    assert.equal(await wifi.evaluate(() => window.TLPTransport.local), false);
    await wifiContext.close();
    assert.deepEqual(errors, []);
    console.log('Browser OK: local + WLAN, 12-node bulk, threshold, five reaction rounds, disconnect, desktop/mobile.');
    console.log('Screenshots: ' + join(tmpdir(), 'tlp-local-desktop.png') + ', ' + join(tmpdir(), 'tlp-local-mobile.png'));
  } finally {
    await browser.close();
    server.closeAllConnections();
    await new Promise((done) => server.close(done));
  }
}

main().catch((error) => { console.error(error); process.exitCode = 1; });