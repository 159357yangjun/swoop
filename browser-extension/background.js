/**
 * background.js — MV3 Service Worker
 *
 * 职责：
 * 1. 通过 webRequest API 嗅探网络请求中的媒体/文件资源
 * 2. 通过 onHeadersReceived 检测 Content-Type（捕获 URL 无扩展名的媒体流）
 * 3. 提供右键上下文菜单（右键链接/视频 → 发送到 IDM Next）
 * 4. 按标签页存储嗅探到的资源列表
 * 5. 处理来自 popup / content script 的消息
 * 6. 通过 Native Messaging 将选中资源发送给 IDM Next 桌面端
 * 7. 定期检测与 IDM Next 桌面端的连接状态
 */

// ── 嗅探规则 ──────────────────────────────────
// 按文件扩展名匹配的常见下载/媒体类型
const FILE_PATTERNS = [
    // 视频
    /\.(mp4|mkv|avi|mov|wmv|flv|webm|m4v|mpg|mpeg|ts|m2ts)(\?|$)/i,
    // 音频
    /\.(mp3|flac|aac|ogg|wav|m4a|wma|opus)(\?|$)/i,
    // 压缩包
    /\.(zip|rar|7z|tar|gz|bz2|xz|iso)(\?|$)/i,
    // 程序/文档
    /\.(exe|msi|apk|dmg|pkg|deb|rpm)(\?|$)/i,
    /\.(pdf|doc|docx|xls|xlsx|ppt|pptx|epub)(\?|$)/i,
    // 字幕
    /\.(srt|ass|ssa|vtt|sub)(\?|$)/i,
    // 图片（大图）
    /\.(png|jpg|jpeg|gif|bmp|webp|svg|tiff)(\?|$)/i,
    // 磁力/种子
    /^(magnet:|ed2k:|thunder:)/i,
    /\.torrent(\?|$)/i,
];

// ── 接管浏览器默认下载的拦截规则 ──────────────
// 与 FILE_PATTERNS 类似，但刻意排除图片与网页（避免误伤“查看图片/保存网页”），
// 仅拦截用户真正想“下载到磁盘”的文件类型（对标 IDM 的“自动捕获”行为）。
const INTERCEPT_PATTERNS = [
    /\.(mp4|mkv|avi|mov|wmv|flv|webm|m4v|mpg|mpeg|ts|m2ts)(\?|$)/i,   // 视频
    /\.(mp3|flac|aac|ogg|wav|m4a|wma|opus)(\?|$)/i,                   // 音频
    /\.(zip|rar|7z|tar|gz|bz2|xz|iso)(\?|$)/i,                        // 压缩包
    /\.(exe|msi|apk|dmg|pkg|deb|rpm)(\?|$)/i,                         // 程序
    /\.(pdf|doc|docx|xls|xlsx|ppt|pptx|epub)(\?|$)/i,                 // 文档
    /\.(srt|ass|ssa|vtt|sub)(\?|$)/i,                                 // 字幕
    /^(magnet:|ed2k:|thunder:)/i,                                     // 磁力/ed2k/迅雷
    /\.torrent(\?|$)/i,                                              // 种子
];

// 媒体 MIME 类型（用于 Content-Type 响应头匹配）
const MEDIA_MIME_TYPES = new Set([
    'video/mp4', 'video/webm', 'video/ogg', 'video/x-msvideo',
    'video/x-flv', 'video/quicktime', 'video/x-matroska',
    'audio/mpeg', 'audio/mp3', 'audio/ogg', 'audio/aac',
    'audio/wav', 'audio/x-wav', 'audio/flac', 'audio/x-m4a',
    'application/zip', 'application/x-rar-compressed',
    'application/x-7z-compressed', 'application/pdf',
    'application/octet-stream', 'application/x-iso9660-image',
    'application/x-bittorrent',
]);

// 媒体 MIME 前缀（匹配 video/* 和 audio/*）
const MEDIA_MIME_PREFIXES = ['video/', 'audio/'];

// 嗅探到的资源上限（防止内存爆炸）
const MAX_RESOURCES_PER_TAB = 200;

// ── 资源存储 ──────────────────────────────────

/**
 * 添加嗅探到的资源到标签页列表
 */
