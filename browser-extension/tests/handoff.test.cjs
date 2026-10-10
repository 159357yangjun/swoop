// Native handoff behavior tests (Node built-ins only, no real browser needed).
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '..', 'background.js'), 'utf8');

function listener() { return { fn: null, addListener(fn) { this.fn = fn; } }; }
function make({ accepted = true, confirm = false, intercept = true, dialogOK = true, tabId = 19, state = 'in_progress' } = {}) {
  const events = { created: listener(), message: listener() };
  const calls = { native: 0, cancel: [], erase: [], dialogs: 0 };
  const item = { id: 73, tabId, state, url: 'https://example.test/file.zip', filename: 'file.zip', mime: 'application/zip' };
  const noop = listener();
  const chrome = {
    runtime: {
      id: 'mock-extension', lastError: null, onInstalled: noop, onMessage: events.message,
      sendNativeMessage(_host, _payload, callback) {
        calls.native++;
        queueMicrotask(() => callback({ success: accepted, message: accepted ? 'accepted' : 'failed' }));
      },
      sendMessage() { return Promise.resolve(); }
    },
    storage: {
      local: { async get() { return { interceptEnabled: intercept, confirmEnabled: confirm }; }, async set() {} },
      session: { async get() { return {}; }, async set() {}, async remove() {} }
    },
    downloads: {
      onCreated: events.created,
      async search({ id }) { return id === item.id ? [{ ...item }] : []; },
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
  const h = make({ confirm: true, tabId: undefined });
  await h.start();
  assert.equal(h.calls.dialogs, 0);
  assert.equal(h.calls.native, 0);
  assert.deepEqual(h.calls.cancel, []);
  assert.deepEqual(h.calls.erase, []);
});
