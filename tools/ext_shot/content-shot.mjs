/**
 * content-shot.mjs — 内容脚本「页面内浮层」的离屏截图 / 运行时断言工具
 *
 * 与 shot.mjs（弹窗）的分工：
 *   shot.mjs   → 加载真实 popup.html，只看弹窗本身
 *   本文件     → 加载 tools/ext_shot/content-harness.html（一个仿真的播放页，
 *                里面 **原样引入 browser-extension/content.js**），
 *                把宿主页面 + 悬浮球 + 资源面板 + 确认卡片一起截下来
 *
 * 不下载任何东西：只用本机 Edge + Node 自带的全局 WebSocket。
 *
 * 用法（Edge 必须先由外部脚本起好，见 tools/ext_shot/run.sh）：
 *   node "D:/.../tools/ext_shot/content-shot.mjs" <调试端口> <harness.html 绝对路径> <输出目录>
 */
import fs from 'node:fs';
import path from 'node:path';

const PORT = Number(process.argv[2] || 9333);
const HARNESS = process.argv[3];
const OUT = process.argv[4];
if (!HARNESS || !OUT) { console.error('用法: node content-shot.mjs <port> <harness.html> <outdir>'); process.exit(2); }
fs.mkdirSync(OUT, { recursive: true });

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

/* ── 取 page 级 target（不能用 /json/version 那条 browser target）── */
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

const VIDEO = { url: 'https://cdn.example-cdn.com/media/bigbuckbunny-1080p-h264-aac.mp4', filename: 'Big Buck Bunny 1080p H.264 AAC.mp4', type: 'video', size: 734003200 };
/* ⚠️ 确认卡片的「画质」下拉只在 looksLikeVideo() 为真时才去查 yt-dlp 格式表，
   而它认的是 VIDEO_HOSTS（youtube/bilibili/…）或 .m3u8/.mpd 结尾的地址。
   用普通 CDN 上的 .mp4 试这一块，下拉永远只有「最佳画质（默认）」一项 ——
   看着像没实现，其实是根本没进那条分支。所以这里用视频站点地址。 */
const VIDEO_SITE = { url: 'https://www.bilibili.com/video/BV1xx411c7mD', filename: '示例视频 · 1080P 完整版', type: 'video', size: -1 };

/* ── 内容脚本拿到的那份资源列表（由 mock 的 getResourcesForTab 返回）── */
const RESOURCES = [
    { url: VIDEO.url,                                                       type: 'video',    filename: VIDEO.filename, size: 734003200 },
    { url: 'https://v.example.com/hls/master.m3u8?token=abc123def456ghi789', type: 'video',    filename: 'master.m3u8', size: -1 },
    { url: 'https://dl.example.org/audio/album/01-opening-theme.flac',       type: 'audio',    filename: '01 - Opening Theme.flac', size: 41943040 },
    { url: 'https://dl.example.org/audio/album/02-battle.mp3',               type: 'audio',    filename: '02 - Battle.mp3', size: 8912896 },
    { url: 'https://mirror.example.cn/tools/curl-8.22.0_1-win64-mingw.zip',  type: 'archive',  filename: 'curl-8.22.0_1-win64-mingw.zip', size: 8904576 },
    { url: 'https://mirror.example.cn/tools/setup-runtime-x64.exe',          type: 'app',      filename: 'setup-runtime-x64.exe', size: 96550912 },
    { url: 'https://docs.example.com/spec/产品技术白皮书-v3.2.pdf',           type: 'document', filename: '产品技术白皮书 v3.2.pdf', size: 4718592 },
];

const LIGHT = { resources: RESOURCES, storage: { uiTheme: 'light' } };
const DARK = { resources: RESOURCES, storage: { uiTheme: 'dark' } };

/* ── 公共动作片段 ── */
const OPEN_PANEL = `
  const b = document.getElementById('idm-next-res-btn');
  if (b) b.click();
`;
const unfocus = (idx) => `
  const items = [...document.querySelectorAll('.idm-res-item')];
  ${idx.map(i => `items[${i}] && items[${i}].querySelector('.idm-res-cb').click();`).join('\n  ')}
`;
const OPEN_CAPTURE = `
  window.__IDM_SEND__({ action: 'showCaptureDialog', resource: ${JSON.stringify(VIDEO_SITE)} });
`;
const HOVER_VIDEO = `
  const v = document.getElementById('v');
  const r = v.getBoundingClientRect();
  v.dispatchEvent(new MouseEvent('mousemove', {
      bubbles: true, clientX: r.left + 60, clientY: r.top + 60,
  }));
`;

/* ⚠️ act / after 必须分开：点悬浮球后 openResPanel 是 chrome.runtime.sendMessage
   的回调里才渲染面板的（mock 里也是 setTimeout），同一个同步表达式里紧接着去查
   .idm-res-item 只会拿到空数组 —— 之前 7 条资源一条都没被取消勾选就是这个原因。 */
