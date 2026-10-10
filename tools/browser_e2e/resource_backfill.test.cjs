'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const helperSource = fs.readFileSync(
    path.join(__dirname, '..', '..', 'browser-extension', 'resource_backfill.js'), 'utf8');
function helper() {
    const scope = { URL };
    scope.globalThis = scope;
    vm.runInNewContext(helperSource, scope, { filename: 'resource_backfill.js' });
    return scope.IDMResourceBackfill;
}

test('early resource timing media URLs are recovered with auditable source', () => {
    const seen = new Set();
    const matches = helper().collect([
        { name: 'https://media.example/video.mp4?token=test' },
        { name: 'https://media.example/live.m3u8' },
        { name: 'https://media.example/audio.mp3' }
    ], seen, 'https://site.example/page');
    assert.equal(matches.length, 3);
    assert.deepEqual(Array.from(matches, x => x.type), ['video', 'video', 'audio']);
    assert.ok(matches.every(x => x.source === 'performance-backfill'));
    assert.equal(seen.size, 3);
});

test('negative sources, query-string bait, non-http and duplicate entries stay excluded', () => {
    const seen = new Set(['https://site.example/seen.mp4']);
    const entries = [
        { name: 'https://site.example/seen.mp4' },
        { name: 'https://site.example/js/script.js?next=video.mp4' },
        { name: 'https://site.example/app.css' },
        { name: 'https://site.example/text.txt' },
        { name: 'data:text/plain,test.mp4' },
        { name: 'blob:https://site.example/id.mp4' },
        { name: 'https://site.example/new.mp4' },
        { name: 'https://site.example/new.mp4' }
    ];
    const matches = helper().collect(entries, seen, 'https://site.example/');
    assert.equal(matches.length, 1);
    assert.equal(matches[0].url, 'https://site.example/new.mp4');
});

test('timing media URL and DOM source share URL dedup', () => {
    const seen = new Set(['https://site.example/from-dom.mp4']);
    const matches = helper().collect([{ name: '/from-dom.mp4' }], seen,
                                     'https://site.example/watch');
    assert.equal(matches.length, 0);
});

function listener() { return { fn: null, addListener(fn) { this.fn = fn; } }; }
function delay(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }

function backgroundHarness() {
    const resources = new Map();
    const onBeforeRequest = listener();
    const onHeadersReceived = listener();
    const noOp = listener();
    const chrome = {
        runtime: { id: 'test-extension', lastError: null,
            onInstalled: noOp, onMessage: noOp, getURL(x) { return 'chrome-extension://test-extension/' + x; } },
        storage: {
            local: { async get() { return {}; }, async set() {} },
            session: {
                async get(key) {
                    const snapshot = resources.get(key) || [];
                    await delay(8);
                    return { [key]: JSON.parse(JSON.stringify(snapshot)) };
                },
                async set(record) {
                    await delay(6);
                    for (const [k, v] of Object.entries(record))
                        resources.set(k, JSON.parse(JSON.stringify(v)));
                },
                async remove(key) { resources.delete(key); }
            }
        },
        webRequest: { onBeforeRequest, onHeadersReceived },
        action: { async setBadgeText() {}, async setBadgeBackgroundColor() {} },
        tabs: { onRemoved: noOp, onUpdated: noOp,
            query(_x, cb) { cb([]); }, sendMessage(_x, _y, cb) { cb({ ok: true }); } },
        downloads: { onCreated: noOp, search: async () => [], cancel(_id, cb) { cb(); },
            erase(_arg, cb) { cb(); } },
        contextMenus: { onClicked: noOp, create() {}, removeAll(cb) { cb(); } }
    };
    vm.runInNewContext(fs.readFileSync(
        path.join(__dirname, '..', '..', 'browser-extension', 'background.js'), 'utf8'),
        { chrome, URL, console, setTimeout() {}, clearTimeout() {} },
        { filename: 'background.js' });
    return { resources, before: onBeforeRequest.fn, headers: onHeadersReceived.fn,
             report: noOp.fn };
}

test('concurrent per-tab resources are not overwritten by storage.session read-modify-write', async () => {
    const h = backgroundHarness();
    for (let i = 0; i < 15; i++) {
        h.before({ tabId: 17, url: 'https://media.example/clip' + i + '.mp4',
                   initiator: 'https://site.example' });
    }
    await delay(430);
    const list = h.resources.get('tab_17') || [];
    assert.equal(list.length, 15, 'lost URL from concurrent storage writes');
    assert.equal(new Set(list.map(r => r.url)).size, 15);
});

test('URL and headers share one entry, enhancing MIME and length without duplicates', async () => {
    const h = backgroundHarness();
    const url = 'https://media.example/v.mp4';
    h.before({ tabId: 21, url, initiator: '' });
    h.headers({ tabId: 21, url, initiator: '', responseHeaders: [
        { name: 'Content-Type', value: 'video/mp4' },
        { name: 'Content-Length', value: '2300' }
    ] });
    await delay(90);
    const list = h.resources.get('tab_21') || [];
    assert.equal(list.length, 1);
    assert.equal(list[0].size, 2300);
    assert.equal(list[0].mimeType, 'video/mp4');
    assert.deepEqual(Array.from(list[0].sources), ['webRequest', 'webRequest-headers']);
});

test('headers alone may insert media when URL event was missed; text is excluded', async () => {
    const h = backgroundHarness();
    h.headers({ tabId: 23, url: 'https://media.example/file-no-extension', responseHeaders: [
        { name: 'Content-Type', value: 'video/mp4' }
    ] });
    h.headers({ tabId: 23, url: 'https://media.example/text', responseHeaders: [
        { name: 'Content-Type', value: 'text/plain' }
    ] });
    await delay(75);
    const list = h.resources.get('tab_23') || [];
    assert.equal(list.length, 1);
    assert.equal(list[0].url, 'https://media.example/file-no-extension');
});
