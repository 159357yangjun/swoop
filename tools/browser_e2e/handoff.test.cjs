// Native handoff behavior tests (Node built-ins only, no real browser needed).
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '..', '..', 'browser-extension', 'background.js'), 'utf8');

function listener() { return { fn: null, addListener(fn) { this.fn = fn; } }; }
function make({ accepted = true, acceptedTaskId = 47, failJournal = false, confirm = false, intercept = true, dialogOK = true, tabId = 19, omitTabId = false, state = 'in_progress' } = {}) {
  const events = { created: listener(), message: listener() };
  const calls = { native: 0, cancel: [], erase: [], dialogs: 0, nativeActions: [] };
  const local = {};
  const SESSION_A = '11111111-1111-4111-8111-111111111111';
  let guiSession = SESSION_A;
  const item = { id: 73, tabId, state, url: 'https://example.test/file.zip', filename: 'file.zip', mime: 'application/zip' };
  if (omitTabId) delete item.tabId;
  const noop = listener();
  const chrome = {
    runtime: {
      id: 'mock-extension', lastError: null, onInstalled: noop, onMessage: events.message,
      getURL(path) { return 'chrome-extension://mock-extension/' + path; },
      sendNativeMessage(_host, _payload, callback) {
        calls.native++;
        calls.nativeActions.push(_payload.action);
        queueMicrotask(() => callback(_payload.action === 'get_task_status'
          ? (_payload.sessionId === guiSession
              ? { success: true, taskId: _payload.taskId, sessionId: guiSession, state: 4,
                  stateText: '失败', error: 'HTTP 404', message: '任务失败' }
              : { success: false, message: 'old desktop session' })
          : { success: accepted, taskId: accepted ? acceptedTaskId : null,
              sessionId: accepted ? guiSession : null,
              message: accepted ? 'accepted' : 'failed' }));
      },
      sendMessage() { return Promise.resolve(); }
    },
    storage: {
      local: {
        async get(key) {
          const selected = key === 'recentHandoffs'
            ? { recentHandoffs: local.recentHandoffs } : {};
          return { interceptEnabled: intercept, confirmEnabled: confirm, ...selected };
        },
        async set(data) {
          if (failJournal && data.recentHandoffs) throw new Error('storage unavailable');
          Object.assign(local, data);
        }
      },
      session: { async get() { return {}; }, async set() {}, async remove() {} }
    },
    downloads: {
      onCreated: events.created,
      async search(query) {
        if (Object.hasOwn(query, 'id')) return query.id === item.id ? [{ ...item }] : [];
        if (query.state === 'in_progress') return item.state === 'in_progress' ? [{ ...item }] : [];
        return [{ ...item }];
      },
      cancel(id, callback) { calls.cancel.push(id); callback(); },
      erase({ id }, callback) { calls.erase.push(id); callback(); }
    },
    tabs: {
      onRemoved: noop, onUpdated: noop,
      sendMessage(_id, _msg, callback) { calls.dialogs++; callback({ ok: dialogOK }); },
      query(_query, callback) { callback([]); }
    },
    webRequest: { onBeforeRequest: noop, onHeadersReceived: noop },
    contextMenus: { onClicked: noop, create() {}, removeAll(cb) { cb(); } },
    action: {
      setBadgeText() {}, setBadgeBackgroundColor() {},
      getBadgeText(_arg, callback) { callback(''); }
    }
  };
  vm.runInNewContext(source, { chrome, console, URL, setTimeout() {}, clearTimeout() {} },
                     { filename: 'background.js' });
  return {
    item, calls,
    setGuiSession(value) { guiSession = value; },
    async message(msg, fromPopup = true, overrideSender = null) {
      return new Promise(resolve => {
        const sender = overrideSender || (fromPopup
          ? { id: 'mock-extension' } // toolbar popup: optional sender.url, no tab
          : { id: 'mock-extension', tab: { id: 19 }, url: 'https://example.test/page' });
        const keep = events.message.fn(msg, sender, resolve);
        assert.equal(keep, true);
      });
    },
    async start() {
      await events.created.fn(item);
      await new Promise(resolve => setImmediate(resolve));
    },
    async submit(opts = {}, tabId = item.tabId) {
      return await new Promise(resolve => {
        const keeps = events.message.fn({
          action: 'downloadWithOptions', url: item.url, filename: item.filename,
          browserDownloadId: item.id, ...opts
        }, { tab: { id: tabId } }, resolve);
        assert.equal(keeps, true);
      });
    }
  };
}

