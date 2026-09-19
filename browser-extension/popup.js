/* ============================================================================
 * popup.js — 扩展弹窗逻辑
 *
 * 职责：展示当前标签页嗅探到的资源 → 让用户挑 → 发到 IDM Next 桌面端。
 *
 * 与旧版（v0.2）的差别：
 *   · 主题：跟随系统 / 亮 / 暗，选择持久化（storage: uiTheme）
 *   · 搜索 + 排序 + 类型筛选（筛选标签带计数）
 *   · 多选（含 Shift 连选）+ 全选 → 批量下载 / 批量入队
 *   · 每行操作收成图标（复制链接 / 选择路径），hover 才出现
 *   · 常驻状态栏 → 弹窗内轻提示 toast
 *   · 三个开关收进设置弹层（原来占三行，把资源列表挤到很窄）
 *
 * ⚠️ 与 background.js 的消息协议保持不变，不要单方面改名：
 *   getResources / downloadWithOptions / batchDownload / checkConnection /
 *   getIntercept / setIntercept / getConfirm / setConfirm / clearResources
 * ========================================================================= */

'use strict';

const $ = (id) => document.getElementById(id);

/* ── 类型元数据：图标 + 中文名 + 颜色类。与桌面端文件类型语义色一致 ── */
const TYPE_META = {
    video:    { name: '视频',   icon: '#i-video',   cls: 't-video' },
    audio:    { name: '音频',   icon: '#i-audio',   cls: 't-audio' },
    image:    { name: '图片',   icon: '#i-image',   cls: 't-image' },
    magnet:   { name: '磁力',   icon: '#i-magnet',  cls: 't-magnet' },
    torrent:  { name: '种子',   icon: '#i-magnet',  cls: 't-magnet' },
    archive:  { name: '压缩包', icon: '#i-archive', cls: 't-archive' },
    app:      { name: '程序',   icon: '#i-app',     cls: 't-app' },
    document: { name: '文档',   icon: '#i-doc',     cls: 't-document' },
    file:     { name: '文件',   icon: '#i-file',    cls: 't-file' },
};
const NON_MEDIA = ['magnet', 'torrent', 'archive', 'app', 'document', 'file'];

/* 筛选标签。'file' 是「上面三类媒体之外的全部」，与旧版行为一致 */
const FILTERS = [
    { key: 'all',   label: '全部' },
    { key: 'video', label: '视频' },
    { key: 'audio', label: '音频' },
    { key: 'image', label: '图片' },
    { key: 'file',  label: '文件' },
];

/* ── 状态 ── */
let allResources = [];          // 原始列表（background 已去重、新的在前）
let filterKey = 'all';
let query = '';
let sortKey = 'default';
const selected = new Set();     // 选中的 URL
const sent = new Set();         // 本次会话内已发送成功的 URL
let lastClickIdx = -1;          // Shift 连选的锚点（相对于当前可见列表）
let visible = [];               // 当前可见（筛选+搜索+排序后）的条目
let themeMode = 'auto';
let toastTimer = null;

/* ── 与 background 通信 ───────────────────────────────────────────── */
function send(msg, cb) {
    if (typeof chrome === 'undefined' || !chrome.runtime || !chrome.runtime.sendMessage) {
        if (cb) cb(null);
        return;
    }
    chrome.runtime.sendMessage(msg, (resp) => {
        // 扩展刚重载 / service worker 未唤醒时会报 lastError；读一下把它消费掉，
        // 否则控制台每个动作都刷一条红字。callback 里拿 undefined 走失败分支即可。
        if (chrome.runtime.lastError) { if (cb) cb(null); return; }
        if (cb) cb(resp);
    });
}

function sendToTab(tabId, msg, cb) {
    chrome.tabs.sendMessage(tabId, msg, () => {
        if (chrome.runtime.lastError) { if (cb) cb(false); return; }
        if (cb) cb(true);
    });
}

function activeTab(cb) {
    chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => cb(tabs && tabs[0]));
}

