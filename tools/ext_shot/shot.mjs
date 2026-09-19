/**
 * shot.mjs — 浏览器扩展 popup 的离屏截图 / 运行时断言工具
 *
 * 为什么要它：扩展 UI 改完，"看起来更好" 这件事没法靠读 CSS 证明。
 * 这里用本机已有的 Edge 无头模式 + CDP **加载真实的 popup.html**，
 * 只在导航前注入一份 mock 的 chrome.* API（真实扩展里这份由浏览器提供），
 * 然后把每个场景截图存盘，并顺便断言主题令牌、行数、计数、按钮文案。
 *
 * 不下载任何东西：只用本机 Edge + Node 自带的全局 WebSocket。
 *
 * 用法（Edge 必须先由外部脚本起好，见 tools/ext_shot/run.sh）：
 *   node "D:/.../tools/ext_shot/shot.mjs" <调试端口> <popup.html 绝对路径> <输出目录>
 */
import fs from 'node:fs';
import path from 'node:path';

const PORT = Number(process.argv[2] || 9333);
const POPUP = process.argv[3];
const OUT = process.argv[4];
if (!POPUP || !OUT) { console.error('用法: node shot.mjs <port> <popup.html> <outdir>'); process.exit(2); }
fs.mkdirSync(OUT, { recursive: true });

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

/* ── 取 page 级 target ──
 * ⚠️ 不能用 /json/version 给的那个 URL：它是 browser 级 target，只认 Target 命名空间。
 *    拿它去发 Page.enable / Runtime.evaluate 会静默失败（回调永远等不到），
 *    必须从 /json/list 里挑 type == "page" 的那条。 */
async function pageTarget() {
    for (let i = 0; i < 40; i++) {
        try {
            const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
            const t = list.find(x => x.type === 'page' && x.webSocketDebuggerUrl);
            if (t) return t;
        } catch { /* Edge 还没起来 */ }
        await sleep(250);
    }
    throw new Error('找不到 page target，Edge 调试端口可能没起来');
}

const target = await pageTarget();
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((res, rej) => {
    ws.addEventListener('open', res, { once: true });
    ws.addEventListener('error', rej, { once: true });
});

let seq = 0;
const pending = new Map();
let consoleErrors = [];
ws.addEventListener('message', (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
        const { res, rej } = pending.get(m.id);
        pending.delete(m.id);
        m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result);
        return;
    }
    // ⚠️ console.* 走 Runtime.consoleAPICalled，不在 Log.entryAdded
    if (m.method === 'Runtime.consoleAPICalled' &&
        (m.params.type === 'error' || m.params.type === 'warning')) {
        consoleErrors.push(m.params.args.map(a => a.value ?? a.description ?? '').join(' '));
    }
    if (m.method === 'Runtime.exceptionThrown') {
        const d = m.params.exceptionDetails;
        consoleErrors.push('UNCAUGHT: ' + (d.exception?.description || d.text));
    }
});
const send = (method, params = {}) => new Promise((res, rej) => {
    const id = ++seq;
    pending.set(id, { res, rej });
    ws.send(JSON.stringify({ id, method, params }));
});

await send('Page.enable');
await send('Runtime.enable');

/* ── mock 的 chrome.*：字段与 background.js 的响应完全一致 ──────────
   popup.js 只通过 chrome.runtime.sendMessage / chrome.storage.local 取数据，
   把这两个 mock 准了，跑的就是真实代码路径（不是另写一个演示页）。 */
