'use strict';
// Real Edge + Chrome-for-Testing MV3 discovery acceptance (no Native Host).
// Fresh profiles, early network requests, concurrent tabs, dynamic DOM,
// deterministic negatives, and a forced service-worker stop/restart.
// Hard assertions precede metric reporting; results are not synthetic.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright-core');
const { Browser, detectBrowserPlatform, resolveBuildId, install } = require('@puppeteer/browsers');

const EXT = path.resolve(__dirname, '..', '..', 'browser-extension');
const ID = 'fmnehlhldcoknecihnlbbcmhpaaapmbp';
const TRIALS = 3;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const allSamples = [];

async function browserPaths() {
  const roots = [process.env['PROGRAMFILES(X86)'], process.env.ProgramFiles,
                 process.env.LOCALAPPDATA].filter(Boolean);
  const edge = roots.map(r => path.join(r, 'Microsoft/Edge/Application/msedge.exe'))
                    .find(fs.existsSync);
  assert.ok(edge, 'Edge executable absent on Windows runner');
  const platform = detectBrowserPlatform();
  assert.ok(platform, 'unsupported Chrome platform');
  const build = await resolveBuildId(Browser.CHROME, platform, 'stable');
  const installed = await install({ browser: Browser.CHROME, buildId: build, platform,
    cacheDir: path.join(os.tmpdir(), 'idm-next-cft-cache') });
  assert.ok(fs.existsSync(installed.executablePath), 'Chrome for Testing absent');
  return [['Edge', edge], ['Chrome for Testing', installed.executablePath]];
}

function fixture(req, res) {
  const u = new URL(req.url, 'http://127.0.0.1');
  const token = u.searchParams.get('token') || '';
  const head = { 'Cache-Control': 'no-store', 'Access-Control-Allow-Origin': '*' };
  const media = {
    '/early.mp4': 'video/mp4',
    '/stream': 'video/mp4',
    '/track.mp3': 'audio/mpeg',
    '/playlist.m3u8': 'application/vnd.apple.mpegurl'
  };
  if (media[u.pathname]) {
    const body = Buffer.from('deterministic-mv3-fixture');
    res.writeHead(200, { ...head, 'Content-Type': media[u.pathname],
                         'Content-Length': body.length });
    res.end(body);
  } else if (u.pathname === '/not-media.txt') {
    res.writeHead(200, { ...head, 'Content-Type': 'text/plain' });
    res.end('this is not media');
  } else if (u.pathname === '/probe.html') {
    const scenario = u.searchParams.get('scenario');
    let paths = [];
    if (scenario === 'cold' || scenario === 'wake')
      paths = ['/early.mp4?token=' + token, '/early.mp4?token=' + token,
               '/not-media.txt?token=' + token + '&next=video.mp4'];
    else if (scenario === 'multi-a')
      paths = ['/early.mp4?token=' + token, '/stream?token=' + token,
               '/not-media.txt?token=' + token + '&next=video.mp4'];
    else if (scenario === 'multi-b')
      paths = ['/early.mp4?token=' + token, '/track.mp3?token=' + token,
               '/not-media.txt?token=' + token + '&next=video.mp4'];
    else if (scenario === 'multi-c')
      paths = ['/early.mp4?token=' + token, '/playlist.m3u8?token=' + token,
               '/not-media.txt?token=' + token + '&next=video.mp4'];
    else if (scenario !== 'dynamic') {
      res.writeHead(404); res.end('unknown scenario'); return;
    }
    const script = 'Promise.all(' + JSON.stringify(paths) +
      '.map(u=>fetch(u,{cache:"no-store"}).then(r=>{if(!r.ok)throw Error(r.status);return r.arrayBuffer()})))' +
      '.then(()=>window.__ready=true).catch(e=>window.__error=String(e));';
    res.writeHead(200, { ...head, 'Content-Type': 'text/html; charset=utf-8' });
    res.end('<!doctype html><html><head><script>' + script +
            '</script></head><body>MV3 fixture</body></html>');
  } else {
    res.writeHead(404); res.end('absent');
  }
}