async function addResource(tabId, resource) {
    const key = `tab_${tabId}`;
    const result = await chrome.storage.session.get(key);
    let list = result[key] || [];

    // 去重：同一 URL 不重复添加
    if (list.some(r => r.url === resource.url)) return;

    // 如果有新的大小信息，更新已有条目
    const existing = list.find(r => r.url === resource.url);
    if (existing && resource.size > 0 && existing.size < 0) {
        existing.size = resource.size;
        await chrome.storage.session.set({ [key]: list });
        return;
    }

    list.unshift(resource);
    if (list.length > MAX_RESOURCES_PER_TAB)
        list = list.slice(0, MAX_RESOURCES_PER_TAB);

    await chrome.storage.session.set({ [key]: list });

    // 更新扩展图标 badge（显示当前页面嗅探到的资源数）
    await chrome.action.setBadgeText({ tabId, text: String(list.length) });
    await chrome.action.setBadgeBackgroundColor({ tabId, color: '#2563eb' });
}

/**
 * 获取标签页的资源列表
 */
async function getResources(tabId) {
    const key = `tab_${tabId}`;
    const result = await chrome.storage.session.get(key);
    return result[key] || [];
}

/**
 * 清空标签页资源
 */
async function clearResources(tabId) {
    const key = `tab_${tabId}`;
    await chrome.storage.session.remove(key);
    await chrome.action.setBadgeText({ tabId, text: '' });
}

/**
 * 推断资源类型
 */
function inferType(url) {
    if (/^(magnet:|ed2k:|thunder:)/i.test(url)) return 'magnet';
    if (/\.torrent/i.test(url)) return 'torrent';
    if (/\.(mp4|mkv|avi|mov|wmv|flv|webm|m4v|mpg|mpeg|ts|m2ts)/i.test(url)) return 'video';
    if (/\.(mp3|flac|aac|ogg|wav|m4a|wma|opus)/i.test(url)) return 'audio';
    if (/\.(jpg|jpeg|png|gif|bmp|webp|svg|tiff)/i.test(url)) return 'image';
    if (/\.(zip|rar|7z|tar|gz|bz2|xz|iso)/i.test(url)) return 'archive';
    if (/\.(exe|msi|apk|dmg|pkg|deb|rpm)/i.test(url)) return 'app';
    if (/\.(pdf|doc|docx|xls|xlsx|ppt|pptx|epub)/i.test(url)) return 'document';
    return 'file';
}

/**
 * 从 URL 推断文件名
 */
function inferFilename(url) {
    let filename = '';
    try {
        const u = new URL(url);
        filename = u.pathname.split('/').pop() || u.hostname;
        if (filename && filename.indexOf('?') >= 0) filename = filename.split('?')[0];
    } catch {
        filename = url.substring(url.lastIndexOf('/') + 1) || 'unknown';
        if (filename && filename.indexOf('?') >= 0) filename = filename.split('?')[0];
    }
    return filename || 'unknown';
}

// ── webRequest 嗅探 ───────────────────────────

// 监听所有网络请求 URL，匹配文件模式
chrome.webRequest.onBeforeRequest.addListener(
    (details) => {
        if (details.tabId < 0) return;

        const url = details.url;
        const matched = FILE_PATTERNS.some(pattern => pattern.test(url));
        if (!matched) return;

        addResource(details.tabId, {
            url,
            type: inferType(url),
            filename: inferFilename(url),
            size: -1,
            timestamp: Date.now(),
            initiator: details.initiator || ''
        });
    },
    { urls: ['<all_urls>'] }
);

// 监听响应头，通过 Content-Type 检测媒体（捕获 URL 无扩展名的流媒体）
chrome.webRequest.onHeadersReceived.addListener(
    (details) => {
        if (details.tabId < 0) return;

        const headers = details.responseHeaders || [];
        let contentType = '';
        let contentLength = -1;

        for (const h of headers) {
            const name = h.name.toLowerCase();
            if (name === 'content-type') contentType = h.value;
            else if (name === 'content-length') contentLength = parseInt(h.value, 10) || -1;
        }

        if (!contentType) return;

        // 检查是否为媒体类型
        const ct = contentType.split(';')[0].trim().toLowerCase();
        const isMedia = MEDIA_MIME_TYPES.has(ct) ||
                        MEDIA_MIME_PREFIXES.some(prefix => ct.startsWith(prefix));

        if (!isMedia) return;

        // 如果 URL 已在列表中，更新大小信息
        const url = details.url;
        const key = `tab_${details.tabId}`;

        chrome.storage.session.get(key).then(result => {
            let list = result[key] || [];
            const existing = list.find(r => r.url === url);
            if (existing) {
                // 更新大小
                if (contentLength > 0 && existing.size < 0) existing.size = contentLength;
                existing.mimeType = ct;
                chrome.storage.session.set({ [key]: list });
            } else {
                // 新发现：URL 无扩展名但 Content-Type 是媒体
                const alreadyMatched = FILE_PATTERNS.some(p => p.test(url));
                if (alreadyMatched) return; // 已通过 URL 模式添加过

                addResource(details.tabId, {
                    url,
                    type: ct.startsWith('video/') ? 'video' :
                          ct.startsWith('audio/') ? 'audio' : 'file',
                    filename: inferFilename(url),
                    size: contentLength,
                    mimeType: ct,
                    timestamp: Date.now(),
                    initiator: details.initiator || ''
                });
            }
        });
    },
    { urls: ['<all_urls>'] },
    ['responseHeaders']
);

