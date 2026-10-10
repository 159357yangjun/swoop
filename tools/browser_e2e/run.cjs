'use strict';
// Real Chrome/Edge runtime exercise of the published MV3 extension.
// Isolated user-data-dir; native host is deliberately NOT registered.
// This is not a GUI/desktop-engine E2E.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const crypto = require('node:crypto');
const { chromium } = require('playwright-core');

const EXT = path.resolve(__dirname, '..', '..', 'browser-extension');
const ID = 'fmnehlhldcoknecihnlbbcmhpaaapmbp';
const payload = Buffer.alloc(262144);
for (let i = 0; i < payload.length; i++) payload[i] = (i * 31 + 7) & 255;
const sourceSha = crypto.createHash('sha256').update(payload).digest('hex');

function browsers() {
  const roots = [process.env['PROGRAMFILES(X86)'], process.env.ProgramFiles,
                 process.env.LOCALAPPDATA].filter(Boolean);
  const pick = (rel) => roots.map(root => path.join(root, rel)).find(fs.existsSync);
  return [
    ['Edge', pick('Microsoft/Edge/Application/msedge.exe')],
    ['Chrome', pick('Google/Chrome/Application/chrome.exe')]
  ];
}
async function httpServer() {
  const server = http.createServer((req, res) => {
    if (req.url === '/file.zip') {
      res.writeHead(200, { 'Content-Type': 'application/zip',
                           'Content-Disposition': 'attachment; filename="file.zip"',
                           'Content-Length': payload.length,
                           'Cache-Control': 'no-store' });
      res.end(payload);
    } else if (req.url === '/index.html') {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end('<!doctype html><html><body><a id="go" href="/file.zip">Download</a></body></html>');
    } else { res.writeHead(404); res.end('missing'); }
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  return { server, port: server.address().port };
}
async function runOne(exe, label, mode, baseUrl) {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'idm-next-intercept-e2e-'));
  let context;
  try {
    context = await chromium.launchPersistentContext(profile, {
      executablePath: exe, headless: true, acceptDownloads: true,
      args: ['--headless=new', '--disable-extensions-except=' + EXT,
             '--load-extension=' + EXT, '--no-first-run',
             '--no-default-browser-check', '--disable-gpu']
    });
    const popup = await context.newPage();
    await popup.goto('chrome-extension://' + ID + '/popup.html', { waitUntil: 'domcontentloaded' });
    const extensionId = await popup.evaluate(() => chrome.runtime.id);
    assert.equal(extensionId, ID, 'wrong extension loaded');
    await popup.evaluate(async (m) => {
      await chrome.storage.local.set({ interceptEnabled: true,
                                      confirmEnabled: m === 'dismiss' });
    }, mode);

    const worker = context.serviceWorkers().find(w => w.url().startsWith('chrome-extension://' + ID + '/'))
      || await context.waitForEvent('serviceworker', {
        predicate: w => w.url().startsWith('chrome-extension://' + ID + '/'), timeout: 12000
      });
    // Instrument only the native API boundary. For "offline" we call the REAL native API,
    // which must return a missing-host error on a clean CI runner. The delayed-accept mode
    // simulates the host reply to exercise the browser-finishes-first race, not host IPC.
    await worker.evaluate((m) => {
      globalThis.__handoffE2E = { calls: [], errors: [] };
      const original = chrome.runtime.sendNativeMessage.bind(chrome.runtime);
      chrome.runtime.sendNativeMessage = function(host, request, callback) {
        globalThis.__handoffE2E.calls.push(request.action);
        if (m === 'browser-first' && request.action === 'add_download') {
          setTimeout(() => callback({ success: true, message: 'stub accepted' }), 1800);
          return;
        }
        return original(host, request, (reply) => {
          if (chrome.runtime.lastError)
            globalThis.__handoffE2E.errors.push(chrome.runtime.lastError.message);
          callback(reply);
        });
      };
    }, mode);

    const page = await context.newPage();
    await page.goto(baseUrl + '/index.html', { waitUntil: 'domcontentloaded' });
    const pending = page.waitForEvent('download', { timeout: 15000 });
    await page.locator('#go').click();
    const download = await pending;

    if (mode === 'dismiss') {
      // This is the ACTUAL content-script UI, not a mocked onMessage callback.
      await page.locator('#idm-next-capture').waitFor({ timeout: 12000 });
      await page.locator('#idm-next-capture .idm-cap-cancel').click();
      await page.locator('#idm-next-capture').waitFor({ state: 'hidden', timeout: 12000 });
    }
    const failure = await download.failure();
    assert.equal(failure, null, 'browser default download must finish: ' + failure);
    const saved = path.join(profile, 'verified.zip');
    await download.saveAs(saved);
    const actual = fs.readFileSync(saved);
    const actualSha = crypto.createHash('sha256').update(actual).digest('hex');
    assert.equal(actual.length, payload.length);
    assert.equal(actualSha, sourceSha);

    if (mode === 'browser-first')
      await new Promise(resolve => setTimeout(resolve, 2300));
    else
      await new Promise(resolve => setTimeout(resolve, 350));
    const observed = await worker.evaluate(() => globalThis.__handoffE2E);
    assert.ok(observed, 'instrumentation missing (worker restarted?)');
    if (mode === 'dismiss') {
      assert.equal(observed.calls.filter(x => x === 'add_download').length, 0,
                   'dismiss should not call the native download API');
    } else {
      assert.ok(observed.calls.includes('add_download'),
                'real downloads.onCreated handler did not call native API');
      if (mode === 'offline')
        assert.ok(observed.errors.length > 0, 'native host was not actually unavailable');
    }
    const live = await popup.evaluate(async () => chrome.downloads.search({}));
    const record = live.find(x => x.url === baseUrl + '/file.zip');
    assert.ok(record && record.state === 'complete',
              'original browser download record missing/canceled/erased');
    console.log('[PASS] ' + label + ' ' + mode +
                ' native_calls=' + observed.calls.join(',') +
                ' bytes=' + actual.length + ' sha256=' + actualSha.slice(0, 16) +
                ' state=' + record.state);
  } finally {
    if (context) await context.close();
    fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 400 });
  }
}
(async () => {
  const found = browsers();
  for (const [name, exe] of found) {
    if (!exe) throw new Error(name + ' not found on Windows runner; this test does not skip browser brands');
  }
  const { server, port } = await httpServer();
  console.log('SERVER_READY port=' + port + ' size=' + payload.length + ' sha256=' + sourceSha);
  try {
    for (const [label, exe] of found)
      for (const mode of ['offline', 'dismiss', 'browser-first'])
        await runOne(exe, label, mode, 'http://127.0.0.1:' + port);
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error('[FAIL] browser takeover e2e:', error); process.exitCode = 1; });