/* ── 工具 ── */
function fmtSize(bytes) {
    if (!bytes || bytes <= 0) return '';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1048576) return (bytes / 1024).toFixed(0) + ' KB';
    if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + ' MB';
    return (bytes / 1073741824).toFixed(2) + ' GB';
}

function hostOf(url) {
    try { return new URL(url).host; } catch { return ''; }
}

function extOf(name) {
    const m = /\.([a-z0-9]{1,6})(?:$|[?#])/i.exec(name || '');
    return m ? m[1].toLowerCase() : '';
}

function groupOf(res) {
    return FILTERS.some(f => f.key === res.type) ? res.type : 'file';
}

function svgIcon(href, extraClass) {
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('class', 'i' + (extraClass ? ' ' + extraClass : ''));
    const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    use.setAttribute('href', href);
    svg.appendChild(use);
    return svg;
}

/* ── 可见列表计算 ─────────────────────────────────────────────────── */
function computeVisible() {
    const q = query.trim().toLowerCase();
    let list = allResources;

    if (q) {
        list = list.filter(r =>
            (r.filename || '').toLowerCase().includes(q) ||
            (r.url || '').toLowerCase().includes(q));
    }
    if (filterKey !== 'all') {
        list = filterKey === 'file'
            ? list.filter(r => !['video', 'audio', 'image'].includes(r.type))
            : list.filter(r => r.type === filterKey);
    }

    const arr = list.slice();
    if (sortKey === 'size') {
        arr.sort((a, b) => (b.size || -1) - (a.size || -1));
    } else if (sortKey === 'name') {
        arr.sort((a, b) => (a.filename || a.url).localeCompare(b.filename || b.url, 'zh'));
    } else if (sortKey === 'type') {
        arr.sort((a, b) => groupOf(a).localeCompare(groupOf(b)) ||
                           (b.size || -1) - (a.size || -1));
    }
    return arr;
}

/* 计数：只受搜索词影响、不受类型筛选影响 —— 这样每个标签上的数字始终是
   「搜到的东西里该类型有几个」，筛选切换时不会互相抵消到看不出含义。 */
function countByGroup() {
    const q = query.trim().toLowerCase();
    const base = q
        ? allResources.filter(r =>
              (r.filename || '').toLowerCase().includes(q) ||
              (r.url || '').toLowerCase().includes(q))
        : allResources;
    const c = { all: base.length, video: 0, audio: 0, image: 0, file: 0 };
    base.forEach(r => {
        const g = groupOf(r);
        if (c[g] === undefined) c.file++;
        else c[g]++;
    });
    return c;
}

/* ── 渲染 ─────────────────────────────────────────────────────────── */
function renderChips() {
    const counts = countByGroup();
    const host = $('chips');
    host.textContent = '';
    FILTERS.forEach(f => {
        const n = counts[f.key] || 0;
        const btn = document.createElement('button');
        btn.className = 'chip' + (filterKey === f.key ? ' active' : '') + (n === 0 ? ' zero' : '');
        btn.dataset.filter = f.key;
        btn.appendChild(document.createTextNode(f.label));
        const b = document.createElement('b');
        b.textContent = n;
        btn.appendChild(b);
        btn.addEventListener('click', () => { filterKey = f.key; render(); });
        host.appendChild(btn);
    });
}

function emptyState(title, hint, icon) {
    const box = document.createElement('div');
    box.className = 'empty';
    box.appendChild(svgIcon(icon || '#i-inbox'));
    const t = document.createElement('div');
    t.className = 'empty-title';
    t.textContent = title;
    box.appendChild(t);
    if (hint) {
        const h = document.createElement('div');
        h.className = 'empty-hint';
        h.textContent = hint;
        box.appendChild(h);
    }
    return box;
}

function render() {
    visible = computeVisible();
    renderChips();

    const listHost = $('resourceList');
    listHost.textContent = '';

    if (visible.length === 0) {
        if (allResources.length === 0) {
            listHost.appendChild(emptyState('还没有嗅探到资源',
                '播放页面里的视频，或打开含下载链接的页面'));
        } else if (query) {
            listHost.appendChild(emptyState('没有匹配的资源',
                '换个关键词试试，或清空搜索框', '#i-search'));
        } else {
            listHost.appendChild(emptyState('该类型下暂无资源', '', '#i-inbox'));
        }
    } else {
        visible.forEach((res, idx) => listHost.appendChild(buildRow(res, idx)));
    }

    updateFooter();
}

function buildRow(res, idx) {
    const meta = TYPE_META[res.type] || TYPE_META.file;
    const row = document.createElement('div');
    row.className = 'row'
        + (selected.has(res.url) ? ' sel' : '')
        + (sent.has(res.url) ? ' sent' : '');

    const box = document.createElement('input');
    box.type = 'checkbox';
    box.checked = selected.has(res.url);
    row.appendChild(box);

    const ico = document.createElement('span');
    ico.className = 'tico ' + meta.cls;
    ico.appendChild(svgIcon(meta.icon));
    row.appendChild(ico);

    const main = document.createElement('span');
    main.className = 'row-main';
    const name = document.createElement('span');
    name.className = 'row-name';
    name.textContent = res.filename || '(未命名)';
    name.title = res.filename || res.url;
    main.appendChild(name);

    const sub = document.createElement('span');
    sub.className = 'row-sub';
    const host = hostOf(res.url) || res.initiator || '';
    if (sent.has(res.url)) {
        const ok = document.createElement('span');
        ok.className = 'badge-ok';
        ok.appendChild(svgIcon('#i-check'));
        ok.appendChild(document.createTextNode('已发送'));
        sub.appendChild(ok);
        const sep = document.createElement('span');
        sep.className = 'sep';
        sep.textContent = '·';
        sub.appendChild(sep);
    }
    if (host) {
        const h = document.createElement('span');
        h.className = 'host';
        h.textContent = host;
        sub.appendChild(h);
    }
    const sizeText = fmtSize(res.size);
    if (sizeText) {
        const s = document.createElement('span');
        s.textContent = '· ' + sizeText;
        sub.appendChild(s);
    }
    const ext = extOf(res.filename || res.url);
    if (ext) {
        const e = document.createElement('span');
        e.textContent = '· ' + ext.toUpperCase();
        sub.appendChild(e);
    }
    main.appendChild(sub);
    row.appendChild(main);

    // 行内操作：hover 才出现，避免列表看起来全是按钮
    const ops = document.createElement('span');
    ops.className = 'row-ops';

    const copyBtn = document.createElement('button');
    copyBtn.className = 'ibtn';
    copyBtn.title = '复制下载链接';
    copyBtn.appendChild(svgIcon('#i-copy'));
    copyBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        copyText(res.url, '链接已复制');
    });
    ops.appendChild(copyBtn);

    const pathBtn = document.createElement('button');
    pathBtn.className = 'ibtn';
    pathBtn.title = '选择保存路径 / 画质（在页面上弹出卡片）';
    pathBtn.appendChild(svgIcon('#i-folder'));
    pathBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        openCaptureFor(res);
    });
    ops.appendChild(pathBtn);
    row.appendChild(ops);

    // 行点击 = 勾选/取消（Shift 连选）。旧版是"点行直接下载"，
    // 那样很容易误触：想看一眼详情就把文件发出去了。
    row.addEventListener('click', (e) => {
        if (e.target === box) { onToggle(res, idx, e.shiftKey); return; }
        onToggle(res, idx, e.shiftKey);
    });
    row.addEventListener('dblclick', () => downloadOne(res));

    return row;
}