// ── 接管浏览器默认下载（chrome.downloads API）──
// 初始化默认设置：默认开启“自动接管浏览器下载”与“下载前确认”
chrome.runtime.onInstalled.addListener(() => {
    chrome.storage.local.get(['interceptEnabled', 'confirmEnabled'], (r) => {
        if (r.interceptEnabled === undefined)
            chrome.storage.local.set({ interceptEnabled: true });
        if (r.confirmEnabled === undefined)
            chrome.storage.local.set({ confirmEnabled: true });
    });
});

/**
 * 读取“接管浏览器下载”设置（默认开启）
 * @returns {Promise<{enabled: boolean}>}
 */
async function getInterceptSettings() {
    const r = await chrome.storage.local.get(['interceptEnabled']);
    return { enabled: r.interceptEnabled !== false }; // 缺省视为开启
}

/**
 * 读取“下载前确认”设置（默认开启：拦截后在页面内弹出选择卡片）
 * @returns {Promise<{enabled: boolean}>}
 */
async function getConfirmSettings() {
    const r = await chrome.storage.local.get(['confirmEnabled']);
    return { enabled: r.confirmEnabled !== false }; // 缺省视为开启
}

/**
 * 判断 URL 是否属于浏览器/扩展自身内部下载（不应拦截）
 */
function isInternalUrl(url) {
    return /^chrome-extension:\/\//i.test(url) ||
           /^chrome:\/\//i.test(url) ||
           /^edge:\/\//i.test(url) ||
           /^about:/i.test(url) ||
           /^blob:/i.test(url) ||
           /^data:/i.test(url) ||
           /clients2\.google\.com|update\.googleapis\.com|edge\.microsoft\.com/i.test(url);
}

/**
 * 判断是否应当拦截并接管该下载
 */
function shouldIntercept(item, settings) {
    if (!settings.enabled) return false;
    const url = item.url || '';
    if (!url) return false;
    if (isInternalUrl(url)) return false;
    // 本扩展自身发起的下载（如有）不拦截，避免自我循环
    if (item.byExtensionId && item.byExtensionId === chrome.runtime.id) return false;
    // 保存网页（HTML）不应接管
    const mime = (item.mime || '').toLowerCase();
    if (mime === 'text/html' || mime === 'application/xhtml+xml') return false;
    const name = item.filename || url;
    return INTERCEPT_PATTERNS.some(p => p.test(name) || p.test(url));
}

// 全局 badge 闪烁反馈（接管成功/失败），避免与按标签页的资源计数 badge 冲突
let badgeResetTimer = null;
function flashBadge(success) {
    chrome.action.setBadgeText({ text: '↓' });
    chrome.action.setBadgeBackgroundColor({ color: success ? '#10b981' : '#ef4444' });
    if (badgeResetTimer) clearTimeout(badgeResetTimer);
    badgeResetTimer = setTimeout(() => {
        chrome.action.getBadgeText({}, (cur) => {
            if (cur === '↓') chrome.action.setBadgeText({ text: '' });
        });
    }, 1500);
}

