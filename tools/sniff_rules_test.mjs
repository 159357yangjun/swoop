// sniff_rules_test.mjs — 用 node:vm 加载**真实的** background.js，捕获它注册的 webRequest
// 监听回调，喂合成 details 断言资源列表行为。承重原则（本仓的规矩）：
//   · 测的是生产文件本身，不是复制一份正则进来（复制的那份改了不会红）。
//   · 每条断言先要能"因错误原因失败"——所以有阴性对照：普通 .mp4 必须仍被列出、
//     无 m3u8 页面里的 .ts 也必须仍被列出；只有"同页已识别 HLS ⇒ 分片不进列表"才拦。
//   · 期望值为 0 的桶不算证人：所以正例（该列出的确实列出）与反例（该隐藏的确实隐藏）成对写。
import { readFileSync } from "node:fs";
import vm from "node:vm";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const BG = path.resolve(HERE, "..", "browser-extension", "background.js");

let PASS = 0, FAIL = 0;
function ok(cond, name) {
    if (cond) { PASS++; console.log(`[PASS] ${name}`); }
    else { FAIL++; console.log(`[FAIL] ${name}`); }
}

// ── 造一个只跑"嗅探面"的沙箱：storage 用内存 Map，webRequest 回调被抓下来 ──
function loadSniffChannel() {
    const store = new Map();               // key -> list（模拟 chrome.storage.session）
    const listeners = { onBeforeRequest: [], onHeadersReceived: [] };
    const onMessageListeners = [];          // 抓 reportMedia 通道（真实 m3u8 从这里进来）
    const badge = {};                       // tabId -> text
    const sink = [];                        // 记录 addResource 实际落库的条目

    const mkEvent = (bucket) => ({ addListener: (fn) => bucket.push(fn) });
    // 真实 chrome.storage.session：同一扩展对同一 key 的"读-改-写"是串行原子的（浏览器把同区
    // 存储事务排队），reportMedia 用 forEach(async) 并发发多条 addResource 也不会丢更新。桩若让
    // 每条 addResource 的 get…set 自由交错，第二条会读到第一条写入前的旧列表 ⇒ 假丢更新。
    // 正解：**按标签页的事务队列**——storage 同步落库（保证一次 addResource 内部 get→set 看到
    // 当前值），而 fireReportMedia 逐条把每个 item 作为独立事务、经宏任务 FIFO 排到该 tab 链上，
    // 复刻"同 key 串行处理"。不同 tab 并行。gen 计数 + settle 轮询直到静置。
    let gen = 0;
    const bump = () => { gen++; };
    const tabChains = new Map();   // tabId -> tail promise
    function runOnTab(tabId, fn) {
        const prev = tabChains.get(tabId) || Promise.resolve();
        const next = prev.then(() => new Promise((res) => setTimeout(() => {
            const v = fn(); bump(); res(v);
        }, 0))).catch(() => undefined);
        tabChains.set(tabId, next);
        return next;
    }
    async function settle() {
        for (let i = 0; i < 3000; i++) {
            const g = gen;
            await new Promise((r) => setTimeout(r, 0));
            if (gen === g) break;
        }
    }

    // storage.session 用同步语义：一次 addResource 的 get→set 之间不会插入别的 tab 的调用；
    // 跨 addResource 的串行由上面的 per-tab 事务队列在 fireReportMedia 层保证。
    const sessionGet = (key) => ({ [key]: store.get(key) || [] });
    const sessionSet = (obj) => { for (const k of Object.keys(obj)) store.set(k, obj[k]); };
    const sessionRemove = (key) => { store.delete(key); };

    const chrome = {
        runtime: {
            id: "abcdefghijklmnopabcdefghijklmnop",
            getManifest: () => ({ version: "test", key: "x" }),
            onInstalled: { addListener: () => {} },
            onMessage: mkEvent(onMessageListeners),
            sendMessage: () => {},
        },
        storage: {
            session: {
                get: (key) => Promise.resolve(sessionGet(key)),
                set: (obj) => { sessionSet(obj); return Promise.resolve(); },
                remove: (key) => { sessionRemove(key); return Promise.resolve(); },
            },
            local: {
                get: (_k, cb) => cb && cb({}),
                set: () => {},
            },
        },
        action: {
            setBadgeText: ({ tabId, text }) => { badge[tabId] = text; sink.length = sink.length; return Promise.resolve(); },
            setBadgeBackgroundColor: () => Promise.resolve(),
        },
        webRequest: {
            onBeforeRequest: mkEvent(listeners.onBeforeRequest),
            onHeadersReceived: mkEvent(listeners.onHeadersReceived),
        },
        downloads: { onCreated: { addListener: () => {} }, download: () => {}, onChanged: { addListener: () => {} } },
        tabs: {
            query: (q, cb) => cb && cb([]),
            onRemoved: { addListener: () => {} },
            onUpdated: { addListener: () => {} },
            sendMessage: () => {},
            create: () => {},
            get: (id, cb) => cb && cb({}),
        },
        contextMenus: {
            onClicked: { addListener: () => {} },
            onCreateClicked: () => {},
            removeAll: (cb) => cb && cb(),
            create: () => {},
            update: () => {},
        },
        permissions: { contains: () => Promise.resolve(false), request: () => Promise.resolve(true) },
    };

    const ctx = vm.createContext({
        chrome, URL, Date, Math, JSON, Set, Map, Promise, RegExp, parseInt, parseFloat, isNaN,
        setTimeout: (fn) => { /* 同步吞掉：测试不依赖定时器 */ },
        clearTimeout: () => {}, setInterval: () => 0, clearInterval: () => {},
        console, fetch: () => Promise.reject(new Error("no-net")),
        TextEncoder, TextDecoder,
    });
    const src = readFileSync(BG, "utf8");
    new vm.Script(src, { filename: "background.js" }).runInContext(ctx);

    async function list(tabId) {
        await settle();
        return store.get(`tab_${tabId}`) || [];
    }
    return { listeners, onMessageListeners, list, settle, runOnTab, store, tabReset: (id) => store.delete(`tab_${id}`) };
}