function onToggle(res, idx, shift) {
    if (shift && lastClickIdx >= 0 && lastClickIdx !== idx) {
        const lo = Math.min(lastClickIdx, idx), hi = Math.max(lastClickIdx, idx);
        const want = !selected.has(res.url);
        for (let i = lo; i <= hi; i++) {
            const u = visible[i].url;
            if (want) selected.add(u); else selected.delete(u);
        }
    } else if (selected.has(res.url)) {
        selected.delete(res.url);
    } else {
        selected.add(res.url);
    }
    lastClickIdx = idx;
    render();
}

function updateFooter() {
    const n = visible.filter(r => selected.has(r.url)).length;
    $('selCount').textContent = n > 0 ? `已选 ${n} 项` : `共 ${visible.length} 项`;

    const allBox = $('selectAll');
    allBox.checked = visible.length > 0 && n === visible.length;
    allBox.indeterminate = n > 0 && n < visible.length;

    const dBtn = $('downloadSelBtn'), qBtn = $('queueSelBtn');
    const target = n > 0 ? n : visible.length;
    dBtn.disabled = target === 0;
    qBtn.disabled = target === 0;
    // 按钮文案直接说明「按下去会发生什么」：没勾选 → 全部下载；勾了 → 下载这 N 项。
    // 靠"没勾选时按钮是灰的"来暗示做不到的批量操作，用户是猜不到的。
    // 未选中时不带数字：总数就在左边的「共 N 项」里，写两遍会把 400px 的底栏挤到换行。
    dBtn.querySelector('.lbl').textContent = n > 0 ? `下载 (${n})` : '全部下载';
    qBtn.querySelector('.lbl').textContent = n > 0 ? `队列 (${n})` : '全部入队';
}