// 核心：监听浏览器新建下载，命中规则则取消浏览器默认下载并转发到 IDM Next
chrome.downloads.onCreated.addListener(async (item) => {
    const settings = await getInterceptSettings();
    if (!shouldIntercept(item, settings)) return;

    const filename = (item.filename || '').split(/[\\/]/).pop() || inferFilename(item.url);

    // 取消并抹除浏览器自身的下载（防止重复落盘）
    chrome.downloads.cancel(item.id, () => { void chrome.runtime.lastError; });
    chrome.downloads.erase({ id: item.id }, () => { void chrome.runtime.lastError; });

    // 读取“下载前确认”设置：开启则在页面内弹出选择卡片（多种选择）；
    // 关闭则按原行为静默直接下载（仍无需用户手动添加）。
    const confirm = await getConfirmSettings();
    if (confirm.enabled && item.tabId >= 0) {
        // 尝试在来源标签页注入确认卡片；content script 不可用时回退静默直发
        chrome.tabs.sendMessage(item.tabId, {
            action: 'showCaptureDialog',
            resource: {
                url: item.url,
                filename: filename,
                type: inferType(item.url),
                size: (item.fileSize && item.fileSize > 0) ? item.fileSize : -1,
                initiator: item.url
            }
        }, (resp) => {
            if (chrome.runtime.lastError) {
                // 该页面无 content script（如 chrome://、部分特殊页）→ 静默直发
                sendToIdmNext(item.url, filename).then(r => flashBadge(r.success));
            }
            // 若 content script 成功接管，卡片内的“立即下载/加入队列”会自行发 downloadWithOptions
        });
    } else {
        sendToIdmNext(item.url, filename).then(result => flashBadge(result.success));
    }
});

// ── 右键上下文菜单 ──────────────────────────────
chrome.runtime.onInstalled.addListener(() => {
    // 链接右键 → 发送到 IDM Next
    chrome.contextMenus.create({
        id: 'send-link',
        title: '发送到 IDM Next 下载',
        contexts: ['link']
    });

    // 视频/音频右键 → 发送到 IDM Next
    chrome.contextMenus.create({
        id: 'send-media',
        title: '发送到 IDM Next 下载',
        contexts: ['video', 'audio']
    });

    // 图片右键 → 发送到 IDM Next
    chrome.contextMenus.create({
        id: 'send-image',
        title: '发送到 IDM Next 下载',
        contexts: ['image']
    });

    // 选中文本（可能是 URL）右键 → 发送到 IDM Next
    chrome.contextMenus.create({
        id: 'send-selection',
        title: '发送选中链接到 IDM Next 下载',
        contexts: ['selection']
    });
});

// 处理右键菜单点击
chrome.contextMenus.onClicked.addListener((info, tab) => {
    let url = '';
    let filename = '';

    switch (info.menuItemId) {
        case 'send-link':
            url = info.linkUrl;
            filename = info.linkText || '';
            break;
        case 'send-media':
            url = info.srcUrl || info.pageUrl;
            break;
        case 'send-image':
            url = info.srcUrl;
            break;
        case 'send-selection':
            url = info.selectionText.trim();
            if (!url.match(/^(https?|ftp|magnet):/i)) {
                // 不是链接，忽略
                return;
            }
            break;
        default:
            return;
    }

    if (!url) return;

    sendToIdmNext(url, filename).then(result => {
        // 用 badge 闪烁提示结果
        if (tab && tab.id >= 0) {
            const color = result.success ? '#10b981' : '#ef4444';
            chrome.action.setBadgeBackgroundColor({ tabId: tab.id, color });
            chrome.action.setBadgeText({ tabId: tab.id, text: result.success ? 'OK' : 'ERR' });
            setTimeout(() => {
                chrome.action.getBadgeText({ tabId: tab.id }, () => {
                    // 恢复原 badge
                    getResources(tab.id).then(list => {
                        chrome.action.setBadgeText({
                            tabId: tab.id,
                            text: list.length > 0 ? String(list.length) : ''
                        });
                        chrome.action.setBadgeBackgroundColor({ tabId: tab.id, color: '#2563eb' });
                    });
                });
            }, 2000);
        }

        // 在网页视频下方显示提示（核心需求：提示在前端页面）
        if (tab && tab.id >= 0) {
            chrome.tabs.sendMessage(tab.id, {
                action: 'showToast',
                message: result.success ? (result.message || '已添加到 IDM Next') : (result.message || '下载失败'),
                type: result.success ? 'success' : 'error',
                url: url
            }, () => { void chrome.runtime.lastError; });
        }

        // 发送通知到 popup（如果打开的话）
        chrome.runtime.sendMessage({
            action: 'downloadResult',
            result: result,
            url: url
        }).catch(() => {}); // popup 可能未打开，忽略错误
    });
});