test('host failure never cancels browser download', async () => {
  const h = make({ accepted: false }); await h.start();
  assert.equal(h.calls.native, 1); assert.deepEqual(h.calls.cancel, []);
  assert.deepEqual(h.calls.erase, []);
});
test('host acceptance retires only in-progress original', async () => {
  const h = make(); await h.start();
  assert.deepEqual(h.calls.cancel, [73]); assert.deepEqual(h.calls.erase, [73]);
});
test('completed browser download stays intact', async () => {
  const h = make({ state: 'complete' }); await h.start();
  assert.deepEqual(h.calls.cancel, []); assert.deepEqual(h.calls.erase, []);
});
test('confirmation does not cancel before the user submits', async () => {
  const h = make({ confirm: true }); await h.start();
  assert.equal(h.calls.dialogs, 1); assert.equal(h.calls.native, 0);
  assert.deepEqual(h.calls.cancel, []);
  const result = await h.submit();
  assert.equal(result.success, true); assert.deepEqual(h.calls.cancel, [73]);
});
test('dismissed confirmation keeps original browser download', async () => {
  const h = make({ confirm: true }); await h.start();
  assert.deepEqual(h.calls.cancel, []); assert.deepEqual(h.calls.erase, []);
});
test('rejected confirmation submission keeps original', async () => {
  const h = make({ confirm: true, accepted: false }); await h.start();
  const result = await h.submit();
  assert.equal(result.success, false); assert.deepEqual(h.calls.cancel, []);
});
test('unrelated tab cannot cancel original download', async () => {
  const h = make({ confirm: true }); await h.start();
  await h.submit({}, 88); assert.deepEqual(h.calls.cancel, []);
});
test('wrong URL cannot cancel original download', async () => {
  const h = make({ confirm: true }); await h.start();
  await h.submit({ url: 'https://example.test/other.zip' });
  assert.deepEqual(h.calls.cancel, []);
});
test('manual popup without download ID cannot cancel original', async () => {
  const h = make({ confirm: true }); await h.start();
  await h.submit({ browserDownloadId: null });
  assert.deepEqual(h.calls.cancel, []);
});

test('automatic takeover is opt-in: disabled leaves browser untouched', async () => {
  const h = make({ intercept: false });
  await h.start();
  assert.equal(h.calls.native, 0);
  assert.deepEqual(h.calls.cancel, []);
  assert.deepEqual(h.calls.erase, []);
});

test('failed confirmation card never bypasses user approval', async () => {
  const h = make({ confirm: true, dialogOK: false });
  await h.start();
  assert.equal(h.calls.dialogs, 1);
  assert.equal(h.calls.native, 0);
  assert.deepEqual(h.calls.cancel, []);
});


test('confirmation cannot silently bypass a download without a source tab', async () => {
  const h = make({ confirm: true, tabId: -1 });
  await h.start();
  assert.equal(h.calls.native, 0);
  assert.deepEqual(h.calls.cancel, []);
  assert.deepEqual(h.calls.erase, []);
});


test('actual Chrome DownloadItem omits tabId: confirmation must fail closed', async () => {
  const h = make({ confirm: true, omitTabId: true });
  await h.start();
  assert.equal(h.calls.dialogs, 0);
  assert.equal(h.calls.native, 0);
  assert.deepEqual(h.calls.cancel, []);
  assert.deepEqual(h.calls.erase, []);
});

test('live browser-download list exposes only eligible in-progress items', async () => {
  const h = make({ confirm: true, omitTabId: true });
  const res = await h.message({ action: 'getBrowserDownloads' });
  assert.equal(res.success, true);
  assert.equal(res.items.length, 1);
  assert.equal(res.items[0].id, 73);
  const completed = make({ state: 'complete' });
  const done = await completed.message({ action: 'getBrowserDownloads' });
  assert.equal(done.items.length, 0);
});

test('explicit popup takeover accepts once before canceling browser download', async () => {
  const h = make({ confirm: true, omitTabId: true });
  const res = await h.message({ action: 'takeoverBrowserDownload', id: 73 });
  assert.equal(res.success, true);
  assert.equal(res.browserCanceled, true);
  assert.equal(h.calls.native, 1);
  assert.deepEqual(h.calls.cancel, [73]);
  assert.deepEqual(h.calls.erase, [73]);
});

test('explicit popup takeover failure preserves original browser download', async () => {
  const h = make({ accepted: false, confirm: true, omitTabId: true });
  const res = await h.message({ action: 'takeoverBrowserDownload', id: 73 });
  assert.equal(res.success, false);
  assert.deepEqual(h.calls.cancel, []);
  assert.deepEqual(h.calls.erase, []);
});

test('content script cannot forge a popup takeover ID', async () => {
  const h = make({ confirm: true, omitTabId: true });
  // This rejection happens synchronously, unlike the valid async popup path.
  const h2 = make({ confirm: true, omitTabId: true });
  const denial = await new Promise(resolve => {
    // Source verification is explicit in the product handler, not in a UI mock.
    const fake = { action: 'takeoverBrowserDownload', id: 73 };
    // exercise via harness method with tab-backed sender; it replies immediately
    h2.message(fake, false).then(resolve);
  });
  assert.equal(denial.success, false);
  assert.equal(h2.calls.native, 0);
  assert.deepEqual(h2.calls.cancel, []);
});