/* ── 提示 ─────────────────────────────────────────────────────────── */
function showToast(msg, type) {
    const el = $('toast');
    el.textContent = '';
    el.className = 'toast show' + (type ? ' ' + type : '');
    el.appendChild(svgIcon(type === 'err' ? '#i-alert' : '#i-check'));
    el.appendChild(document.createTextNode(msg));
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { el.className = 'toast'; }, 2800);
}

function copyText(text, okMsg) {
    const done = () => showToast(okMsg || '已复制', 'ok');
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(done).catch(() => fallbackCopy(text, done));
    } else {
        fallbackCopy(text, done);
    }
}
function fallbackCopy(text, done) {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); done(); } catch { showToast('复制失败', 'err'); }
    document.body.removeChild(ta);
}

/* 页面上也提示一次：用户点了下载之后多半马上切回页面，
   只在弹窗里提示的话他已经看不到了。 */
function pageToast(msg, type) {
    activeTab((tab) => {
        if (!tab) return;
        sendToTab(tab.id, { action: 'showToast', message: msg, type: type || 'success', url: '' });
    });
}

/* ── 发送下载 ─────────────────────────────────────────────────────── */
function targets() {
    const picked = visible.filter(r => selected.has(r.url));
    return picked.length > 0 ? picked : visible;
}

function downloadOne(res) {
    send({ action: 'downloadWithOptions', url: res.url, filename: res.filename || '', saveDir: '', queued: queuedByDefault },
        (resp) => {
            const ok = !!(resp && resp.success);
            const msg = (resp && resp.message) || (ok ? '已添加到 IDM Next' : '发送失败');
            if (ok) { sent.add(res.url); render(); }
            showToast(msg, ok ? 'ok' : 'err');
            pageToast(msg, ok ? 'success' : 'error');
        });
}

function sendBatch(queued) {
    const list = targets();
    if (list.length === 0) return;
    const dBtn = $('downloadSelBtn'), qBtn = $('queueSelBtn');
    dBtn.disabled = qBtn.disabled = true;
    showToast(`正在发送 ${list.length} 个资源…`);

    send({
        action: 'batchDownload',
        items: list.map(r => ({ url: r.url, filename: r.filename || '', queued })),
    }, (resp) => {
        const n = (resp && typeof resp.success === 'number') ? resp.success : 0;
        const msg = (resp && resp.message) || (n > 0 ? `已发送 ${n} 个` : '批量发送失败');
        if (n > 0) {
            list.slice(0, n).forEach(r => sent.add(r.url));
            selected.clear();
        }
        render();
        showToast(msg, n > 0 ? 'ok' : 'err');
        pageToast(msg, n > 0 ? 'success' : 'error');
    });
}