// ── 标签页生命周期 ─────────────────────────────
chrome.tabs.onRemoved.addListener((tabId) => {
    clearResources(tabId);
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo) => {
    if (changeInfo.status === 'loading' && changeInfo.url) {
        clearResources(tabId);
    }
});

// ── Native Messaging ──────────────────────────
const NATIVE_HOST = 'com.tencent.idm_next';

// 浏览器扩展“加入队列”使用的固定队列名（GUI 端不存在时自动创建）
const BROWSER_QUEUE = '浏览器';

// 连接状态缓存
let hostConnected = null; // null=未知, true=已连接, false=未连接
let hostCheckTime = 0;

/**
 * 检测 IDM Next 桌面端是否在运行
 * @returns {Promise<boolean>}
 */
function checkHostConnection() {
    const now = Date.now();
    // 5 秒内不重复检测
    if (hostConnected !== null && now - hostCheckTime < 5000) {
        return Promise.resolve(hostConnected);
    }

    return new Promise((resolve) => {
        try {
            chrome.runtime.sendNativeMessage(NATIVE_HOST, { action: 'ping' }, (response) => {
                if (chrome.runtime.lastError) {
                    hostConnected = false;
                } else {
                    hostConnected = !!(response && response.success);
                }
                hostCheckTime = Date.now();
                resolve(hostConnected);
            });
        } catch {
            hostConnected = false;
            hostCheckTime = Date.now();
            resolve(false);
        }
    });
}

/**
 * 发送下载请求到 IDM Next 桌面端
 * @param {string} url - 下载链接
 * @param {string} filename - 建议文件名
 * @param {object} [opts] - 选项：{ saveDir, queued, format }
 *        saveDir: 自定义保存目录（空=应用默认）
 *        queued:  true 则加入“浏览器”队列而非立即下载
 *        format:  视频画质选择串（yt-dlp -f 选择串，空=最佳画质）
 * @returns {Promise<{success: boolean, message: string}>}
 */
function sendToIdmNext(url, filename = '', opts = {}) {
    return new Promise((resolve) => {
        try {
            chrome.runtime.sendNativeMessage(NATIVE_HOST, {
                action: 'add_download',
                url: url,
                filename: filename,
                saveDir: opts.saveDir || '',
                queue: opts.queued ? BROWSER_QUEUE : '',
                format: opts.format || ''
            }, (response) => {
                if (chrome.runtime.lastError) {
                    resolve({ success: false, message: chrome.runtime.lastError.message });
                } else if (response && response.success) {
                    resolve({ success: true, message: response.message || '已添加到 IDM Next' });
                } else {
                    resolve({ success: false, message: (response && response.message) || 'IDM Next 未响应' });
                }
            });
        } catch (e) {
            resolve({ success: false, message: String(e) });
        }
    });
}

/**
 * 批量发送下载请求
 * @param {Array<{url, filename}>} items
 * @returns {Promise<{success: number, failed: number, message: string}>}
 */
function batchSendToIdmNext(items) {
    return new Promise((resolve) => {
        try {
            chrome.runtime.sendNativeMessage(NATIVE_HOST, {
                action: 'batch_add',
                items: items
            }, (response) => {
                if (chrome.runtime.lastError) {
                    resolve({ success: 0, failed: items.length, message: chrome.runtime.lastError.message });
                } else if (response && response.success) {
                    resolve({
                        success: response.added || items.length,
                        failed: response.failed || 0,
                        message: response.message || `已添加 ${response.added || items.length} 个任务`
                    });
                } else {
                    resolve({ success: 0, failed: items.length, message: (response && response.message) || 'IDM Next 未响应' });
                }
            });
        } catch (e) {
            resolve({ success: 0, failed: items.length, message: String(e) });
        }
    });
}

/**
 * 查询视频 URL 的可选画质/格式列表（经 Native Messaging 转发到桌面端 yt-dlp -J）
 * @param {string} url
 * @returns {Promise<{success: boolean, formats: Array, message: string}>}
 */
function listFormats(url) {
    return new Promise((resolve) => {
        try {
            chrome.runtime.sendNativeMessage(NATIVE_HOST, {
                action: 'list_formats',
                url: url
            }, (response) => {
                if (chrome.runtime.lastError) {
                    resolve({ success: false, message: chrome.runtime.lastError.message, formats: [] });
                } else if (response && response.success) {
                    resolve({ success: true, formats: response.formats || [], message: '' });
                } else {
                    resolve({ success: false, message: (response && response.message) || '未能获取画质列表', formats: [] });
                }
            });
        } catch (e) {
            resolve({ success: false, message: String(e), formats: [] });
        }
    });
}