async function waitFor(fn, label, ms=10000) {
  const deadline = Date.now() + ms;
  let latest;
  while (Date.now() < deadline) {
    try { latest = await fn(); if (latest) return latest; }
    catch (e) { latest = String(e); }
    await sleep(150);
  }
  throw Error(label + ' did not converge; last=' + JSON.stringify(latest));
}

async function runTrial(exe, label, index, base) {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'idm-mv3-discovery-'));
  const name = label + '/trial-' + index;
  let context;
  const counters = { expected: 0, found: 0, negatives: 0, falsePositives: 0, duplicates: 0 };
  try {
    context = await chromium.launchPersistentContext(profile, {
      executablePath: exe, headless: true, acceptDownloads: true,
      args: ['--headless=new', '--disable-extensions-except=' + EXT,
             '--load-extension=' + EXT, '--no-first-run',
             '--no-default-browser-check', '--disable-gpu']
    });
    async function openScenario(scenario, token) {
      const tab = await context.newPage();
      const target = base + '/probe.html?scenario=' + scenario + '&token=' + token;
      await tab.goto(target, { waitUntil: 'domcontentloaded' });
      await tab.waitForFunction(() => window.__ready === true || !!window.__error, { timeout: 12000 });
      const error = await tab.evaluate(() => window.__error || null);
      assert.equal(error, null, name + '/' + scenario + ': fixture fetch failed: ' + error);
      return { tab, token, target };
    }

    // The first early request finishes BEFORE opening an extension-owned page.
    // This is the cold-start race absent from the previous media-backfill test.
    const cold = await openScenario('cold', 'cold-' + index);
    const popup = await context.newPage();
    await popup.goto('chrome-extension://' + ID + '/popup.html', { waitUntil: 'domcontentloaded' });
    assert.equal(await popup.evaluate(() => chrome.runtime.id), ID);

    async function read(tab) {
      const url = tab.url();
      const id = await popup.evaluate(async target => {
        const tabs = await chrome.tabs.query({});
        return (tabs.find(t => t.url === target) || {}).id ?? null;
      }, url);
      assert.ok(Number.isSafeInteger(id), name + ': no tab ID for ' + url);
      return popup.evaluate(async tabId => {
        const key = 'tab_' + tabId;
        const data = await chrome.storage.session.get(key);
        return data[key] || [];
      }, id);
    }
    async function verify(tabInfo, expectedPaths, requiredSource=null) {
      let rows = await waitFor(async () => {
        const list = await read(tabInfo.tab);
        const hits = expectedPaths.filter(p => list.some(r =>
          new URL(r.url).pathname === p && new URL(r.url).searchParams.get('token') === tabInfo.token));
        return hits.length === expectedPaths.length ? list : false;
      }, name + '/' + tabInfo.token);
      // Give in-flight onHeadersReceived / DOM messages time to finish before judging negatives.
      await sleep(500);
      rows = await read(tabInfo.tab);
      const expectedUrls = expectedPaths.map(p => base + p + '?token=' + tabInfo.token);
      for (const exact of expectedUrls) {
        const hits = rows.filter(r => r.url === exact);
        counters.expected++;
        counters.found += Math.min(hits.length, 1);
        if (hits.length !== 1) counters.duplicates += Math.max(0, hits.length - 1);
        assert.equal(hits.length, 1, name + ' missing/duplicate ' + exact + ': ' + JSON.stringify(rows));
        if (requiredSource && exact.includes(requiredSource.path))
          assert.ok((hits[0].sources || []).includes(requiredSource.source),
                    name + ' missing ' + requiredSource.source + ': ' + JSON.stringify(hits[0]));
      }
      // A query parameter ending in video.mp4 MUST NOT promote a text/plain response to media.
      const negatives = rows.filter(r => r.url.includes('/not-media.txt'));
      if (!tabInfo.token.startsWith('dynamic-')) counters.negatives++;
      counters.falsePositives += negatives.length;
      assert.equal(negatives.length, 0,
                   name + ' classified plain text as media: ' + JSON.stringify(negatives));
      assert.equal(rows.length, expectedPaths.length,
                   name + ' cross-tab leak/unexpected resource: ' + JSON.stringify(rows));
      return rows;
    }
    await verify(cold, ['/early.mp4']);

    // Prove that a real MV3 worker can stop and be reactivated, not merely
    // send a message to an already-running worker.
    const cdp = await context.newCDPSession(popup);
    await cdp.send('ServiceWorker.enable');
    const before = await waitFor(async () =>
      context.serviceWorkers().find(w => w.url().startsWith('chrome-extension://' + ID + '/')),
      name + ' original service worker');
    await before.evaluate(() => { globalThis.__discoveryInstanceMarker = 'pre-stop'; });
    await cdp.send('ServiceWorker.stopWorker', { scopeURL: 'chrome-extension://' + ID + '/' });
    await waitFor(async () => {
      try { await before.evaluate(() => 'alive'); return false; } catch { return true; }
    }, name + ' service worker stop');
    const wake = await openScenario('wake', 'wake-' + index);
    await verify(wake, ['/early.mp4']);
    const after = await waitFor(async () =>
      context.serviceWorkers().find(w => w.url().startsWith('chrome-extension://' + ID + '/')),
      name + ' restarted service worker');
    assert.equal(await after.evaluate(() => globalThis.__discoveryInstanceMarker),
                 undefined, name + ' old worker survived stop');

    // DOM mutation without HTTP fetch must be detected by the content script.
    const dynamic = await openScenario('dynamic', 'dynamic-' + index);
    await dynamic.tab.locator('#idm-next-tokens').waitFor({ state: 'attached', timeout: 12000 });
    await dynamic.tab.evaluate(token => {
      const video = document.createElement('video');
      video.preload = 'none';
      const source = document.createElement('source');
      source.src = '/early.mp4?token=' + token;
      source.type = 'video/mp4';
      video.appendChild(source);
      document.body.appendChild(video);
    }, dynamic.token);
    await verify(dynamic, ['/early.mp4']);

    // Three tabs fetch concurrently; each has a unique token. Session keys may
    // never mix and MIME-only stream detection cannot depend on a .mp4 pathname.
    const concurrent = await Promise.all(['multi-a', 'multi-b', 'multi-c'].map((sc, i) =>
      openScenario(sc, 'parallel-' + index + '-' + i)));
    const expects = [['/early.mp4','/stream'], ['/early.mp4','/track.mp3'],
                     ['/early.mp4','/playlist.m3u8']];
    await Promise.all(concurrent.map((entry,i)=>verify(entry,expects[i])));
    for (const entry of concurrent) await entry.tab.close();
    console.log('[PASS] MV3_DISCOVERY ' + name + ' ' + JSON.stringify(counters) +
                ' worker_stop=confirmed worker_restart=confirmed');
    allSamples.push({ browser: label, trial: index, ...counters });
  } finally {
    if (context) await context.close();
    fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 400 });
  }
}

(async () => {
  const server = http.createServer(fixture);
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  try {
    const base = 'http://127.0.0.1:' + server.address().port;
    for (const [label, exe] of await browserPaths())
      for (let i = 1; i <= TRIALS; i++) await runTrial(exe, label, i, base);
    const totals = allSamples.reduce((a, x) => {
      for (const k of ['expected','found','negatives','falsePositives','duplicates']) a[k] += x[k];
      return a;
    }, { expected:0, found:0, negatives:0, falsePositives:0, duplicates:0 });
    const report = { rounds: allSamples.length, ...totals,
      detectionRate: totals.found / totals.expected,
      falsePositiveCount: totals.falsePositives, duplicateCount: totals.duplicates };
    console.log('MV3_DISCOVERY_MATRIX_RESULT ' + JSON.stringify(report));
    assert.equal(totals.found, totals.expected);
    assert.equal(totals.falsePositives, 0);
    assert.equal(totals.duplicates, 0);
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => {
  console.error('[FAIL] MV3_DISCOVERY_MATRIX ' + (error.stack || error));
  process.exitCode = 1;
});