test('stale or finished download cannot be transferred from popup', async () => {
  const h = make({ state: 'complete', confirm: true, omitTabId: true });
  const res = await h.message({ action: 'takeoverBrowserDownload', id: 73 });
  assert.equal(res.success, false);
  assert.equal(h.calls.native, 0);
  assert.deepEqual(h.calls.cancel, []);
});


test('popup takeover rejects browser content-script senders even from our extension', async () => {
  const h = make({ confirm: true, omitTabId: true });
  const result = await h.message({ action: 'takeoverBrowserDownload', id: 73 }, false);
  assert.equal(result.success, false);
  assert.equal(h.calls.native, 0);
  assert.deepEqual(h.calls.cancel, []);
});

test('verified popup.html opened in extension tab can request explicit takeover', async () => {
  const h = make({ confirm: true, omitTabId: true });
  const trusted = { id: 'mock-extension', tab: { id: 77 },
                    url: 'chrome-extension://mock-extension/popup.html' };
  const result = await h.message({ action: 'takeoverBrowserDownload', id: 73 }, true, trusted);
  assert.equal(result.success, true);
  assert.equal(h.calls.native, 1);
  assert.deepEqual(h.calls.cancel, [73]);
});

test('tracked handoff persists task ID and can show later failure, not just accepted', async () => {
  const h = make();
  await h.start();
  const saved = await h.message({ action: 'getRecentHandoffs' });
  assert.equal(saved.success, true);
  assert.equal(saved.items[0].taskId, 47);
  assert.equal(saved.items[0].browserDownloadId, 73);
  const status = await h.message({ action: 'getHandoffStatus', taskId: 47 });
  assert.equal(status.success, true);
  assert.equal(status.state, 4);
  assert.equal(status.error, 'HTTP 404');
  assert.equal(h.calls.nativeActions.at(-1), 'get_task_status');
  // Cancellation history stays visible as a recovery witness.
  assert.deepEqual(h.calls.erase, []);
});
test('CLI-style success without task ID cannot cancel browser original', async () => {
  const h = make({ acceptedTaskId: null });
  await h.start();
  assert.deepEqual(h.calls.cancel, []);
  assert.deepEqual(h.calls.erase, []);
  const list = await h.message({ action: 'getRecentHandoffs' });
  assert.equal(list.items.length, 0);
});
test('if handoff journal cannot persist, browser download is preserved', async () => {
  const h = make({ failJournal: true });
  await h.start();
  assert.deepEqual(h.calls.cancel, []);
  assert.deepEqual(h.calls.erase, []);
});
test('content scripts cannot query handoff history or per-task state', async () => {
  const h = make();
  await h.start();
  const list = await h.message({ action: 'getRecentHandoffs' }, false);
  const status = await h.message({ action: 'getHandoffStatus', taskId: 47 }, false);
  assert.equal(list.success, false);
  assert.equal(status.success, false);
});
test('status query rejects untracked task IDs', async () => {
  const h = make();
  await h.start();
  const prevNative = h.calls.native;
  const reply = await h.message({ action: 'getHandoffStatus', taskId: 999 });
  assert.equal(reply.success, false);
  assert.equal(h.calls.native, prevNative);
});

test('manually sent resource also leaves a queryable task record', async () => {
  const h = make({ confirm: true });
  const accepted = await h.message({ action: 'download', url: h.item.url, filename: 'file.zip' });
  assert.equal(accepted.success, true);
  assert.equal(accepted.taskId, 47);
  assert.equal(accepted.tracked, true);
  const history = await h.message({ action: 'getRecentHandoffs' });
  assert.equal(history.items[0].taskId, 47);
  const state = await h.message({ action: 'getHandoffStatus', taskId: 47 });
  assert.equal(state.state, 4);
  assert.equal(state.error, 'HTTP 404');
});

test('manual send without task ID is explicitly not tracked', async () => {
  const h = make({ acceptedTaskId: null });
  const result = await h.message({ action: 'download', url: h.item.url, filename: 'file.zip' });
  assert.equal(result.success, true);
  assert.equal(result.tracked, false);
  assert.equal(result.taskId, null);
  const history = await h.message({ action: 'getRecentHandoffs' });
  assert.equal(history.items.length, 0);
});

test('desktop process restart rejects reused integer ID from old handoff', async () => {
  const h = make();
  await h.start();
  const original = await h.message({ action: 'getRecentHandoffs' });
  assert.equal(original.items[0].taskId, 47);
  assert.equal(original.items[0].sessionId, '11111111-1111-4111-8111-111111111111');
  h.setGuiSession('22222222-2222-4222-8222-222222222222');
  const status = await h.message({ action: 'getHandoffStatus', taskId: 47 });
  assert.equal(status.success, false);
  assert.match(status.message, /old desktop session/);
  assert.deepEqual(h.calls.erase, []);
});