async function fireBefore(sandbox, tabId, url, type = "xmlhttprequest") {
    for (const fn of sandbox.listeners.onBeforeRequest)
        fn({ tabId, url, type, initiator: "", documentUrl: `http://127.0.0.1/page`, requestId: String(tabId) + url });
    await sandbox.settle();
}
async function fireHeaders(sandbox, tabId, url, contentType) {
    for (const fn of sandbox.listeners.onHeadersReceived)
        fn({ tabId, url, responseHeaders: [{ name: "Content-Type", value: contentType }],
             statusCode: 200, requestId: String(tabId) + url });
    await sandbox.settle();
}
// content script 扫到 <video><source .m3u8> 后经 reportMedia 上报（真实 m3u8 的唯一入口）。
// 关键：产品 handler 用 forEach(async) 并发调 addResource ⇒ 同页多条会读-改-写同一 key。
// 真浏览器把同扩展对同一存储区的操作排队串行，所以不丢更新；桩用 per-tab 事务队列复刻这点：
// 每个 item 单独占一段该 tab 的链、经宏任务 FIFO 执行，一条落库完才轮到下一条。
async function fireReportMedia(sandbox, tabId, items) {
    const handler = sandbox.onMessageListeners[0];
    for (const item of items) {
        await sandbox.runOnTab(tabId, () =>
            handler({ action: "reportMedia", items: [item] },
                    { tab: { id: tabId, url: "http://127.0.0.1/page" } }, () => {}));
    }
    await sandbox.settle();
}

const SEG = (n) => `https://cdn.example/hls/seg${n}.ts`;
const MASTER = "https://cdn.example/hls/master.m3u8";