function openCaptureFor(res) {
    activeTab((tab) => {
        if (!tab) return;
        sendToTab(tab.id, {
            action: 'showCaptureDialogFor',
            resource: { url: res.url, filename: res.filename || '', type: res.type || 'file', size: res.size || -1 },
        }, (ok) => {
            if (ok) { showToast('已在页面打开选择卡片'); return; }
            // 页面没有 content script（如 chrome:// 页面）：退回直接发送
            showToast('当前页面不支持选择卡片，已直接发送');
            send({ action: 'downloadWithOptions', url: res.url, filename: res.filename || '', saveDir: '', queued: queuedByDefault },
                (resp) => {
                    const ok2 = !!(resp && resp.success);
                    if (ok2) { sent.add(res.url); render(); }
                    showToast((resp && resp.message) || (ok2 ? '已添加' : '发送失败'), ok2 ? 'ok' : 'err');
                });
        });
    });
}

/* ── 连接状态 ─────────────────────────────────────────────────────── */
function checkConnection() {
    const dot = $('connDot'), txt = $('connText'), about = $('aboutConn');
    dot.className = 'dot';
    txt.textContent = '检测中';
    if (about) about.textContent = '检测中…';
    send({ action: 'checkConnection' }, (resp) => {
        const on = !!(resp && resp.connected);
        dot.className = 'dot ' + (on ? 'on' : 'off');
        txt.textContent = on ? '已连接' : '未连接';
        txt.title = on ? 'IDM Next 桌面端已连接'
                       : '未连接：请确认 IDM Next 正在运行（未运行时下载会回退到命令行）';
        if (about) {
            about.textContent = on ? '已连接 · 可发送下载'
                                   : '未连接 · 下载会回退到 idm-next --cli add';
        }
    });
}

/* ── 设置项 ───────────────────────────────────────────────────────── */
let queuedByDefault = false;
let confirmBefore = false;

function loadSettings() {
    send({ action: 'getIntercept' }, (r) => {
        if (r) $('interceptToggle').checked = !!r.enabled;
    });
    send({ action: 'getConfirm' }, (r) => {
        if (r) { confirmBefore = !!r.enabled; $('confirmToggle').checked = confirmBefore; }
    });
    // 队列开关与主题都是本地偏好，直接读 storage
    if (chrome.storage && chrome.storage.local) {
        chrome.storage.local.get(['queueByDefault', 'uiTheme'], (v) => {
            queuedByDefault = !!(v && v.queueByDefault);
            $('queueToggle').checked = queuedByDefault;
            if (v && v.uiTheme) { themeMode = v.uiTheme; applyTheme(); }
        });
    }
}

/* ── 主题 ─────────────────────────────────────────────────────────── */
const mq = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;

function applyTheme() {
    const dark = themeMode === 'dark' || (themeMode === 'auto' && mq && mq.matches);
    document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
    const icon = dark ? '#i-moon' : '#i-sun';
    const btn = $('themeBtn');
    btn.textContent = '';
    btn.appendChild(svgIcon(icon));
    // 图标表示"当前状态"，标题把"点了会怎样"说清楚，避免和设置按钮的图形混淆
    btn.title = themeMode === 'auto' ? '主题：跟随系统（点击切到' + (dark ? '亮色' : '暗色') + '）'
                                    : '主题：' + (dark ? '暗色' : '亮色') + '（点击切换到' + (dark ? '亮色' : '暗色') + '）';
    document.querySelectorAll('#themeSeg button').forEach(b => {
        b.classList.toggle('active', b.dataset.mode === themeMode);
    });
}

function setTheme(mode) {
    themeMode = mode;
    applyTheme();
    if (chrome.storage && chrome.storage.local) chrome.storage.local.set({ uiTheme: mode });
}

