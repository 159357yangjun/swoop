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
const { Browser, detectBrowserPlatform, resolveBuildId, install } = require('@puppeteer/browsers');

const EXT = path.resolve(__dirname, '..', '..', 'browser-extension');
const ID = 'fmnehlhldcoknecihnlbbcmhpaaapmbp';
const payload = Buffer.alloc(262144);
for (let i = 0; i < payload.length; i++) payload[i] = (i * 31 + 7) & 255;
const sourceSha = crypto.createHash('sha256').update(payload).digest('hex');

async function browsers() {
  const roots = [process.env['PROGRAMFILES(X86)'], process.env.ProgramFiles,
                 process.env.LOCALAPPDATA].filter(Boolean);
  const pick = (rel) => roots.map(root => path.join(root, rel)).find(fs.existsSync);
  const edge = pick('Microsoft/Edge/Application/msedge.exe');
  assert.ok(edge, 'Edge browser missing from Windows runner');
  const platform = detectBrowserPlatform();
  assert.ok(platform, 'unsupported Chrome for Testing host platform');
  const buildId = await resolveBuildId(Browser.CHROME, platform, 'stable');
  const cacheDir = path.join(os.tmpdir(), 'idm-next-cft-cache');
  console.log('CFT_INSTALL build=' + buildId + ' platform=' + platform);
  const installed = await install({ browser: Browser.CHROME, buildId, platform, cacheDir,
                                   downloadProgressCallback: 'default' });
  assert.ok(fs.existsSync(installed.executablePath), 'Chrome for Testing executable not installed');
  console.log('CFT_READY exe=' + installed.executablePath);
  return [['Edge', edge], ['Chrome for Testing', installed.executablePath]];
}
async function httpServer() {
  const server = http.createServer((req, res) => {
    if (req.url === '/file.zip' || req.url === '/slow.zip') {
      const slow = req.url === '/slow.zip';
      res.writeHead(200, { 'Content-Type': 'application/zip',
                           'Content-Disposition': 'attachment; filename="' + (slow ? 'slow' : 'file') + '.zip"',
                           'Content-Length': payload.length,
                           'Cache-Control': 'no-store' });
      if (slow) {
        let at = 0;
        const timer = setInterval(() => {
          if (res.destroyed || at >= payload.length) {
            clearInterval(timer);
            if (!res.destroyed) res.end();
            return;
          }
          res.write(payload.subarray(at, at + 4096));
          at += 4096;
        }, 80);
        res.on('close', () => clearInterval(timer));
      } else res.end(payload);
    } else if (req.url === '/index.html' || req.url === '/slow.html') {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      const slow = req.url === '/slow.html';
      res.end('<!doctype html><html><body><a id="go" download="' + (slow ? 'slow' : 'file') + '.zip" href="/' + (slow ? 'slow' : 'file') + '.zip">Download</a></body></html>');
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
             '--no-default-browser-check', '--disable-gpu',
             ]
    });
    const popup = await context.newPage();
    await popup.goto('chrome-extension://' + ID + '/popup.html', { waitUntil: 'domcontentloaded' });
    const extensionId = await popup.evaluate(() => chrome.runtime.id);
    assert.equal(extensionId, ID, 'wrong extension loaded');
    await popup.evaluate(async (m) => {
      await chrome.storage.local.set({ interceptEnabled: true,
                                      confirmEnabled: m === 'no-source-confirm' || m === 'manual-dismiss' || m === 'popup-takeover-offline' });
    }, mode);

    // Service workers can be suspended/restarted between browser scenarios.
    // A target URL alone is not proof that the extension APIs are initialized.
    let worker = null;
    for (let attempt = 0; attempt < 40 && !worker; attempt++) {
      await popup.evaluate(() => new Promise(resolve => chrome.runtime.sendMessage(
        { action: 'getIntercept' }, response => resolve(response || null))));
      for (const candidate of context.serviceWorkers()) {
        if (!candidate.url().startsWith('chrome-extension://' + ID + '/')) continue;
        try {
          const ready = await candidate.evaluate(() => typeof chrome !== 'undefined'
            && !!(chrome.runtime && chrome.runtime.sendNativeMessage));
          if (ready) { worker = candidate; break; }
        } catch { /* target was replaced: retry after waking the extension */ }
      }
      if (!worker) await new Promise(resolve => setTimeout(resolve, 200));
    }
    assert.ok(worker, 'extension service worker never exposed nativeMessaging API');
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

    const explicitPopup = mode === 'popup-takeover-offline';
    const pagePath = explicitPopup ? '/slow.html' : '/index.html';
    const filePath = explicitPopup ? '/slow.zip' : '/file.zip';
    const page = await context.newPage();
    await page.goto(baseUrl + pagePath, { waitUntil: 'domcontentloaded' });
    await page.locator('#idm-next-tokens').waitFor({ state: 'attached', timeout: 12000 });
    const sourceTab = await popup.evaluate(async (url) => {
      const tabs = await chrome.tabs.query({});
      return tabs.find(x => x.url === url) || null;
    }, baseUrl + pagePath);
    assert.ok(sourceTab && sourceTab.id >= 0, 'source page has no browser tab ID');
    const contentReady = await popup.evaluate(async (tabId) => {
      return await new Promise(resolve => chrome.tabs.sendMessage(
        tabId, { action: 'showToast', message: 'E2E ready' },
        resp => resolve(!chrome.runtime.lastError && !!(resp && resp.ok))));
    }, sourceTab.id);
    assert.equal(contentReady, true, 'content script could not receive the probe');
    console.log('[PRE] ' + label + ' ' + mode + ' sourceTab=' + sourceTab.id);
    const pending = page.waitForEvent('download', { timeout: 15000 });
    await page.locator('#go').click();
    const download = await pending;

    if (explicitPopup) {
      // Genuine popup DOM + real browser downloads API. Native Host is absent:
      // a click must request handoff, report failure, preserve browser task.
      await popup.reload({ waitUntil: 'domcontentloaded' });
      const transfer = popup.locator('#browserCaptures .browser-capture-send');
      await transfer.first().waitFor({ timeout: 12000 });
      const text = await transfer.first().innerText();
      assert.match(text, /转交 IDM Next/);
      await transfer.first().click();
      await popup.locator('#browserCaptures .browser-capture-send').first()
        .waitFor({ state: 'visible', timeout: 12000 });
      console.log('[POPUP_CLICK] ' + label + ' mode=' + mode + ' clicked=true');
    }

    if (mode === 'no-source-confirm' || mode === 'manual-dismiss') {
      const diagnostic = await popup.evaluate(async (url) => {
        const all = await chrome.downloads.search({});
        const item = all.find(x => x.url === url);
        return item ? { id: item.id, tabId: item.tabId, state: item.state } : null;
      }, baseUrl + '/file.zip');
      console.log('[CAPTURE_PRE] ' + label + ' mode=' + mode + ' download=' + JSON.stringify(diagnostic));
      assert.ok(diagnostic && Number.isSafeInteger(diagnostic.id), 'download item missing');
      assert.equal(diagnostic.tabId, undefined, 'DownloadItem unexpectedly includes tabId');
      if (mode === 'no-source-confirm') {
        // Genuine onCreated, no tabId: fail closed without inventing a source tab.
        await page.waitForTimeout(300);
        assert.equal(await page.locator('#idm-next-capture').count(), 0,
                     'automatic confirmation must not target an unverified tab');
      } else {
        // Separate real content UI test: explicit tab-targeted message simulates
        // the capture-card entry, then an ACTUAL DOM click dismisses the card.
        // Does not claim that downloads.onCreated knows this tab.
        const ack = await popup.evaluate(({ tabId, url, id }) => new Promise(resolve => {
          chrome.tabs.sendMessage(tabId, { action: 'showCaptureDialog',
            resource: { url, filename: 'file.zip', browserDownloadId: id } },
            response => resolve(!chrome.runtime.lastError && !!(response && response.ok)));
        }), { tabId: sourceTab.id, url: baseUrl + '/file.zip', id: diagnostic.id });
        assert.equal(ack, true, 'explicit capture card could not reach content script');
        await page.locator('#idm-next-capture').waitFor({ timeout: 12000 });
        await page.locator('#idm-next-capture .idm-cap-cancel').click();
        await page.locator('#idm-next-capture').waitFor({ state: 'hidden', timeout: 12000 });
      }
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
    if (mode === 'manual-dismiss' || mode === 'no-source-confirm') {
      assert.equal(observed.calls.filter(x => x === 'add_download').length, 0,
                   'unconfirmed browser download must not reach native host');
    } else {
      assert.ok(observed.calls.includes('add_download'),
                'native add_download did not reach background handler');
      if (explicitPopup) assert.equal(observed.calls.filter(x => x === 'add_download').length, 1,
                                    'explicit popup click must hand off exactly once');
      if (mode === 'offline' || explicitPopup)
        assert.ok(observed.errors.length > 0, 'native host was not actually unavailable');
    }
    const live = await popup.evaluate(async () => chrome.downloads.search({}));
    const record = live.find(x => x.url === baseUrl + filePath);
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
  const found = await browsers();
  const { server, port } = await httpServer();
  console.log('SERVER_READY port=' + port + ' size=' + payload.length + ' sha256=' + sourceSha);
  try {
    for (const [label, exe] of found)
      for (const mode of ['offline', 'no-source-confirm', 'manual-dismiss', 'browser-first', 'popup-takeover-offline'])
        await runOne(exe, label, mode, 'http://127.0.0.1:' + port);
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error('[FAIL] browser takeover e2e:', error); process.exitCode = 1; });
