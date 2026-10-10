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

// For HTTP(S) only the pathname contains the file type. Query parameters
// such as `?next=video.mp4` are not evidence that a text file is a video.
// Keep opaque/non-HTTP schemes intact (e.g. magnet: and ed2k:).
function urlPathForPattern(url) {
    try {
        const parsed = new URL(url);
        if (parsed.protocol === 'http:' || parsed.protocol === 'https:')
            return parsed.pathname;
    } catch { /* use original for nonstandard links */ }
    return url;
}

// ── 资源存储 ──────────────────────────────────
// A network URL event, its response headers, and the content script may arrive
// simultaneously. storage.session read/modify/write is NOT atomic: serialize by tab.
const tabResourceWrites = new Map();
function withTabWrite(tabId, change) {
    const prev = tabResourceWrites.get(tabId) || Promise.resolve();
    const next = prev.catch(() => {}).then(change);
    tabResourceWrites.set(tabId, next);
    next.finally(() => {
        if (tabResourceWrites.get(tabId) === next) tabResourceWrites.delete(tabId);
    }).catch(() => {});
    return next;
}

async function addResource(tabId, resource) {
    if (!Number.isSafeInteger(tabId) || tabId < 0 ||
        !resource || typeof resource.url !== 'string' || !resource.url) return;
    return withTabWrite(tabId, async () => {
        const key = `tab_${tabId}`;
        const result = await chrome.storage.session.get(key);
        const list = result[key] || [];
        const existing = list.find(r => r.url === resource.url);
        const source = resource.source || 'dom';
        if (existing) {
            let changed = false;
            if (resource.size > 0 && !(existing.size > 0)) {
                existing.size = resource.size;
                changed = true;
            }
            if (resource.mimeType && !existing.mimeType) {
                existing.mimeType = resource.mimeType;
                changed = true;
            }
            const sources = existing.sources || [existing.source || 'unknown'];
            if (!sources.includes(source)) {
                sources.push(source);
                existing.sources = sources;
                changed = true;
            }
            if (changed) await chrome.storage.session.set({ [key]: list });
            return;
        }
        list.unshift({ ...resource, source, sources: [source] });
        if (list.length > MAX_RESOURCES_PER_TAB) list.length = MAX_RESOURCES_PER_TAB;
        await chrome.storage.session.set({ [key]: list });
        try {
            await chrome.action.setBadgeText({ tabId, text: String(list.length) });
            await chrome.action.setBadgeBackgroundColor({ tabId, color: '#2563eb' });
        } catch {
            // The tab may have closed between resource discovery and badge update.
        }
    });
}

async function getResources(tabId) {
    const pending = tabResourceWrites.get(tabId);
    if (pending) await pending.catch(() => {});
    const key = `tab_${tabId}`;
    const result = await chrome.storage.session.get(key);
    return result[key] || [];
}

async function clearResources(tabId) {
    return withTabWrite(tabId, async () => {
        await chrome.storage.session.remove(`tab_${tabId}`);
        try { await chrome.action.setBadgeText({ tabId, text: '' }); } catch {}
    });
}

/**
 * 推断资源类型
 */