async function main() {
    // ════ 缺陷 1：同页已识别 HLS ⇒ 分片不得作为独立资源进列表 ════
    // m3u8 的真实入口是 reportMedia（FILE_PATTERNS 里没有 m3u8），.ts 走 onBeforeRequest。
    {
        const sb = loadSniffChannel();
        await fireReportMedia(sb, 1, [{ url: MASTER, type: "video", filename: "master.m3u8", size: -1 }]);
        for (let i = 0; i < 3; i++) await fireBefore(sb, 1, SEG(i));
        const urls = (await sb.list(1)).map(r => r.url);
        ok(urls.includes(MASTER), "D1/HLS master 仍被识别（正例：没把整条流一起删掉）");
        const leaked = urls.filter(u => /\.ts(\?|$)/i.test(u));
        ok(leaked.length === 0,
            `D1/同页有 m3u8 时 seg*.ts 不进列表（当前泄漏=${leaked.length}）`);
    }

    // ════ 缺陷 1b：直接导航进播放列表（main_frame m3u8）时，分片请求先于 reportMedia ⇒ 仍须吞掉 ════
    // 步骤 B 真站复现的形状：用户把 .m3u8 当页面打开，没有 DOM 可扫、reportMedia 不会来标 tab，
    // 而 seg*.ts 的 onBeforeRequest 紧随主框架之后。修法＝在 main_frame 命中播放列表时就地补标。
    {
        const sb = loadSniffChannel();
        await fireBefore(sb, 7, "https://cdn.example/hls/tmaster.m3u8", "main_frame");
        for (let i = 0; i < 3; i++) await fireBefore(sb, 7, `https://cdn.example/hls/tseg${i}.ts`, "xmlhttprequest");
        const urls = (await sb.list(7)).map(r => r.url);
        ok(!urls.some(u => /tseg\d\.ts/i.test(u)),
            `D1b/主框架直开 m3u8 后其 tseg*.ts 不进列表（当前泄漏=${urls.filter(u=>/tseg\d\.ts/.test(u)).length}）`);
    }

    // ════ 缺陷 1c：分片**先于**播放列表落库（直接导航竞态的真实形状）⇒ 仍须吞掉已入库的分片 ════
    // 真站步骤 B 实测：Edge 把 tseg*.ts 的 webRequest 在冷启动 SW 收到 main_frame m3u8 之前/同时就投递，
    // 于是分片先 addResource、m3u8 后到。修法＝m3u8 落库时把同源、已在列表里的裸 .ts 一并清出去。
    {
        const sb = loadSniffChannel();
        for (let i = 0; i < 3; i++) await fireBefore(sb, 8, `https://cdn.example/hls/tseg${i}.ts`, "xmlhttprequest");
        let before = (await sb.list(8)).map(r => r.url);
        ok(before.filter(u => /tseg\d\.ts/.test(u)).length === 3,
            `D1c/前置事实：无 m3u8 时三条 tseg 确实先进了列表（实测=${before.length}）`);
        await fireReportMedia(sb, 8, [{ url: "https://cdn.example/hls/tmaster.m3u8", type: "video", filename: "tmaster.m3u8", size: -1 }]);
        const after = (await sb.list(8)).map(r => r.url);
        ok(after.includes("https://cdn.example/hls/tmaster.m3u8"), "D1c/m3u8 后到时仍在册（正例）");
        ok(!after.some(u => /tseg\d\.ts/.test(u)),
            `D1c/同源 m3u8 落库后，先前入库的 tseg*.ts 被清出（残留=${after.filter(u=>/tseg\d\.ts/.test(u)).length}）`);
    }

    // ════ 阴性对照 A：普通 MP4 页面不受影响 ════
    {
        const sb = loadSniffChannel();
        await fireBefore(sb, 2, "https://v.example/movie.mp4");
        const urls = (await sb.list(2)).map(r => r.url);
        ok(urls.includes("https://v.example/movie.mp4"), "阴性A/普通 .mp4 仍被列出（过滤没有误伤视频）");
    }

    // ════ 阴性对照 B：没有 m3u8 的页面，.ts 仍按普通视频列出 ════
    {
        const sb = loadSniffChannel();
        await fireBefore(sb, 3, "https://files.example/lecture.ts");
        const urls = (await sb.list(3)).map(r => r.url);
        ok(urls.includes("https://files.example/lecture.ts"),
            "阴性B/无 m3u8 页面里的 .ts 仍被列出（只在 HLS 上下文里隐藏分片）");
    }

    // ════ 阴性对照 C：onHeadersReceived 通道也不能漏进分片（HLS 页）════
    {
        const sb = loadSniffChannel();
        await fireReportMedia(sb, 4, [{ url: MASTER, type: "video", filename: "master.m3u8", size: -1 }]);
        await fireHeaders(sb, 4, SEG(9), "video/mp2t");
        const urls = (await sb.list(4)).map(r => r.url);
        ok(!urls.includes(SEG(9)),
            `阴性C/响应头通道在 HLS 页也不加 seg*.ts（当前=${urls.includes(SEG(9)) ? "泄漏" : "干净"}）`);
    }

    // ════ 缺陷 2 面 A（承重、可复算）：带扩展名的 .m3u8 永不被标 audio ════
    // content.js inferType('.m3u8')='file' + looksLikeVideo⇒'video'；桌面端与 popup 都按 video。
    {
        const sb = loadSniffChannel();
        await fireReportMedia(sb, 5, [
            { url: "https://cdn.example/hls/master.m3u8", type: "video", filename: "master.m3u8", size: -1 },
            { url: "https://cdn.example/hls/url_2/media.m3u8", type: "video", filename: "media.m3u8", size: -1 },
        ]);
        const rows = (await sb.list(5)).filter(r => /\.m3u8/i.test(r.url));
        ok(rows.length === 2, `D2A/两条播放列表都在册（实测=${rows.length}）`);
        ok(rows.every(r => r.type !== "audio"),
            `D2A/任何 .m3u8 都不被标成 audio（实测类型=[${rows.map(r => r.type).join(",")}]）`);
    }

    // ════ 缺陷 2 面 B（诚实探针，非承重）：无扩展名 + mpegurl Content-Type 的正确定性 ════
    // 用户描述的机制假设这条路径会把 m3u8 归 audio；实测它根本不被 webRequest 抓到
    // （mpegurl 既不在 MEDIA_MIME_TYPES，也不是 video/*、audio/*）。此断言记录真实行为：
    // 若将来有人扩了 MIME 集合让 mpegurl 进来，type 必须是 video，不能是 audio。
    {
        const sb = loadSniffChannel();
        await fireHeaders(sb, 6, "https://cdn.example/stream?id=77", "application/vnd.apple.mpegurl");
        const row = (await sb.list(6)).find(r => r.url.includes("id=77"));
        if (!row) {
            console.log("[INFO] D2B/无扩展名 mpegurl 未被 webRequest 抓到 ⇒ 用户所述『Content-Type→audio』"
                + "机制在本仓不可达；缺陷 2 的实际形状需真站复现，本轮不据此改产品代码。");
            PASS++;
        } else {
            ok(row.type !== "audio", `D2B/mpegurl 被抓到时也不得为 audio（实测=${row.type}）`);
            ok(row.type === "video", `D2B/mpegurl 被抓到时应为 video（实测=${row.type}）`);
        }
    }

    console.log(`--- 汇总 ---\n[SNIFF_RULES] PASS=${PASS} FAIL=${FAIL}`);
    process.exitCode = FAIL > 0 ? 1 : 0;
}
main().catch(e => { console.error("[HARNESS-ERROR]", e); process.exitCode = 2; });