const SCEN = [
    {
        name: 'content-panel-light', desc: '资源面板 · 亮色（默认全选）',
        data: LIGHT, act: OPEN_PANEL,
        settle: '#idm-next-res-panel',
    },
    {
        name: 'content-panel-dark', desc: '资源面板 · 暗色（取消 3 项 → 计数与按钮文案联动）',
        data: DARK, act: OPEN_PANEL, settle: '#idm-next-res-panel',
        after: unfocus([0, 2, 4]),
    },
    {
        name: 'content-panel-search', desc: '资源面板 · 搜索过滤（只留音频 2 条，提交范围也随之收敛）',
        data: DARK, act: OPEN_PANEL, settle: '#idm-next-res-panel',
        after: `
            const s = document.querySelector('.idm-res-search input');
            s.value = 'audio';
            s.dispatchEvent(new Event('input', { bubbles: true }));
        `,
    },
    {
        name: 'content-panel-empty', desc: '资源面板 · 空状态（暗色，下载键应禁用）',
        data: { resources: [], storage: { uiTheme: 'dark' } }, act: OPEN_PANEL,
        settle: '#idm-next-res-panel',
    },
    {
        name: 'content-capture-light', desc: '下载确认卡片 · 亮色（视频站点 → 画质已填充）',
        data: LIGHT, act: OPEN_CAPTURE,
        settle: '#idm-next-capture',
    },
    {
        name: 'content-capture-dark', desc: '下载确认卡片 · 暗色（画质查询失败降级）',
        data: { ...DARK, formatsFailure: true }, act: OPEN_CAPTURE,
        settle: '#idm-next-capture',
    },
    {
        name: 'content-float-toast', desc: '悬停下载按钮 + 错误 toast（暗色）',
        data: DARK,
        act: HOVER_VIDEO + `
            window.__IDM_SEND__({ action: 'showToast', message: '下载失败：无法连接到服务器', type: 'error' });
        `,
        settle: '#idm-next-res-btn',
    },
];

const fileUrl = 'file:///' + HARNESS.replace(/\\/g, '/');

/* 等页面脚本把注入节点建出来（悬浮球是最先出现的那个） */
async function waitFor(sel, tries = 40) {
    for (let i = 0; i < tries; i++) {
        const r = await send('Runtime.evaluate', {
            expression: `!!document.querySelector(${JSON.stringify(sel)})`,
            returnByValue: true,
        });
        if (r.result.value) return true;
        await sleep(120);
    }
    return false;
}

/* 探测：把「该看见什么」变成可断言的数字/字符串，而不是靠肉眼看图。
   ⚠️ layout 这一段是重点：模型这边看不了 PNG，所以「有没有溢出、有没有换行把按钮挤出去」
   只能靠几何量出来 —— 弹窗那次就是靠这个才发现 footer 文案换行把按钮顶出边界的。 */