const MOCK = `
window.chrome = {
  runtime: {
    lastError: undefined,
    onMessage: { addListener(){}, removeListener(){} },
    sendMessage(msg, cb) {
      const S = window.__SHOT__ || {};
      let resp = {};
      switch (msg.action) {
        case 'getResources':  resp = { resources: S.resources || [], tabUrl: S.tabUrl || '' }; break;
        case 'checkConnection': resp = { connected: !!S.connected, version: S.connected ? '0.2.0' : null }; break;
        case 'getIntercept':  resp = { enabled: !!S.intercept }; break;
        case 'getConfirm':    resp = { enabled: !!S.confirm }; break;
        case 'setIntercept':
        case 'setConfirm':    resp = { ok: true }; break;
        case 'clearResources': resp = { ok: true }; break;
        case 'downloadWithOptions': resp = { success: true, message: '已添加到 IDM Next' }; break;
        case 'batchDownload': {
          const n = (msg.items || []).length;
          resp = { success: n, message: '已发送 ' + n + ' 个到 IDM Next' };
          break;
        }
        default: resp = {};
      }
      setTimeout(() => cb && cb(resp), 8);
    },
  },
  tabs: {
    query(q, cb) { setTimeout(() => cb([{ id: 1, url: (window.__SHOT__ || {}).tabUrl || '' }]), 0); },
    sendMessage(id, m, cb) { setTimeout(() => cb && cb({ ok: true }), 0); },
  },
  storage: {
    local: {
      _d: {},
      get(keys, cb) {
        const d = Object.assign({}, this._d, (window.__SHOT__ || {}).storage || {});
        if (typeof keys === 'function') { keys(d); return; }
        setTimeout(() => cb(d), 0);
      },
      set(o, cb) { Object.assign(this._d, o); if (cb) cb(); },
    },
  },
  action: { setBadgeText() {}, setBadgeBackgroundColor() {} },
};
`;

/* ── 场景数据：覆盖六种文件类型 + 已发送态 + 各种大小 ─────────────── */
const RESOURCES = [
    { url: 'https://cdn.example-cdn.com/media/bigbuckbunny-1080p-h264-aac.mp4',   type: 'video',    filename: 'Big Buck Bunny 1080p H.264 AAC.mp4', size: 734003200, timestamp: 1 },
    { url: 'https://v.example.com/hls/master.m3u8?token=abc123def456ghi789',      type: 'video',    filename: 'master.m3u8', size: -1, timestamp: 2 },
    { url: 'https://dl.example.org/audio/album/01-opening-theme.flac',            type: 'audio',    filename: '01 - Opening Theme.flac', size: 41943040, timestamp: 3 },
    { url: 'https://dl.example.org/audio/album/02-battle.mp3',                    type: 'audio',    filename: '02 - Battle.mp3', size: 8912896, timestamp: 4 },
    { url: 'https://img.example.net/gallery/cover-hd-2026-09-18.jpg',             type: 'image',    filename: 'cover-hd-2026-09-18.jpg', size: 2539520, timestamp: 5 },
    { url: 'https://mirror.example.cn/tools/curl-8.22.0_1-win64-mingw.zip',       type: 'archive',  filename: 'curl-8.22.0_1-win64-mingw.zip', size: 8904576, timestamp: 6 },
    { url: 'https://mirror.example.cn/tools/setup-runtime-x64.exe',               type: 'app',      filename: 'setup-runtime-x64.exe', size: 96550912, timestamp: 7 },
    { url: 'https://docs.example.com/spec/产品技术白皮书-v3.2.pdf',                type: 'document', filename: '产品技术白皮书 v3.2.pdf', size: 4718592, timestamp: 8 },
    { url: 'magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=ubuntu-24.04.iso', type: 'magnet', filename: 'ubuntu-24.04.iso', size: -1, timestamp: 9 },
    { url: 'https://cdn.example-cdn.com/media/intro-teaser-webm.webm',            type: 'video',    filename: 'intro-teaser-webm.webm', size: 12897484, timestamp: 10 },
];

const BASE = {
    connected: true,
    intercept: true,
    confirm: false,
    tabUrl: 'https://www.example-video-site.com/watch?v=dQw4w9WgXcQ&list=PL1234567890',
    resources: RESOURCES,
    storage: { queueByDefault: false, uiTheme: 'light' },
};