/* ── 绑定 ─────────────────────────────────────────────────────────── */
document.addEventListener('DOMContentLoaded', () => {
    // 主题要在首屏就定下来，避免亮→暗闪一下
    if (mq) { applyTheme(); }
    if (chrome.storage && chrome.storage.local) {
        chrome.storage.local.get(['uiTheme'], (v) => {
            if (v && v.uiTheme) { themeMode = v.uiTheme; }
            applyTheme();
        });
    }

    $('themeBtn').addEventListener('click', () => {
        const nowDark = document.documentElement.getAttribute('data-theme') === 'dark';
        setTheme(nowDark ? 'light' : 'dark');
    });

    $('settingsBtn').addEventListener('click', () => $('settingsSheet').classList.add('show'));
    $('closeSheetBtn').addEventListener('click', () => $('settingsSheet').classList.remove('show'));
    $('recheckBtn').addEventListener('click', () => {
        checkConnection();
        showToast('已重新检测连接');
    });

    document.querySelectorAll('#themeSeg button').forEach(b => {
        b.addEventListener('click', () => setTheme(b.dataset.mode));
    });

    const search = $('searchInput');
    search.addEventListener('input', () => {
        query = search.value;
        $('searchWrap').classList.toggle('has-text', !!query);
        selected.clear();
        lastClickIdx = -1;
        render();
    });
    $('searchClear').addEventListener('click', () => {
        search.value = ''; query = '';
        $('searchWrap').classList.remove('has-text');
        render();
        search.focus();
    });
    $('sortSelect').addEventListener('change', (e) => { sortKey = e.target.value; render(); });

    $('selectAll').addEventListener('change', (e) => {
        if (e.target.checked) visible.forEach(r => selected.add(r.url));
        else selected.clear();
        render();
    });

    $('clearBtn').addEventListener('click', () => {
        send({ action: 'clearResources' }, () => {
            allResources = [];
            selected.clear(); sent.clear();
            lastClickIdx = -1;
            render();
            showToast('已清空本页嗅探结果');
        });
    });

    $('downloadSelBtn').addEventListener('click', () => sendBatch(queuedByDefault));
    $('queueSelBtn').addEventListener('click', () => sendBatch(true));

    $('interceptToggle').addEventListener('change', (e) => {
        send({ action: 'setIntercept', enabled: e.target.checked });
        showToast(e.target.checked ? '已开启自动接管浏览器下载' : '已关闭自动接管');
    });
    $('confirmToggle').addEventListener('change', (e) => {
        confirmBefore = e.target.checked;
        send({ action: 'setConfirm', enabled: confirmBefore });
    });
    $('queueToggle').addEventListener('change', (e) => {
        queuedByDefault = e.target.checked;
        if (chrome.storage && chrome.storage.local) chrome.storage.local.set({ queueByDefault: queuedByDefault });
        showToast(queuedByDefault ? '新下载将加入「浏览器」队列' : '新下载将立即开始');
    });

    // 键盘：/ 聚焦搜索、Esc 关闭弹层或清搜索、Ctrl+A 全选
    document.addEventListener('keydown', (e) => {
        const sheetOpen = $('settingsSheet').classList.contains('show');
        if (e.key === '/' && document.activeElement !== search && !sheetOpen) {
            e.preventDefault(); search.focus();
        } else if (e.key === 'Escape') {
            if (sheetOpen) $('settingsSheet').classList.remove('show');
            else if (query) { search.value = ''; query = ''; $('searchWrap').classList.remove('has-text'); render(); }
        } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a' && !sheetOpen) {
            e.preventDefault();
            visible.forEach(r => selected.add(r.url));
            render();
        }
    });

    loadSettings();
    checkConnection();
    loadResources();
});

function loadResources() {
    const host = $('resourceList');
    host.textContent = '';
    host.appendChild(emptyState('加载中…', ''));
    send({ action: 'getResources' }, (resp) => {
        allResources = (resp && resp.resources) || [];
        // 保留仍然存在的选中项，避免刷新后选择被清空
        const urls = new Set(allResources.map(r => r.url));
        [...selected].forEach(u => { if (!urls.has(u)) selected.delete(u); });

        const tabUrlEl = $('tabHost');
        const tabUrl = (resp && resp.tabUrl) || '';
        const host2 = tabUrl ? hostOf(tabUrl) : '';
        tabUrlEl.textContent = host2 || tabUrl || '—';
        tabUrlEl.title = tabUrl;
        render();
    });
}