const PROBE = `(() => {
    const cs = getComputedStyle(document.documentElement);
    const q = (s) => document.querySelector(s);
    const txt = (s) => { const e = q(s); return e ? e.textContent.trim() : null; };
    const panel = q('#idm-next-res-panel');
    const items = [...document.querySelectorAll('.idm-res-item')];
    const btn = q('#idm-next-res-btn');
    const cap = q('#idm-next-capture');
    const toast = q('.idm-next-toast');
    const fl = q('.idm-next-float-btn');
    const csOf = (e) => e ? getComputedStyle(e) : null;
    const rct = (e) => { if (!e) return null; const r = e.getBoundingClientRect();
        return { x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width),
                 h: Math.round(r.height), right: Math.round(r.right), bottom: Math.round(r.bottom) }; };
    // scrollWidth > clientWidth 即为横向溢出；子元素右边界超出容器右边界即为被挤出
    const ovf = (e) => e ? (e.scrollWidth - e.clientWidth) : null;
    const outside = (host, kids) => {
        if (!host) return null;
        const hr = host.getBoundingClientRect();
        return [...host.querySelectorAll(kids)].map(k => {
            const r = k.getBoundingClientRect();
            return Math.round(Math.max(0, r.right - hr.right)) + Math.round(Math.max(0, r.bottom - hr.bottom));
        });
    };
    return JSON.stringify({
        themeAttr: document.documentElement.getAttribute('data-idm-theme'),
        tokenBg: cs.getPropertyValue('--idm-bg').trim(),
        tokenAccent: cs.getPropertyValue('--idm-accent').trim(),
        overflow: {
            panel: ovf(panel), capture: ovf(cap), toast: ovf(toast),
            foot: panel ? ovf(q('.idm-res-foot')) : null,
            list: panel ? ovf(q('.idm-res-list')) : null,
        },
        // 页脚里的按钮/计数右边界是否越界（>0 就是被挤出去了）
        footOutside: panel ? outside(q('.idm-res-foot'), 'button,.idm-res-selall,.idm-res-picked') : null,
        captureOutside: cap ? outside(cap, 'button,.idm-cap-row input,.idm-cap-row select') : null,
        rects: { panel: rct(panel), capture: rct(cap), toast: rct(toast), float: rct(fl),
                 list: panel ? rct(q('.idm-res-list')) : null },
        /* 图标「真的画出来了」吗？只数 .idm-i 元素个数是不够的 ——
           <use> 引用不到 symbol 时元素照样存在，只是渲染成空白。
           getBBox() 在解析成功时返回 symbol 内容的包围盒，失败时是 0×0，
           所以这里量的是「画出来多大」，不是「有没有这个标签」。 */
        iconPaint: [...document.querySelectorAll('.idm-i use')]
            .slice(0, 8)
            .map(u => { try { const b = u.getBBox();
                return Math.round(b.width) + 'x' + Math.round(b.height); } catch (e) { return 'ERR'; } }),
        spriteSymbols: document.querySelectorAll('#idm-next-sprite symbol').length,
        // 悬浮球
        float: {
            text: btn ? btn.textContent.trim() : null,
            bg: btn ? csOf(btn).backgroundColor : null,
            badge: btn && q('#idm-next-res-btn .n').classList.contains('on')
                 ? q('#idm-next-res-btn .n').textContent : null,
        },
        // 资源面板
        panel: panel ? {
            bg: csOf(panel).backgroundColor,
            head: txt('.idm-res-head .hd-t'),
            count: txt('.idm-res-count'),
            hasSearch: !!q('.idm-res-search input'),
            total: items.length,
            visible: items.filter(i => !i.hidden).length,
            checked: items.filter(i => i.querySelector('.idm-res-cb').checked).length,
            tiles: document.querySelectorAll('.idm-res-tile .idm-i').length,
            picked: txt('.idm-res-picked'),
            selAll: txt('.idm-res-selall'),
            down: q('.idm-res-down') ? q('.idm-res-down').textContent.trim() : null,
            queue: q('.idm-res-queue') ? q('.idm-res-queue').textContent.trim() : null,
            downDisabled: q('.idm-res-down') ? q('.idm-res-down').disabled : null,
            queueDisabled: q('.idm-res-queue') ? q('.idm-res-queue').disabled : null,
            empty: !!q('.idm-res-empty'),
        } : null,
        // 确认卡片
        capture: cap ? {
            bg: csOf(cap).backgroundColor,
            title: txt('.idm-cap-head .hd-t'),
            labels: [...cap.querySelectorAll('.idm-cap-actions button')].map(b => b.textContent.trim()),
            formatOptions: [...cap.querySelectorAll('.idm-cap-fmt option')].map(o => o.textContent.trim()),
            icons: cap.querySelectorAll('.idm-i').length,
        } : null,
        // 悬停按钮 / toast
        floatBtn: fl ? { text: fl.textContent.trim(), shown: fl.classList.contains('show'),
                         bg: csOf(fl).backgroundColor } : null,
        toast: toast ? { text: toast.textContent.trim(), cls: toast.className,
                         bg: csOf(toast).backgroundColor } : null,
    });
})()`;

const report = [];
for (const s of SCEN) {
    consoleErrors = [];
    await send('Emulation.setDeviceMetricsOverride',
        { width: 1000, height: 700, deviceScaleFactor: 1.5, mobile: false });
    const id = (await send('Page.addScriptToEvaluateOnNewDocument', {
        source: `window.__SHOT__ = ${JSON.stringify(s.data)};\n`,
    })).identifier;

    // file:// 页面第二次导航可能命中缓存，用 reload 语义强制重跑内容脚本
    await send('Page.navigate', { url: fileUrl + '?s=' + encodeURIComponent(s.name) });
    await waitFor('#idm-next-res-btn');
    await sleep(150);
    await send('Runtime.evaluate', { expression: s.act });
    await waitFor(s.settle);
    if (s.after) {
        await sleep(160);   // 等异步渲染真的落进 DOM 再操作它
        await send('Runtime.evaluate', { expression: s.after });
    }
    await sleep(420);   // 留给过渡动画落定

    const probe = (await send('Runtime.evaluate', { expression: PROBE, returnByValue: true })).result.value;
    const shot = await send('Page.captureScreenshot', { format: 'png' });
    const file = path.join(OUT, s.name + '.png');
    fs.writeFileSync(file, Buffer.from(shot.data, 'base64'));
    await send('Page.removeScriptToEvaluateOnNewDocument', { identifier: id });

    report.push({ name: s.name, desc: s.desc, probe: JSON.parse(probe), errors: [...consoleErrors], file });
    console.log(`\n[${s.name}] ${s.desc}`);
    console.log('   ' + probe);
    if (consoleErrors.length) console.log('   ⚠️ console: ' + consoleErrors.join(' | '));
}

fs.writeFileSync(path.join(OUT, 'content-report.json'), JSON.stringify(report, null, 2));
console.log(`\n截图 ${report.length} 张 → ${OUT}`);
ws.close();