const SCEN = [
    {
        name: 'popup-light', desc: '亮色 · 资源列表（勾选 3 项）',
        data: BASE,
        async act() {
            await send('Runtime.evaluate', { expression: `
                const rows = [...document.querySelectorAll('.row')];
                [0,1,2].forEach(i => rows[i] && rows[i].click());
            ` });
        },
    },
    {
        name: 'popup-dark', desc: '暗色 · 同一份数据（验证主题令牌真的换了）',
        data: { ...BASE, storage: { queueByDefault: true, uiTheme: 'dark' } },
        async act() {
            await send('Runtime.evaluate', { expression: `
                const rows = [...document.querySelectorAll('.row')];
                [0,3].forEach(i => rows[i] && rows[i].click());
            ` });
        },
    },
    {
        name: 'popup-search', desc: '搜索 + 类型筛选（计数随搜索联动）',
        data: BASE,
        async act() {
            await send('Runtime.evaluate', { expression: `
                const s = document.getElementById('searchInput');
                s.value = 'example.cn';
                s.dispatchEvent(new Event('input', { bubbles: true }));
            ` });
        },
    },
    {
        name: 'popup-settings', desc: '设置弹层（三个开关 + 主题三态）',
        data: BASE,
        async act() {
            await send('Runtime.evaluate', { expression: `
                document.getElementById('settingsBtn').click();
            ` });
        },
    },
    {
        name: 'popup-empty', desc: '空状态（未嗅探到资源）',
        data: { ...BASE, resources: [] },
        async act() {},
    },
    {
        name: 'popup-offline', desc: '桌面端未连接 + 无匹配搜索结果',
        data: { connected: false, resources: RESOURCES.slice(0, 2), tabUrl: 'https://example.com/', storage: { uiTheme: 'dark' } },
        async act() {
            await send('Runtime.evaluate', { expression: `
                const s = document.getElementById('searchInput');
                s.value = 'zzz-not-found';
                s.dispatchEvent(new Event('input', { bubbles: true }));
            ` });
        },
    },
];

const fileUrl = 'file:///' + POPUP.replace(/\\/g, '/');

async function waitStable() {
    let last = -1;
    for (let i = 0; i < 30; i++) {
        await sleep(300);
        const r = await send('Runtime.evaluate', {
            expression: `document.querySelectorAll('.row,.empty,.chip').length`,
            returnByValue: true,
        });
        const n = r.result.value;
        if (n > 0 && n === last) return;
        last = n;
    }
}

const report = [];
for (const s of SCEN) {
    consoleErrors = [];
    await send('Emulation.setDeviceMetricsOverride',
        { width: 400, height: 560, deviceScaleFactor: 2, mobile: false });
    const id = (await send('Page.addScriptToEvaluateOnNewDocument', {
        source: `window.__SHOT__ = ${JSON.stringify(s.data)};\n` + MOCK,
    })).identifier;

    await send('Page.navigate', { url: fileUrl });
    await waitStable();
    await s.act();
    await sleep(350);   // 让 toast/过渡动画落定

    const probe = (await send('Runtime.evaluate', {
        expression: `(() => {
            const cs = getComputedStyle(document.documentElement);
            const body = getComputedStyle(document.body);
            const rows = document.querySelectorAll('.row');
            const chips = [...document.querySelectorAll('.chip')].map(c => c.textContent.trim());
            return JSON.stringify({
                theme: document.documentElement.getAttribute('data-theme'),
                bg: body.backgroundColor,
                text: body.color,
                tokenBg: cs.getPropertyValue('--bg').trim(),
                accent: cs.getPropertyValue('--accent').trim(),
                rows: rows.length,
                chips,
                selCount: document.getElementById('selCount').textContent,
                dlLabel: document.querySelector('#downloadSelBtn .lbl').textContent,
                conn: document.getElementById('connText').textContent,
                sheet: document.getElementById('settingsSheet').classList.contains('show'),
                toast: document.getElementById('toast').textContent,
            });
        })()`, returnByValue: true,
    })).result.value;

    // 注意 send() 已经把 CDP 响应的 result 解出来了：
    // captureScreenshot 的 result 就是 {data}，所以这里是 shot.data（不是 shot.result.data）
    const shot = await send('Page.captureScreenshot', { format: 'png' });
    const file = path.join(OUT, s.name + '.png');
    fs.writeFileSync(file, Buffer.from(shot.data, 'base64'));
    await send('Page.removeScriptToEvaluateOnNewDocument', { identifier: id });

    const p = JSON.parse(probe);
    report.push({ name: s.name, desc: s.desc, probe: p, errors: [...consoleErrors], file });
    console.log(`\n[${s.name}] ${s.desc}`);
    console.log('   ' + probe);
    if (consoleErrors.length) console.log('   ⚠️ console: ' + consoleErrors.join(' | '));
}

fs.writeFileSync(path.join(OUT, 'report.json'), JSON.stringify(report, null, 2));
console.log(`\n截图 ${report.length} 张 → ${OUT}`);
ws.close();