function inferType(url) {
    url = urlPathForPattern(url);
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
        const matched = FILE_PATTERNS.some(pattern =>
            pattern.test(urlPathForPattern(url)));
        if (!matched) return;

        addResource(details.tabId, {
            url,
            type: inferType(url),
            filename: inferFilename(url),
            size: -1,
            timestamp: Date.now(),
            initiator: details.initiator || '',
            source: 'webRequest'
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

        // Same URL updates MIME/size, or inserts if the onBeforeRequest
        // event was missed. Always pass through the serialized dedup layer.
        const url = details.url;
        addResource(details.tabId, {
            url,
            type: ct.startsWith('video/') ? 'video' :
                  ct.startsWith('audio/') ? 'audio' : 'file',
            filename: inferFilename(url),
            size: contentLength,
            mimeType: ct,
            timestamp: Date.now(),
            initiator: details.initiator || '',
            source: 'webRequest-headers'
        });
    },
    { urls: ['<all_urls>'] },
    ['responseHeaders']
);

// ── 接管浏览器默认下载（chrome.downloads API）──
// 默认保守：资源发现保持开启，但自动接管必须用户主动启用；确认仍默认开启。
chrome.runtime.onInstalled.addListener(() => {
    chrome.storage.local.get(['interceptEnabled', 'confirmEnabled'], (r) => {
        if (r.interceptEnabled === undefined)
            chrome.storage.local.set({ interceptEnabled: false });
        if (r.confirmEnabled === undefined)
            chrome.storage.local.set({ confirmEnabled: true });
    });
});

/**
 * 读取“接管浏览器下载”设置（默认关闭，用户主动开启）
 * @returns {Promise<{enabled: boolean}>}
 */
async function getInterceptSettings() {
    const r = await chrome.storage.local.get(['interceptEnabled']);
    return { enabled: r.interceptEnabled === true }; // 缺省视为关闭，避免未经确认转移下载所有权
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
    return INTERCEPT_PATTERNS.some(p => p.test(name) || p.test(urlPathForPattern(url)));
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

// Only verifiable task IDs may supersede a browser download. A native
// success without a task ID (e.g. CLI fallback) can never cancel the original.
let handoffJournalWrite = Promise.resolve();
function trustedPopupSender(sender) {
    return sender && (sender.url === chrome.runtime.getURL('popup.html') ||
        (!sender.tab && sender.id === chrome.runtime.id));
}
function rememberHandoff(item, taskId, sessionId) {
    handoffJournalWrite = handoffJournalWrite.catch(() => {}).then(async () => {
        const stored = await chrome.storage.local.get('recentHandoffs');
        const prior = Array.isArray(stored.recentHandoffs) ? stored.recentHandoffs : [];
        const filename = (item.filename || '').split(/[\\/]/).pop() || inferFilename(item.url);
        const record = {
            taskId, sessionId, browserDownloadId: item.id,
            filename: filename.slice(0, 180), at: Date.now()
        };
        const recentHandoffs = [record, ...prior.filter(x => x.taskId !== taskId)].slice(0, 12);
        await chrome.storage.local.set({ recentHandoffs });
        return true;
    });
    return handoffJournalWrite.catch(err => {
        console.warn('[IDM Next] 无法持久化接管记录；浏览器原下载将保留:', err);
        return false;
    });
}

// 浏览器下载属于用户已有的后备通道。桌面端明确接收前，绝不能取消/erase。
// 注意“任务已添加”不是“文件已经完成”；失败任务仍需在桌面端可见且可重试。
// 若用户在确认卡片里拒绝/关闭，浏览器原下载继续，不会丢失。
async function retireBrowserDownloadAfterAccept(item, result) {
    flashBadge(!!(result && result.success && result.tracked));
    if (!result || !result.success) return false;
    // A positive task ID is required for later status lookup. CLI fallback
    // currently returns no ID; preserve browser download in that case.
    if (!Number.isSafeInteger(result.taskId) || result.taskId <= 0 ||
        typeof result.sessionId !== 'string' || !result.sessionId) return false;
    if (!await rememberHandoff(item, result.taskId, result.sessionId)) return false;
    try {
        // 如果浏览器已经下载完成，保留已完成文件；宁可有两份，也不删除数据。
        const matches = await chrome.downloads.search({ id: item.id });
        const live = matches.find(x => x.id === item.id && x.url === item.url);
        if (!live || live.state !== 'in_progress') return false;
        const canceled = await new Promise(resolve => {
            chrome.downloads.cancel(item.id, () => resolve(!chrome.runtime.lastError));
        });
        if (!canceled) return false;
        // Deliberately keep the canceled entry in browser history. Users need
        // a visible fallback when the desktop task later fails or disappears.
        return true;
    } catch (e) {
        console.warn('[IDM Next] 已接收任务，但浏览器原下载未能取消:', e);
        return false;
    }
}

// 匹配下载时，先征询/提交给桌面端；只有接受后才转移所有权。
chrome.downloads.onCreated.addListener(async (item) => {
    const settings = await getInterceptSettings();
    if (!shouldIntercept(item, settings)) return;
    const filename = (item.filename || '').split(/[\\/]/).pop() || inferFilename(item.url);
    const confirm = await getConfirmSettings();

    // Chrome downloads.DownloadItem does not provide tabId. An undefined ID is
    // NOT a valid tab and must never be used to guess which page to interrupt.
    // Keep the browser download; users can send resources manually from popup.
    if (confirm.enabled && (!Number.isSafeInteger(item.tabId) || item.tabId < 0)) {
        console.warn('[IDM Next] 浏览器下载事件不含可靠标签页 ID，保留浏览器下载；可从扩展弹窗手动发送');
        flashBadge(false);
        return;
    }
    if (confirm.enabled) {
        // 确认卡片关闭/拒绝时浏览器原下载保持原样。
        chrome.tabs.sendMessage(item.tabId, {
            action: 'showCaptureDialog',
            resource: {
                url: item.url,
                filename,
                browserDownloadId: item.id,
                type: inferType(item.url),
                size: (item.fileSize && item.fileSize > 0) ? item.fileSize : -1,
                initiator: item.url
            }
        }, (resp) => {
            if (chrome.runtime.lastError || !resp || resp.ok !== true) {
                // 用户要求确认，但此页面无法显示卡片：不绕过确认直接下载。
                // 浏览器原下载继续；用户仍可从 popup/右键菜单手动发送。
                console.warn('[IDM Next] 此页面无法显示接管确认卡片，保留浏览器原下载');
                flashBadge(false);
            }
        });
    } else {
        sendToIdmNext(item.url, filename).then(result =>
            retireBrowserDownloadAfterAccept(item, result));
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
                    const taskId = Number.isSafeInteger(response.taskId) && response.taskId > 0
                        ? response.taskId : null;
                    const sessionId = typeof response.sessionId === 'string' &&
                        /^[a-f0-9-]{32,36}$/i.test(response.sessionId) ? response.sessionId : null;
                    if (!taskId || !sessionId) {
                        // Some CLI fallback paths cannot return a persistent ID.
                        resolve({ success: true, taskId: null, tracked: false,
                            message: '桌面端报告已接收，但没有有效任务身份；浏览器原下载会保留' });
                    } else {
                        // Also cover manual resource sends, not just takeovers.
                        rememberHandoff({ id: null, filename, url }, taskId, sessionId).then(tracked =>
                            resolve({ success: true, taskId, sessionId, tracked,
                                message: tracked ? (response.message || '已添加到 IDM Next')
                                    : '任务已接收，但扩展无法保存查询记录；请在桌面端查看' }));
                    }
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

    // chrome.downloads.DownloadItem 没有 tabId：popup 明确列出仍在进行的下载，
    // 由用户亲自选择是否转交。不要猜测 activeTab 并自动取消他人的任务。
    if (msg.action === 'getBrowserDownloads') {
        chrome.downloads.search({ state: 'in_progress' }).then(items => {
            const eligible = items.filter(item => shouldIntercept(item, { enabled: true }));
            sendResponse({ success: true, items: eligible.slice(0, 20).map(item => ({
                id: item.id, filename: item.filename || inferFilename(item.url),
                url: item.url, size: item.totalBytes > 0 ? item.totalBytes : -1
            })) });
        }).catch(e => sendResponse({ success: false, items: [], message: String(e) }));
        return true;
    }

    if (msg.action === 'takeoverBrowserDownload') {
        // 执行只能来自扩展自己的 popup，不接受网页内容脚本指定要取消的 ID。
        // The toolbar popup may have no sender.tab. The E2E harness (and a user
        // opening popup.html in an extension-owned tab) DO have sender.tab.
        // Authenticate either the exact extension popup URL, or the extension's
        // own no-tab sender. A webpage content script has a different URL.
        const trustedPopup = trustedPopupSender(sender);
        if (!trustedPopup || !Number.isSafeInteger(msg.id) || msg.id < 0) {
            sendResponse({ success: false, message: '无效的浏览器下载接管请求' });
            return false;
        }
        chrome.downloads.search({ id: msg.id }).then(async items => {
            const item = items.find(x => x.id === msg.id && x.state === 'in_progress' &&
                                             shouldIntercept(x, { enabled: true }));
            if (!item) {
                sendResponse({ success: false, message: '原下载已结束或不符合接管条件' });
                return;
            }
            const filename = (item.filename || '').split(/[\\/]/).pop() || inferFilename(item.url);
            const result = await sendToIdmNext(item.url, filename);
            const canceled = result.success ? await retireBrowserDownloadAfterAccept(item, result) : false;
            sendResponse({
                success: !!result.success,
                accepted: !!result.success,
                taskId: result.taskId || null,
                browserCanceled: canceled,
                message: result.success
                    ? (canceled ? 'IDM Next 已接收，浏览器原下载已取消'
                                : 'IDM Next 已接收；浏览器下载仍在进行或已完成，请检查重复文件')
                    : (result.message || '桌面端未接收；浏览器会继续下载')
            });
        }).catch(e => sendResponse({ success: false, message: String(e) }));
        return true;
    }

    if (msg.action === 'getRecentHandoffs') {
        if (!trustedPopupSender(sender)) {
            sendResponse({ success: false, items: [] }); return false;
        }
        chrome.storage.local.get('recentHandoffs').then(data =>
            sendResponse({ success: true,
                items: Array.isArray(data.recentHandoffs) ? data.recentHandoffs.slice(0, 5) : [] })
        ).catch(e => sendResponse({ success: false, items: [], message: String(e) }));
        return true;
    }
    if (msg.action === 'getHandoffStatus') {
        if (!trustedPopupSender(sender) || !Number.isSafeInteger(msg.taskId) || msg.taskId <= 0) {
            sendResponse({ success: false, message: '无效的任务状态请求' });
            return false;
        }
        chrome.storage.local.get('recentHandoffs').then(data => {
            const records = Array.isArray(data.recentHandoffs) ? data.recentHandoffs : [];
            const record = records.find(x => x.taskId === msg.taskId);
            if (!record || typeof record.sessionId !== 'string' || !record.sessionId) {
                sendResponse({ success: false, message: '任务身份不完整，不能确认状态' });
                return;
            }
            chrome.runtime.sendNativeMessage(NATIVE_HOST,
                { action: 'get_task_status', taskId: msg.taskId,
                  sessionId: record.sessionId }, response => {
                    if (chrome.runtime.lastError) {
                        sendResponse({ success: false, message: '桌面端无法查询：' + chrome.runtime.lastError.message });
                    } else if (response && response.success) {
                        const verified = response.taskId === msg.taskId &&
                            response.sessionId === record.sessionId;
                        sendResponse(verified ? response :
                            { success: false, message: '桌面端任务身份与接管记录不一致' });
                    } else {
                        sendResponse(response && response.success === false ? response :
                            { success: false, message: '桌面端未返回有效任务状态' });
                    }
                });
        }).catch(e => sendResponse({ success: false, message: String(e) }));
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
                    initiator: sender.tab.url || '',
                    source: item.source || 'dom'
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
        }).then(async result => {
            // 只有自动接管确认卡片携带浏览器下载 ID；手动 popup 不触及浏览器任务。
            if (Number.isSafeInteger(msg.browserDownloadId) && sender.tab && sender.tab.id >= 0) {
                try {
                    const matches = await chrome.downloads.search({ id: msg.browserDownloadId });
                    const original = matches.find(x =>
                        x.id === msg.browserDownloadId &&
                        x.url === msg.url &&
                        x.tabId === sender.tab.id);
                    if (original) await retireBrowserDownloadAfterAccept(original, result);
                } catch (e) {
                    console.warn('[IDM Next] 浏览器原下载保持不变:', e);
                }
            }
            sendResponse(result);
        });
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