// ── 消息处理 ──────────────────────────────────
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
    // popup 请求获取当前标签页的资源列表
    if (msg.action === 'getResources') {
        chrome.tabs.query({ active: true, currentWindow: true }, async (tabs) => {
            if (tabs[0]) {
                const list = await getResources(tabs[0].id);
                sendResponse({ resources: list, tabUrl: tabs[0].url });
            } else {
                sendResponse({ resources: [], tabUrl: '' });
            }
        });
        return true;
    }

    // content script 请求查询视频画质/格式列表（经 Native Messaging 转发到桌面端 yt-dlp）
    if (msg.action === 'listFormats') {
        listFormats(msg.url).then(result => sendResponse(result));
        return true;
    }

    // popup / content script 请求发送下载到 IDM Next
    if (msg.action === 'download') {
        sendToIdmNext(msg.url, msg.filename || '').then(result => {
            sendResponse(result);
        });
        return true;
    }

    // popup 请求批量下载
    if (msg.action === 'batchDownload') {
        const items = (msg.items || []).map(r => ({
            url: r.url,
            filename: r.filename || '',
            // 批量下载也支持“加入队列”：转换为 host 的 queue 字段
            queue: r.queued ? BROWSER_QUEUE : '',
            // 视频画质选择（yt-dlp -f 串，空=最佳画质）
            format: r.format || ''
        }));
        batchSendToIdmNext(items).then(result => {
            sendResponse(result);
        });
        return true;
    }

    // popup 请求检测连接状态
    if (msg.action === 'checkConnection') {
        checkHostConnection().then(connected => {
            sendResponse({ connected, version: hostConnected ? '0.2.0' : null });
        });
        return true;
    }

    // content script 报告页面中发现的媒体元素
    if (msg.action === 'reportMedia') {
        if (sender.tab && sender.tab.id >= 0) {
            const items = msg.items || [];
            items.forEach(item => {
                addResource(sender.tab.id, {
                    url: item.url,
                    type: item.type || 'file',
                    filename: item.filename || '',
                    size: item.size || -1,
                    timestamp: Date.now(),
                    initiator: sender.tab.url || ''
                });
            });
        }
        sendResponse({ ok: true });
        return false;
    }

    // 清空当前标签页资源
    if (msg.action === 'clearResources') {
        chrome.tabs.query({ active: true, currentWindow: true }, async (tabs) => {
            if (tabs[0]) await clearResources(tabs[0].id);
            sendResponse({ ok: true });
        });
        return true;
    }

    // popup 请求读取“接管浏览器下载”开关状态
    if (msg.action === 'getIntercept') {
        getInterceptSettings().then(s => sendResponse({ enabled: s.enabled }));
        return true;
    }

    // content script 请求当前标签页的嗅探资源（用于页面内批量勾选面板）
    if (msg.action === 'getResourcesForTab') {
        const tid = (sender.tab && sender.tab.id >= 0) ? sender.tab.id : -1;
        if (tid >= 0) {
            getResources(tid).then(list => sendResponse({ resources: list }));
        } else {
            sendResponse({ resources: [] });
        }
        return true;
    }

    // popup 切换“接管浏览器下载”开关
    if (msg.action === 'setIntercept') {
        chrome.storage.local.set({ interceptEnabled: !!msg.enabled });
        sendResponse({ ok: true });
        return false;
    }

    // popup/页面确认卡片 请求带选项的下载（另存目录 / 加入队列 / 画质）
    if (msg.action === 'downloadWithOptions') {
        sendToIdmNext(msg.url, msg.filename || '', {
            saveDir: msg.saveDir || '',
            queued: !!msg.queued,
            format: msg.format || ''
        }).then(result => sendResponse(result));
        return true;
    }

    // popup 读取“下载前确认”开关
    if (msg.action === 'getConfirm') {
        getConfirmSettings().then(s => sendResponse({ enabled: s.enabled }));
        return true;
    }

    // popup 切换“下载前确认”开关
    if (msg.action === 'setConfirm') {
        chrome.storage.local.set({ confirmEnabled: !!msg.enabled });
        sendResponse({ ok: true });
        return false;
    }
});
