/**
 * content.js — 内容脚本
 *
 * 扫描页面 DOM 中的媒体元素（<video>、<audio>、<a download>、<img>、<object>），
 * 将发现的资源上报给 background service worker。
 * 使用 MutationObserver 监听动态加载的内容。
 */

(function() {
    'use strict';

    const foundUrls = new Set();

    /* ═══════════════════════════════════════════════════════════════════════
     * 设计令牌（与桌面端 resources/qss/{light,dark}.qss、popup.css 同源）
     *
     * 之前这里每块注入样式都各自硬编码粉蓝渐变 + 毛玻璃，和桌面端已经改成的
     * 「中性灰 + 单一强调色 + 小圆角」完全对不上，亮/暗也没法切。现在所有颜色
     * 只走 --idm-* 令牌，暗色由 <html data-idm-theme="dark"> 一个属性切换
     * （CSS 变量会向下继承，注入的浮层都能吃到）。
     *
     * 主题来源与 popup 共用同一个 storage 键 uiTheme = auto|light|dark，
     * 所以在弹窗里切的主题会同步影响页面内面板。
     * ═══════════════════════════════════════════════════════════════════════ */
    const THEME_KEY = 'uiTheme';

    function tokensCSS() {
        return `
        :root{
            --idm-bg:#ffffff; --idm-sunken:#f7f8fa; --idm-hover:#f2f3f5; --idm-active:#eceef1;
            --idm-border:#e6e8eb; --idm-border-strong:#d6dbe1;
            --idm-text:#1f2329; --idm-text-2:#5a6270; --idm-text-3:#8a8f99;
            --idm-accent:#2563eb; --idm-accent-hover:#1d4ed8;
            --idm-accent-soft:#eef2ff; --idm-accent-border:#c7d2fe;
            --idm-ok:#2f9e6f; --idm-warn:#b7791f; --idm-err:#d05252;
            --idm-shadow:0 10px 28px rgba(15,23,42,.16);
            --idm-r:8px; --idm-r-sm:6px;
            --idm-font:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Microsoft YaHei",sans-serif;
        }
        html[data-idm-theme="dark"]{
            --idm-bg:#1a1b1e; --idm-sunken:#202124; --idm-hover:#26272b; --idm-active:#2c2d31;
            --idm-border:#32343a; --idm-border-strong:#3d4046;
            --idm-text:#e8eaed; --idm-text-2:#a8adb6; --idm-text-3:#6b707a;
            --idm-accent:#4c8dff; --idm-accent-hover:#6aa0ff;
            --idm-accent-soft:#22314f; --idm-accent-border:#2f4a78;
            --idm-ok:#3fb27f; --idm-warn:#d0a03f; --idm-err:#e06565;
            --idm-shadow:0 10px 28px rgba(0,0,0,.5);
        }
        /* 所有注入浮层共用的重置：宿主页面的 box-sizing 约定不该改我们的排版。
           刻意只重置 box-sizing 这一条 —— 用 * 去覆盖 width/height 会连带把
           我们自己设的固定尺寸（勾选框 15px、图标块 26px）一起冲掉。
           注意：宿主页面里像 body input{...} 这种 specificity 比单类名高的规则
           仍可能压过我们（除非改成 #id 前缀 + 全量重写），目前按「实际站点上没
           观察到问题」先不做，风险记在 memory。 */
        .idm-next-toast,.idm-next-toast *,
        .idm-next-float-btn,.idm-next-float-btn *,
        .idm-next-capture,.idm-next-capture *,
        .idm-next-res-btn,.idm-next-res-btn *,
        .idm-next-res-panel,.idm-next-res-panel *{box-sizing:border-box;}
        `;
    }

    /* 内联 SVG 图标库：与 popup.html 的 symbol 是同一批路径数据。
       刻意不用 emoji（⬇ × ✓ ⚠）——字形随系统变、且和桌面端那套线性自绘图标
       放一起像两个产品。 */
    function spriteSVG() {
        const S = (id, d) => `<symbol id="idm-i-${id}" viewBox="0 0 24 24">${d}</symbol>`;
        return `<svg aria-hidden="true" style="position:absolute;width:0;height:0;overflow:hidden">`
            + S('download', '<path d="M12 4v10.4"/><path d="M7.8 10.6L12 14.8l4.2-4.2"/><path d="M4.8 18.6h14.4"/>')
            + S('queue', '<path d="M4 6.5h11M4 12h11M4 17.5h7"/><path d="M18 12v7M18 19l2.4-2.4"/><path d="M18 19l-2.4-2.4"/>')
            + S('close', '<path d="M6.6 6.6l10.8 10.8M17.4 6.6L6.6 17.4"/>')
            + S('check', '<path d="M5 12.8l4.4 4.2L19 7.6"/>')
            + S('info', '<circle cx="12" cy="12" r="8.4"/><path d="M12 11v5.2M12 7.9h.01"/>')
            + S('alert', '<circle cx="12" cy="12" r="8.4"/><path d="M12 7.8v5.4M12 16.2h.01"/>')
            + S('search', '<circle cx="11" cy="11" r="7"/><path d="M20 20l-4.2-4.2"/>')
            + S('folder', '<path d="M4 7.4c0-.9.7-1.6 1.6-1.6h3.2l2 2.4h8.6c.9 0 1.6.7 1.6 1.6v7.8c0 .9-.7 1.6-1.6 1.6H5.6A1.6 1.6 0 0 1 4 17.6Z"/>')
            + S('video', '<rect x="3" y="5" width="18" height="14" rx="2.4"/><path d="M10 9.4l5 2.6-5 2.6Z"/>')
            + S('audio', '<path d="M9 17.5V6.2l9-1.7v11"/><circle cx="6.6" cy="17.6" r="2.4"/><circle cx="15.6" cy="15.9" r="2.4"/>')
            + S('image', '<rect x="3" y="4.5" width="18" height="15" rx="2.4"/><circle cx="8.8" cy="10" r="1.6"/><path d="M4 17l4.8-4.4 3.4 3 3-2.6L20 17"/>')
            + S('archive', '<rect x="3.5" y="4" width="17" height="5" rx="1.4"/><path d="M5 9v9.2c0 .8.6 1.4 1.4 1.4h11.2c.8 0 1.4-.6 1.4-1.4V9"/><path d="M10.3 13h3.4"/>')
            + S('app', '<rect x="3.5" y="3.5" width="17" height="17" rx="3"/><path d="M8.5 12h7M12 8.4v7.2"/>')
            + S('doc', '<path d="M6 3.6h7.4L19 9.2v11.2H6Z"/><path d="M13.2 3.6v5.8H19"/><path d="M9 13.5h6M9 16.5h4"/>')
            + S('file', '<path d="M6 3.6h7.4L19 9.2v11.2H6Z"/><path d="M13.2 3.6v5.8H19"/>')
            + S('magnet', '<path d="M6 4.5v7a6 6 0 0 0 12 0v-7"/><path d="M6 9.6h4M14 9.6h4"/>')
            + S('inbox', '<path d="M4 13l2.2-7.2A2 2 0 0 1 8.1 4.4h7.8a2 2 0 0 1 1.9 1.4L20 13v5.2a1.4 1.4 0 0 1-1.4 1.4H5.4A1.4 1.4 0 0 1 4 18.2Z"/><path d="M4 13h4l1 2.4h6L16 13h4"/>')
            + `</svg>`;
    }

    /* 只注入一次：令牌样式表 + 图标 sprite */
    function ensureChrome() {
        if (!document.getElementById('idm-next-tokens')) {
            const st = document.createElement('style');
            st.id = 'idm-next-tokens';
            st.textContent = tokensCSS();
            (document.head || document.documentElement).appendChild(st);
        }
        if (!document.getElementById('idm-next-sprite')) {
            const host = document.createElement('div');
            host.id = 'idm-next-sprite';
            host.setAttribute('aria-hidden', 'true');
            host.style.cssText = 'position:absolute;width:0;height:0;overflow:hidden;pointer-events:none;';
            host.innerHTML = spriteSVG();
            (document.body || document.documentElement).appendChild(host);
        }
    }

    /* 生成一个 <svg><use/></svg>；size 单位 px */
    function ico(name, size, extraClass) {
        // 有些站点会整块替换 document.body.innerHTML，顺手把我们的 sprite 也删掉。
        // 这里按需补一次，否则图标会变成空白（元素还在、但 <use> 引用不到 symbol）。
        if (!document.getElementById('idm-next-sprite')) ensureChrome();
        const ns = 'http://www.w3.org/2000/svg';
        const svg = document.createElementNS(ns, 'svg');
        svg.setAttribute('class', 'idm-i' + (extraClass ? ' ' + extraClass : ''));
        svg.setAttribute('viewBox', '0 0 24 24');
        const s = size || 16;
        svg.style.width = s + 'px';
        svg.style.height = s + 'px';
        const use = document.createElementNS(ns, 'use');
        use.setAttribute('href', '#idm-i-' + name);
        svg.appendChild(use);
        return svg;
    }

    /* 图标通用外观（描边线性、圆头，和桌面端 AppIcons 一致） */
    const ICON_CSS = `
        .idm-i{flex:0 0 auto;stroke:currentColor;fill:none;stroke-width:1.7;
            stroke-linecap:round;stroke-linejoin:round;display:block;}
    `;

    let idmThemeMode = 'auto';
    function resolveTheme() {
        if (idmThemeMode === 'light' || idmThemeMode === 'dark') return idmThemeMode;
        return (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches)
            ? 'dark' : 'light';
    }
    function applyThemeMode() {
        const t = resolveTheme();
        const root = document.documentElement;
        if (t === 'dark') root.setAttribute('data-idm-theme', 'dark');
        else root.removeAttribute('data-idm-theme');
    }
    function initTheme() {
        ensureChrome();
        applyThemeMode();
        try {
            chrome.storage.local.get([THEME_KEY], (r) => {
                idmThemeMode = (r && r[THEME_KEY]) || 'auto';
                applyThemeMode();
            });
            chrome.storage.onChanged.addListener((changes, area) => {
                if (area === 'local' && changes[THEME_KEY]) {
                    idmThemeMode = changes[THEME_KEY].newValue || 'auto';
                    applyThemeMode();
                }
            });
        } catch (e) { /* 无 storage 权限时静默降级为跟随系统 */ }
        if (window.matchMedia) {
            const mq = window.matchMedia('(prefers-color-scheme: dark)');
            const on = () => { if (idmThemeMode === 'auto') applyThemeMode(); };
            if (mq.addEventListener) mq.addEventListener('change', on);
            else if (mq.addListener) mq.addListener(on);
        }
    }
    initTheme();

    /**
     * 上报媒体资源到 background
     */
    function reportMedia(items) {
        if (items.length === 0) return;
        chrome.runtime.sendMessage({ action: 'reportMedia', items: items });
    }

    /**
     * 从 URL 推断文件名（解码 + 清理 query string）
     */
    function inferFilename(url) {
        try {
            const u = new URL(url, window.location.origin);
            let name = u.pathname.split('/').pop();
            if (name) name = decodeURIComponent(name);
            // 去掉 query 参数
            if (name && name.indexOf('?') >= 0) name = name.split('?')[0];
            return name || '';
        } catch {
            return '';
        }
    }

    /**
     * 推断资源类型
     */
    function inferType(url) {
        if (/^(magnet:|ed2k:|thunder:)/i.test(url)) return 'magnet';
        if (/\.torrent(\?|$)/i.test(url)) return 'torrent';
        if (/\.(mp4|mkv|avi|mov|wmv|flv|webm|m4v|mpg|mpeg|ts|m2ts)(\?|$)/i.test(url)) return 'video';
        if (/\.(mp3|flac|aac|ogg|wav|m4a|wma|opus)(\?|$)/i.test(url)) return 'audio';
        if (/\.(jpg|jpeg|png|gif|bmp|webp|svg|tiff)(\?|$)/i.test(url)) return 'image';
        if (/\.(zip|rar|7z|tar|gz|bz2|xz|iso)(\?|$)/i.test(url)) return 'archive';
        if (/\.(exe|msi|apk|dmg|pkg|deb|rpm)(\?|$)/i.test(url)) return 'app';
        if (/\.(pdf|doc|docx|xls|xlsx|ppt|pptx|epub)(\?|$)/i.test(url)) return 'document';
        return 'file';
    }

    // 视频站点域名（与桌面端 VideoDownloader::VIDEO_SITES 对齐）
    const VIDEO_HOSTS = [
        'youtube.com', 'youtu.be', 'bilibili.com', 'b23.tv', 'vimeo.com',
        'dailymotion.com', 'twitch.tv', 'nicovideo.jp', 'tiktok.com',
        'douyin.com', 'instagram.com', 'facebook.com', 'twitter.com',
        'x.com', 'soundcloud.com', 'pinterest.com', 'reddit.com',
        'streamable.com', 'pan.baidu.com'
    ];

    /**
     * 判断 URL 是否为可查询画质的视频站点
     */
    function looksLikeVideo(url) {
        if (!url) return false;
        const u = url.toLowerCase();
        if (VIDEO_HOSTS.some(h => u.indexOf(h) >= 0)) return true;
        if (u.endsWith('.m3u8') || u.endsWith('.mpd')) return true;
        return false;
    }

    /**
     * 扫描 <video> 和 <audio> 元素（含 <source> 子元素）
     */
    function scanMediaElements() {
        const items = [];

        // <video src="..."> 和 <video><source src="..."></video>
        document.querySelectorAll('video').forEach(v => {
            // 检查 src 属性
            if (v.src && !foundUrls.has(v.src)) {
                foundUrls.add(v.src);
                items.push({
                    url: v.src,
                    type: 'video',
                    filename: inferFilename(v.src),
                    size: -1
                });
            }
            // 检查 currentSrc（可能通过 JS 设置）
            if (v.currentSrc && !foundUrls.has(v.currentSrc)) {
                foundUrls.add(v.currentSrc);
                items.push({
                    url: v.currentSrc,
                    type: 'video',
                    filename: inferFilename(v.currentSrc),
                    size: -1
                });
            }
            // 检查 <source> 子元素
            v.querySelectorAll('source').forEach(s => {
                if (s.src && !foundUrls.has(s.src)) {
                    foundUrls.add(s.src);
                    items.push({
                        url: s.src,
                        type: 'video',
                        filename: inferFilename(s.src),
                        size: -1
                    });
                }
            });
        });

        // <audio src="..."> 和 <audio><source src="..."></audio>
        document.querySelectorAll('audio').forEach(a => {
            if (a.src && !foundUrls.has(a.src)) {
                foundUrls.add(a.src);
                items.push({
                    url: a.src,
                    type: 'audio',
                    filename: inferFilename(a.src),
                    size: -1
                });
            }
            if (a.currentSrc && !foundUrls.has(a.currentSrc)) {
                foundUrls.add(a.currentSrc);
                items.push({
                    url: a.currentSrc,
                    type: 'audio',
                    filename: inferFilename(a.currentSrc),
                    size: -1
                });
            }
            a.querySelectorAll('source').forEach(s => {
                if (s.src && !foundUrls.has(s.src)) {
                    foundUrls.add(s.src);
                    items.push({
                        url: s.src,
                        type: 'audio',
                        filename: inferFilename(s.src),
                        size: -1
                    });
                }
            });
        });

        return items;
    }

    /**
     * 扫描带 download 属性的 <a> 标签
     */
    function scanDownloadLinks() {
        const items = [];
        document.querySelectorAll('a[download]').forEach(a => {
            const href = a.href || a.getAttribute('href');
            if (href && !foundUrls.has(href)) {
                foundUrls.add(href);
                const filename = a.getAttribute('download') || inferFilename(href);
                items.push({
                    url: href,
                    type: 'file',
                    filename: filename,
                    size: -1
                });
            }
        });
        return items;
    }

    /**
     * 扫描直链（href 以常见下载扩展名结尾的 <a> 标签）
     */
    function scanDirectLinks() {
        const items = [];
        const pattern = /\.(mp4|mkv|avi|mov|flv|webm|mp3|flac|wav|ogg|zip|rar|7z|exe|apk|pdf|epub|torrent|iso|dmg|msi|deb|rpm|doc|docx|xls|xlsx|ppt|pptx|srt|ass|vtt)(\?|$)/i;

        document.querySelectorAll('a[href]').forEach(a => {
            const href = a.href;
            if (!href || foundUrls.has(href)) return;
            if (!pattern.test(href)) return;

            foundUrls.add(href);
            const text = a.textContent.trim();
            items.push({
                url: href,
                type: inferType(href),
                filename: text || inferFilename(href),
                size: -1
            });
        });
        return items;
    }

    /**
     * 扫描 <img> 大图（仅捕获 > 300px 或 src 含大图特征的图片）
     */
    function scanImages() {
        const items = [];
        document.querySelectorAll('img[src]').forEach(img => {
            const src = img.src;
            if (!src || foundUrls.has(src)) return;

            // 只捕获较大的图片（width > 300 或 naturalWidth > 800）
            const w = img.naturalWidth || img.width || 0;
            if (w < 300) return;

            foundUrls.add(src);
            items.push({
                url: src,
                type: 'image',
                filename: inferFilename(src),
                size: -1
            });
        });
        return items;
    }

    /**
     * 扫描 <object> 和 <embed>（Flash / PDF 等）
     */
    function scanObjects() {
        const items = [];
        document.querySelectorAll('object[data], embed[src]').forEach(el => {
            const url = el.getAttribute('data') || el.getAttribute('src');
            if (url && !foundUrls.has(url)) {
                foundUrls.add(url);
                items.push({
                    url: url,
                    type: inferType(url),
                    filename: inferFilename(url),
                    size: -1
                });
            }
        });
        return items;
    }

    /**
     * 主扫描函数
     */
    function scanAll() {
        const items = [
            ...scanMediaElements(),
            ...scanDownloadLinks(),
            ...scanDirectLinks(),
            ...scanImages(),
            ...scanObjects()
        ];
        if (items.length > 0) {
            reportMedia(items);
        }
        // 发现可下载资源（非纯图片）时，在页面右下角显示“本页资源”批量入口
        const interesting = items.some(it => it.type && it.type !== 'image');
        if (interesting) setResButtonCount(items.filter(it => it.type && it.type !== 'image').length);
    }

    // 页面加载完成后首次扫描
    scanAll();

    // 使用 MutationObserver 监听 DOM 变化，动态加载的视频也能被抓到
    let debounceTimer = null;
    const observer = new MutationObserver(() => {
        if (debounceTimer) clearTimeout(debounceTimer);
        debounceTimer = setTimeout(() => {
            scanAll();
            updateVideoMap();
        }, 800);
    });

    // 确保 document.body 存在
    if (document.body) {
        observer.observe(document.body, {
            childList: true,
            subtree: true
        });
    } else {
        // body 还没就绪，等 DOMContentLoaded
        document.addEventListener('DOMContentLoaded', () => {
            if (document.body) {
                observer.observe(document.body, {
                    childList: true,
                    subtree: true
                });
            }
        });
    }

    // ── 页面内提示（视频下方浮动 toast，玻璃拟态粉蓝主题）────────
    const videoElementMap = new Map();

    function updateVideoMap() {
        videoElementMap.clear();
        document.querySelectorAll('video').forEach(v => {
            const rect = v.getBoundingClientRect();
            if (rect.width < 100 || rect.height < 60) return; // 忽略小图标/缩略图
            if (v.src) videoElementMap.set(v.src, v);
            if (v.currentSrc && v.currentSrc !== v.src) videoElementMap.set(v.currentSrc, v);
            v.querySelectorAll('source').forEach(s => {
                if (s.src) videoElementMap.set(s.src, v);
            });
        });
    }

    // 注入一次样式（中性表面 + 细边框 + 类型着色图标；不再用粉蓝渐变/毛玻璃）
    function ensureToastStyles() {
        if (document.getElementById('idm-next-toast-style')) return;
        ensureChrome();
        const style = document.createElement('style');
        style.id = 'idm-next-toast-style';
        style.textContent = `
        .idm-next-toast{
            position:fixed;z-index:2147483647;max-width:340px;box-sizing:border-box;
            display:flex;align-items:center;gap:10px;padding:11px 13px;border-radius:var(--idm-r);
            color:var(--idm-text);font-size:13px;line-height:1.45;font-family:var(--idm-font);
            background:var(--idm-bg);border:1px solid var(--idm-border);
            box-shadow:var(--idm-shadow);
            pointer-events:auto;opacity:0;transform:translateY(-10px) scale(.99);
            transition:opacity .24s ease,transform .24s cubic-bezier(.2,.8,.2,1);
        }
        .idm-next-toast.show{opacity:1;transform:translateY(0) scale(1);}
        .idm-next-toast .ic{flex:0 0 auto;width:24px;height:24px;display:flex;align-items:center;
            justify-content:center;border-radius:50%;background:var(--idm-accent-soft);
            color:var(--idm-accent);}
        .idm-next-toast.success .ic{background:rgba(47,158,111,.12);color:var(--idm-ok);}
        .idm-next-toast.error .ic{background:rgba(208,82,82,.12);color:var(--idm-err);}
        .idm-next-toast .tx{flex:1 1 auto;word-break:break-word;}
        ${ICON_CSS}
        .idm-next-float-btn{position:fixed;z-index:2147483646;display:none;cursor:pointer;
            align-items:center;gap:6px;padding:7px 12px;border-radius:var(--idm-r);
            color:#fff;font-size:12.5px;font-weight:600;font-family:var(--idm-font);
            background:var(--idm-accent);border:none;box-shadow:0 6px 18px rgba(15,23,42,.22);
            user-select:none;opacity:0;transform:translateY(-6px);
            transition:opacity .2s ease,transform .2s ease,background .15s ease;}
        .idm-next-float-btn.show{display:flex;}
        .idm-next-float-btn:hover{background:var(--idm-accent-hover);}
        `;
        document.head.appendChild(style);
    }

    // 在视频下方（或右上角兜底）显示提示
    function showToast(message, type = 'info', targetRect = null) {
        ensureToastStyles();
        const el = document.createElement('div');
        el.className = 'idm-next-toast ' + (type === 'error' ? 'error' : (type === 'success' ? 'success' : 'info'));

        const ic = document.createElement('div');
        ic.className = 'ic';
        ic.appendChild(ico(type === 'error' ? 'alert' : (type === 'success' ? 'check' : 'info'), 14));
        const tx = document.createElement('div');
        tx.className = 'tx';
        tx.textContent = message;

        el.appendChild(ic);
        el.appendChild(tx);

        // 优先锚定到视频下方（视口坐标）；视频不在视口内则右上角兜底
        if (targetRect) {
            const inView = targetRect.bottom > 0 && targetRect.top < window.innerHeight;
            if (inView) {
                el.style.left = Math.max(12, targetRect.left) + 'px';
                el.style.top = (targetRect.bottom + 12) + 'px';
                el.style.right = 'auto';
            } else {
                el.style.top = '16px';
                el.style.right = '16px';
                el.style.left = 'auto';
            }
        } else {
            el.style.top = '16px';
            el.style.right = '16px';
            el.style.left = 'auto';
        }

        document.body.appendChild(el);
        requestAnimationFrame(() => el.classList.add('show'));

        setTimeout(() => {
            el.classList.remove('show');
            setTimeout(() => el.remove(), 360);
        }, 4200);
    }

    // 向后台发送「下载此视频」请求，并在页面直接给出提示
    function downloadViaExtension(url) {
        if (!url) return;
        chrome.runtime.sendMessage(
            { action: 'download', url: url, filename: inferFilename(url) },
            (response) => {
                const success = !!(response && response.success);
                const message = success ? (response && response.message) || '已添加到 IDM Next'
                                         : (response && response.message) || '下载失败';
                const v = videoElementMap.get(url);
                const r = v ? v.getBoundingClientRect() : null;
                showToast(message, success ? 'success' : 'error',
                          r ? { left: r.left, top: r.top, bottom: r.bottom } : null);
            }
        );
    }

    // ── 下载确认卡片（拦截 / 悬停 / popup 触发，给予多种选择）────────
    function ensureCaptureStyles() {
        if (document.getElementById('idm-next-capture-style')) return;
        ensureChrome();
        const style = document.createElement('style');
        style.id = 'idm-next-capture-style';
        style.textContent = `
        .idm-next-capture{position:fixed;z-index:2147483647;top:18px;left:50%;
            transform:translateX(-50%) translateY(-8px);box-sizing:border-box;width:348px;max-width:92vw;
            padding:13px 14px 12px;border-radius:10px;color:var(--idm-text);
            font-family:var(--idm-font);font-size:13px;background:var(--idm-bg);
            border:1px solid var(--idm-border);box-shadow:var(--idm-shadow);
            opacity:0;transition:opacity .22s ease,transform .22s cubic-bezier(.2,.8,.2,1);}
        .idm-next-capture.show{opacity:1;transform:translateX(-50%) translateY(0);}
        .idm-cap-head{display:flex;align-items:center;gap:7px;
            font-weight:600;font-size:13.5px;color:var(--idm-text);
            margin-bottom:11px;padding-bottom:10px;border-bottom:1px solid var(--idm-border);}
        .idm-cap-head .hd-ic{color:var(--idm-accent);display:flex;}
        .idm-cap-head .hd-t{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
        .idm-cap-close{flex:0 0 auto;display:flex;align-items:center;justify-content:center;
            width:24px;height:24px;border-radius:var(--idm-r-sm);cursor:pointer;
            color:var(--idm-text-3);transition:background .15s ease,color .15s ease;}
        .idm-cap-close:hover{background:var(--idm-hover);color:var(--idm-text);}
        .idm-cap-row{display:flex;align-items:center;gap:8px;margin-bottom:8px;}
        .idm-cap-row label{flex:0 0 48px;color:var(--idm-text-2);font-size:12.5px;}
        .idm-cap-row input,.idm-cap-row select{flex:1 1 auto;min-width:0;padding:6px 8px;
            border-radius:var(--idm-r-sm);border:1px solid var(--idm-border-strong);
            background:var(--idm-sunken);color:var(--idm-text);font-size:12.5px;
            font-family:var(--idm-font);}
        .idm-cap-row input:focus,.idm-cap-row select:focus{outline:none;
            border-color:var(--idm-accent);background:var(--idm-bg);}
        .idm-cap-row select option{background:var(--idm-bg);color:var(--idm-text);}
        .idm-cap-actions{display:flex;gap:7px;margin-top:10px;}
        .idm-cap-actions button{flex:1 1 auto;display:flex;align-items:center;justify-content:center;
            gap:5px;padding:8px 6px;border:1px solid transparent;border-radius:var(--idm-r-sm);
            font-size:12.5px;font-weight:600;cursor:pointer;font-family:var(--idm-font);
            transition:background .15s ease,border-color .15s ease,color .15s ease;}
        .idm-cap-now{background:var(--idm-accent);color:#fff;}
        .idm-cap-now:hover{background:var(--idm-accent-hover);}
        .idm-cap-queue{background:var(--idm-accent-soft);color:var(--idm-accent);
            border-color:var(--idm-accent-border);}
        .idm-cap-queue:hover{border-color:var(--idm-accent);}
        .idm-cap-cancel{background:var(--idm-sunken);color:var(--idm-text-2);
            border-color:var(--idm-border);}
        .idm-cap-cancel:hover{background:var(--idm-hover);color:var(--idm-text);}
        .idm-cap-actions button:disabled{opacity:.55;cursor:default;}
        ${ICON_CSS}
        `;
        document.head.appendChild(style);
    }

    // 显示下载确认卡片；返回创建的 DOM 节点（便于外部移除）
    function showCaptureDialog(resource) {
        if (!resource || !resource.url) return null;
        ensureCaptureStyles();
        // 已存在则先移除，避免叠加
        const old = document.getElementById('idm-next-capture');
        if (old) old.remove();

        const card = document.createElement('div');
        card.id = 'idm-next-capture';
        card.className = 'idm-next-capture';

        const head = document.createElement('div');
        head.className = 'idm-cap-head';
        const hdIc = document.createElement('span');
        hdIc.className = 'hd-ic';
        hdIc.appendChild(ico('download', 15));
        const hdT = document.createElement('span');
        hdT.className = 'hd-t';
        hdT.textContent = '下载任务';
        const close = document.createElement('span');
        close.className = 'idm-cap-close';
        close.title = '关闭';
        close.appendChild(ico('close', 14));
        head.appendChild(hdIc);
        head.appendChild(hdT);
        head.appendChild(close);
        card.appendChild(head);

        const nameRow = document.createElement('div');
        nameRow.className = 'idm-cap-row';
        const nameLabel = document.createElement('label');
        nameLabel.textContent = '文件名';
        const nameInput = document.createElement('input');
        nameInput.className = 'idm-cap-name';
        nameInput.value = resource.filename || '';
        nameRow.appendChild(nameLabel);
        nameRow.appendChild(nameInput);
        card.appendChild(nameRow);

        const dirRow = document.createElement('div');
        dirRow.className = 'idm-cap-row';
        const dirLabel = document.createElement('label');
        dirLabel.textContent = '目录';
        const dirInput = document.createElement('input');
        dirInput.className = 'idm-cap-dir';
        dirInput.placeholder = '留空=默认目录';
        dirRow.appendChild(dirLabel);
        dirRow.appendChild(dirInput);
        card.appendChild(dirRow);

        // 画质选择（仅视频站点查询 yt-dlp 格式列表）
        const fmtRow = document.createElement('div');
        fmtRow.className = 'idm-cap-row';
        const fmtLabel = document.createElement('label');
        fmtLabel.textContent = '画质';
        const fmtSelect = document.createElement('select');
        fmtSelect.className = 'idm-cap-fmt';
        const defOpt = document.createElement('option');
        defOpt.value = '';
        defOpt.textContent = '最佳画质（默认）';
        fmtSelect.appendChild(defOpt);
        fmtRow.appendChild(fmtLabel);
        fmtRow.appendChild(fmtSelect);
        card.appendChild(fmtRow);

        // 视频站点：异步查询画质列表并填充下拉
        if (looksLikeVideo(resource.url)) {
            fmtSelect.disabled = true;
            const loading = document.createElement('option');
            loading.textContent = '查询画质中…';
            loading.disabled = true;
            fmtSelect.appendChild(loading);
            chrome.runtime.sendMessage({ action: 'listFormats', url: resource.url }, (res) => {
                fmtSelect.innerHTML = '';
                const best = document.createElement('option');
                best.value = '';
                best.textContent = '最佳画质（默认）';
                fmtSelect.appendChild(best);
                if (res && res.success && Array.isArray(res.formats) && res.formats.length) {
                    res.formats.forEach((f) => {
                        const opt = document.createElement('option');
                        opt.value = f.fmt || f.id || '';
                        opt.textContent = f.label || opt.value;
                        if (opt.value) fmtSelect.appendChild(opt);
                    });
                } else {
                    const err = document.createElement('option');
                    err.textContent = '画质不可用';
                    err.disabled = true;
                    fmtSelect.appendChild(err);
                }
                fmtSelect.disabled = false;
            });
        }

        const actions = document.createElement('div');
        actions.className = 'idm-cap-actions';
        const btnNow = document.createElement('button');
        btnNow.className = 'idm-cap-now';
        btnNow.appendChild(ico('download', 14));
        const nowLbl = document.createElement('span');
        nowLbl.textContent = '立即下载';
        btnNow.appendChild(nowLbl);
        const btnQueue = document.createElement('button');
        btnQueue.className = 'idm-cap-queue';
        btnQueue.appendChild(ico('queue', 14));
        const qLbl = document.createElement('span');
        qLbl.textContent = '加入队列';
        btnQueue.appendChild(qLbl);
        const btnCancel = document.createElement('button');
        btnCancel.className = 'idm-cap-cancel';
        btnCancel.textContent = '取消';
        actions.appendChild(btnNow);
        actions.appendChild(btnQueue);
        actions.appendChild(btnCancel);
        card.appendChild(actions);

        document.body.appendChild(card);
        requestAnimationFrame(() => card.classList.add('show'));

        function remove() { card.classList.remove('show'); setTimeout(() => card.remove(), 220); }
        function submit(queued) {
            const fn = nameInput.value.trim();
            const dir = dirInput.value.trim();
            const fmt = fmtSelect.value;
            btnNow.disabled = btnQueue.disabled = true;
            chrome.runtime.sendMessage({
                action: 'downloadWithOptions',
                url: resource.url,
                filename: fn,
                saveDir: dir,
                format: fmt,
                queued: queued
            }, (response) => {
                const success = !!(response && response.success);
                const message = success ? (response && response.message) || '已添加到 IDM Next'
                                         : (response && response.message) || '下载失败';
                showToast(message, success ? 'success' : 'error');
                remove();
            });
        }

        close.addEventListener('click', remove);
        btnCancel.addEventListener('click', remove);
        btnNow.addEventListener('click', () => submit(false));
        btnQueue.addEventListener('click', () => submit(true));
        return card;
    }

    // 由悬停按钮触发：针对某个视频/媒体 URL 打开确认卡片
    function openCaptureForUrl(url) {
        if (!url) return;
        showCaptureDialog({ url: url, filename: inferFilename(url), type: 'video', size: -1 });
    }

    // IDM 风格：鼠标悬停视频时，右上角浮现「下载此视频」按钮
    let hoverVideo = null;
    let dlButton = null;
    function ensureDlButton() {
        if (dlButton) return dlButton;
        const b = document.createElement('div');
        b.className = 'idm-next-float-btn';
        b.title = '用 IDM Next 下载这个视频';
        b.appendChild(ico('download', 14));
        const lbl = document.createElement('span');
        lbl.textContent = '下载此视频';
        b.appendChild(lbl);
        b.addEventListener('click', (e) => {
            e.stopPropagation();
            e.preventDefault();
            if (hoverVideo) {
                const url = hoverVideo.currentSrc || hoverVideo.src;
                openCaptureForUrl(url);   // 打开确认卡片，给予多种选择
                b.style.opacity = '0';
                setTimeout(() => b.classList.remove('show'), 200);
            }
        });
        document.body.appendChild(b);
        dlButton = b;
        return b;
    }

    document.addEventListener('mousemove', (e) => {
        const t = e.target;
        if (t && t.tagName === 'VIDEO' && t.getBoundingClientRect().width > 120) {
            hoverVideo = t;
            const b = ensureDlButton();
            b.classList.add('show');
            const r = t.getBoundingClientRect();
            const bw = b.offsetWidth || 120;
            b.style.left = Math.max(8, r.right - bw - 12) + 'px';
            b.style.top = (r.top + 12) + 'px';
            requestAnimationFrame(() => { b.style.opacity = '1'; });
        } else if (dlButton && dlButton.classList.contains('show') && t !== dlButton) {
            dlButton.style.opacity = '0';
            const btn = dlButton;
            setTimeout(() => { if (btn.style.opacity === '0') btn.classList.remove('show'); }, 200);
            hoverVideo = null;
        }
    }, true);

    // 监听 background / popup 发来的提示消息
    chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
        if (msg.action === 'showToast') {
            updateVideoMap();
            let targetRect = null;
            if (msg.url) {
                const video = videoElementMap.get(msg.url);
                if (video) {
                    const r = video.getBoundingClientRect();
                    targetRect = { left: r.left, top: r.top, bottom: r.bottom };
                }
            }
            showToast(msg.message, msg.type || 'info', targetRect);
            sendResponse({ ok: true });
        } else if (msg.action === 'showCaptureDialog') {
            // 浏览器自动拦截触发：在页面内弹出下载确认卡片
            showCaptureDialog(msg.resource || { url: msg.url, filename: '' });
            sendResponse({ ok: true });
        } else if (msg.action === 'showCaptureDialogFor') {
            // popup 点“选择路径”触发：针对某条嗅探资源打开确认卡片
            showCaptureDialog(msg.resource);
            sendResponse({ ok: true });
        }
        return false;
    });

    // ── 本页资源批量勾选面板（识别网页信息 → 直接多选下载）────────
    /* 视觉上刻意做成 popup 的「内嵌版」：同样的行结构（勾选 + 类型图标块 + 名称/元信息）、
       同样的搜索框、同样的页脚（左：全选/已选计数，右：队列 + 主按钮），
       这样用户在弹窗里学到的操作模型可以直接搬到页面上。 */
    function ensureResStyles() {
        if (document.getElementById('idm-next-res-style')) return;
        ensureChrome();
        const style = document.createElement('style');
        style.id = 'idm-next-res-style';
        // 勾选标记用 data URI（这里是浏览器 CSS，data: 正常支持；
        // 注意不能做成 input 的 ::after —— 浏览器不对 <input> 渲染伪元素）
        const checkURI = "url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='white' stroke-width='3.6' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M5 12.8l4.4 4.2L19 7.6'/%3E%3C/svg%3E\")";
        style.textContent = `
        .idm-next-res-btn{position:fixed;z-index:2147483645;right:16px;bottom:16px;
            display:flex;align-items:center;gap:7px;padding:9px 13px;border-radius:999px;
            color:#fff;font-size:12.5px;font-weight:600;cursor:pointer;
            font-family:var(--idm-font);background:var(--idm-accent);
            border:none;box-shadow:0 6px 18px rgba(15,23,42,.24);user-select:none;
            opacity:0;transform:translateY(8px);
            transition:opacity .2s ease,transform .2s ease,background .15s ease;}
        .idm-next-res-btn.show{opacity:1;transform:translateY(0);}
        .idm-next-res-btn:hover{background:var(--idm-accent-hover);}
        .idm-next-res-btn .n{display:none;min-width:18px;height:18px;padding:0 5px;border-radius:9px;
            background:rgba(255,255,255,.24);font-size:11px;font-weight:700;
            align-items:center;justify-content:center;line-height:18px;text-align:center;}
        .idm-next-res-btn .n.on{display:inline-block;}

        .idm-next-res-panel{position:fixed;z-index:2147483646;right:16px;bottom:60px;
            width:348px;max-width:92vw;max-height:64vh;display:flex;flex-direction:column;
            box-sizing:border-box;border-radius:10px;background:var(--idm-bg);color:var(--idm-text);
            overflow:hidden;font-family:var(--idm-font);font-size:13px;
            border:1px solid var(--idm-border);box-shadow:var(--idm-shadow);
            opacity:0;transform:translateY(8px) scale(.99);
            transition:opacity .2s ease,transform .2s ease;}
        .idm-next-res-panel.show{opacity:1;transform:translateY(0) scale(1);}

        .idm-res-head{display:flex;align-items:center;gap:7px;padding:10px 11px;
            border-bottom:1px solid var(--idm-border);background:var(--idm-bg);}
        .idm-res-head .hd-ic{color:var(--idm-accent);display:flex;}
        .idm-res-head .hd-t{font-weight:600;font-size:13px;color:var(--idm-text);}
        .idm-res-head .idm-res-count{color:var(--idm-text-3);font-weight:500;margin-left:3px;}
        .idm-res-head .sp{flex:1 1 auto;}
        .idm-res-close{flex:0 0 auto;display:flex;align-items:center;justify-content:center;
            width:24px;height:24px;border-radius:var(--idm-r-sm);cursor:pointer;
            color:var(--idm-text-3);transition:background .15s ease,color .15s ease;}
        .idm-res-close:hover{background:var(--idm-hover);color:var(--idm-text);}

        .idm-res-search{display:flex;align-items:center;gap:6px;margin:8px 10px 0;
            padding:0 8px;height:30px;border-radius:var(--idm-r-sm);
            border:1px solid var(--idm-border-strong);background:var(--idm-sunken);
            color:var(--idm-text-3);}
        .idm-res-search:focus-within{border-color:var(--idm-accent);background:var(--idm-bg);}
        .idm-res-search input{flex:1 1 auto;min-width:0;border:none;background:transparent;
            font-size:12.5px;color:var(--idm-text);font-family:var(--idm-font);outline:none;}

        .idm-res-list{overflow-y:auto;padding:6px 6px;flex:1 1 auto;display:flex;flex-direction:column;}
        .idm-res-item{display:flex;align-items:flex-start;gap:9px;padding:7px 7px;
            border-radius:var(--idm-r-sm);cursor:pointer;flex:0 0 auto;
            transition:background .12s ease;}
        .idm-res-item:hover{background:var(--idm-hover);}
        .idm-res-item[hidden]{display:none;}
        .idm-res-cb{flex:0 0 auto;width:15px;height:15px;margin:2px 0 0;cursor:pointer;
            appearance:none;-webkit-appearance:none;border:1.5px solid var(--idm-border-strong);
            border-radius:4px;background:var(--idm-bg);background-position:center;
            background-repeat:no-repeat;background-size:11px 11px;transition:all .12s ease;}
        .idm-res-cb:checked{border-color:var(--idm-accent);background-color:var(--idm-accent);
            background-image:${checkURI};}
        .idm-res-tile{flex:0 0 auto;width:26px;height:26px;border-radius:var(--idm-r-sm);
            display:flex;align-items:center;justify-content:center;
            background:var(--idm-sunken);color:var(--idm-text-2);}
        .idm-res-tile.t-video{color:#7c5cd6;}
        .idm-res-tile.t-audio{color:#2f9e6f;}
        .idm-res-tile.t-image{color:#c98a24;}
        .idm-res-tile.t-archive{color:#b06a3c;}
        .idm-res-tile.t-app{color:#3d7edb;}
        .idm-res-tile.t-magnet{color:#d05252;}
        .idm-res-meta{flex:1 1 auto;min-width:0;}
        .idm-res-name{font-size:12.5px;color:var(--idm-text);word-break:break-all;line-height:1.35;}
        .idm-res-sub{font-size:11px;color:var(--idm-text-3);margin-top:2px;}
        .idm-res-empty{padding:26px 18px;text-align:center;color:var(--idm-text-3);font-size:12.5px;
            flex:1 1 auto;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:8px;}
        .idm-res-empty .idm-i{opacity:.5;}

        .idm-res-fmtrow{display:flex;align-items:center;gap:8px;padding:8px 10px 0;}
        .idm-res-fmtrow label{flex:0 0 auto;color:var(--idm-text-2);font-size:12px;}
        .idm-res-fmt{flex:1 1 auto;min-width:0;padding:5px 8px;border-radius:var(--idm-r-sm);
            border:1px solid var(--idm-border-strong);background:var(--idm-sunken);
            color:var(--idm-text);font-size:12px;font-family:var(--idm-font);}
        .idm-res-fmt:focus{outline:none;border-color:var(--idm-accent);}
        .idm-res-fmt option{background:var(--idm-bg);color:var(--idm-text);}

        .idm-res-foot{display:flex;align-items:center;gap:7px;padding:9px 10px;
            border-top:1px solid var(--idm-border);background:var(--idm-sunken);
            flex-wrap:nowrap;white-space:nowrap;}
        .idm-res-selall{flex:0 0 auto;display:flex;align-items:center;gap:6px;cursor:pointer;
            color:var(--idm-text-2);font-size:12px;padding:4px 6px;border-radius:var(--idm-r-sm);
            transition:background .15s ease,color .15s ease;}
        .idm-res-selall:hover{background:var(--idm-hover);color:var(--idm-text);}
        .idm-res-picked{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;
            color:var(--idm-text-3);font-size:11.5px;}
        .idm-res-foot button{flex:0 0 auto;display:flex;align-items:center;justify-content:center;gap:5px;
            padding:7px 11px;border:1px solid transparent;border-radius:var(--idm-r-sm);
            font-size:12px;font-weight:600;cursor:pointer;font-family:var(--idm-font);
            transition:background .15s ease,border-color .15s ease,color .15s ease;}
        .idm-res-down{background:var(--idm-accent);color:#fff;}
        .idm-res-down:hover{background:var(--idm-accent-hover);}
        .idm-res-queue{background:var(--idm-accent-soft);color:var(--idm-accent);
            border-color:var(--idm-accent-border);}
        .idm-res-queue:hover{border-color:var(--idm-accent);}
        .idm-res-foot button:disabled{opacity:.55;cursor:default;}
        ${ICON_CSS}
        `;
        document.head.appendChild(style);
    }

    const typeNames = {
        video: '视频', audio: '音频', image: '图片', magnet: '磁力',
        torrent: '种子', archive: '压缩包', app: '程序', document: '文档', file: '文件'
    };
    // 类型 → 图标名（与 popup.js 的 TYPE_META 同一套映射）
    const typeIcons = {
        video: 'video', audio: 'audio', image: 'image', magnet: 'magnet',
        torrent: 'archive', archive: 'archive', app: 'app', document: 'doc', file: 'file'
    };
    function fmtSize(bytes) {
        if (!bytes || bytes <= 0) return '';
        if (bytes < 1048576) return (bytes / 1024).toFixed(0) + ' KB';
        if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + ' MB';
        return (bytes / 1073741824).toFixed(2) + ' GB';
    }

    function getResButton() {
        let b = document.getElementById('idm-next-res-btn');
        if (b) return b;
        ensureResStyles();
        b = document.createElement('div');
        b.id = 'idm-next-res-btn';
        b.className = 'idm-next-res-btn';
        b.title = '查看本页嗅探到的可下载资源';
        b.appendChild(ico('download', 14));
        const lbl = document.createElement('span');
        lbl.textContent = '本页资源';
        b.appendChild(lbl);
        const n = document.createElement('span');
        n.className = 'n';
        b.appendChild(n);
        b.addEventListener('click', (e) => {
            e.stopPropagation();
            openResPanel();
        });
        document.body.appendChild(b);
        requestAnimationFrame(() => b.classList.add('show'));
        return b;
    }

    // 悬浮球上的数量徽标：让用户在不打开面板时也知道「这页有东西」
    function setResButtonCount(count) {
        const b = getResButton();
        const n = b.querySelector('.n');
        if (!n) return;
        if (count > 0) { n.textContent = String(count); n.classList.add('on'); }
        else { n.textContent = ''; n.classList.remove('on'); }
    }

    function closeResPanel() {
        const p = document.getElementById('idm-next-res-panel');
        if (p) { p.classList.remove('show'); setTimeout(() => p.remove(), 200); }
    }

    function openResPanel() {
        ensureResStyles();
        closeResPanel();
        chrome.runtime.sendMessage({ action: 'getResourcesForTab' }, (resp) => {
            const list = (resp && resp.resources) || [];
            renderResPanel(list);
        });
    }

    function renderResPanel(list) {
        closeResPanel();
        const panel = document.createElement('div');
        panel.id = 'idm-next-res-panel';
        panel.className = 'idm-next-res-panel';

        const head = document.createElement('div');
        head.className = 'idm-res-head';
        const hdIc = document.createElement('span');
        hdIc.className = 'hd-ic';
        hdIc.appendChild(ico('inbox', 15));
        const hdT = document.createElement('span');
        hdT.className = 'hd-t';
        hdT.textContent = '本页资源';
        const hdCount = document.createElement('span');
        hdCount.className = 'idm-res-count';
        hdCount.textContent = list.length ? String(list.length) : '';
        const hdSp = document.createElement('span');
        hdSp.className = 'sp';
        const close = document.createElement('span');
        close.className = 'idm-res-close';
        close.title = '关闭';
        close.appendChild(ico('close', 14));
        close.addEventListener('click', closeResPanel);
        head.appendChild(hdIc);
        head.appendChild(hdT);
        head.appendChild(hdCount);
        head.appendChild(hdSp);
        head.appendChild(close);
        panel.appendChild(head);

        // 搜索：纯前端过滤（只切 display，dataset.idx 指向原 list，索引不会错位）
        const searchWrap = document.createElement('div');
        searchWrap.className = 'idm-res-search';
        searchWrap.appendChild(ico('search', 13));
        const searchInput = document.createElement('input');
        searchInput.type = 'text';
        searchInput.placeholder = '搜索文件名或地址…';
        searchInput.spellcheck = false;
        searchWrap.appendChild(searchInput);
        panel.appendChild(searchWrap);

        const body = document.createElement('div');
        body.className = 'idm-res-list';
        panel.appendChild(body);

        if (list.length === 0) {
            const empty = document.createElement('div');
            empty.className = 'idm-res-empty';
            empty.appendChild(ico('inbox', 26));
            const e1 = document.createElement('div');
            e1.textContent = '暂未嗅探到可下载资源';
            const e2 = document.createElement('div');
            e2.style.fontSize = '11.5px';
            e2.textContent = '播放视频或滚动页面后再试';
            empty.appendChild(e1);
            empty.appendChild(e2);
            body.appendChild(empty);
        }

        const rows = [];
        list.forEach((r, idx) => {
            const item = document.createElement('label');
            item.className = 'idm-res-item';
            const cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.className = 'idm-res-cb';
            cb.checked = true;
            cb.dataset.idx = String(idx);
            const tile = document.createElement('span');
            const t = r.type || 'file';
            tile.className = 'idm-res-tile t-' + t;
            tile.appendChild(ico(typeIcons[t] || 'file', 15));
            const meta = document.createElement('div');
            meta.className = 'idm-res-meta';
            const name = document.createElement('div');
            name.className = 'idm-res-name';
            name.textContent = r.filename || r.url;
            name.title = r.url;
            const sub = document.createElement('div');
            sub.className = 'idm-res-sub';
            const sz = fmtSize(r.size);
            sub.textContent = (typeNames[r.type] || '文件') + (sz ? ' · ' + sz : '');
            meta.appendChild(name);
            meta.appendChild(sub);
            item.appendChild(cb);
            item.appendChild(tile);
            item.appendChild(meta);
            /* 这里刻意不写「点击整行切换勾选」的 JS：item 是 <label>，点击行内任意位置
               浏览器自己就会把 click 派发给它包着的 checkbox；再手动翻一次 cb.checked
               会和标签的默认行为相互抵消（老代码就是这个毛病 —— 点文件名没反应）。 */
            body.appendChild(item);
            rows.push({ item, cb, r });
        });

        // 全局画质选择（仅当存在视频资源时出现）
        const hasVideo = list.some(r => r.type === 'video' || looksLikeVideo(r.url));
        let fmtSel = null;
        if (hasVideo) {
            const fmtRow = document.createElement('div');
            fmtRow.className = 'idm-res-fmtrow';
            const fmtLabel = document.createElement('label');
            fmtLabel.textContent = '画质（视频）';
            fmtSel = document.createElement('select');
            fmtSel.className = 'idm-res-fmt';
            const defOpt = document.createElement('option');
            defOpt.value = '';
            defOpt.textContent = '最佳画质（默认）';
            fmtSel.appendChild(defOpt);
            const loading = document.createElement('option');
            loading.textContent = '查询画质中…';
            loading.disabled = true;
            fmtSel.appendChild(loading);
            fmtRow.appendChild(fmtLabel);
            fmtRow.appendChild(fmtSel);
            panel.appendChild(fmtRow);
            // 取第一个视频资源查询画质列表
            const firstVideo = list.find(r => r.type === 'video' || looksLikeVideo(r.url));
            if (firstVideo) {
                chrome.runtime.sendMessage({ action: 'listFormats', url: firstVideo.url }, (res) => {
                    fmtSel.innerHTML = '';
                    const best = document.createElement('option');
                    best.value = '';
                    best.textContent = '最佳画质（默认）';
                    fmtSel.appendChild(best);
                    if (res && res.success && Array.isArray(res.formats) && res.formats.length) {
                        res.formats.forEach((f) => {
                            const opt = document.createElement('option');
                            opt.value = f.fmt || f.id || '';
                            opt.textContent = f.label || opt.value;
                            if (opt.value) fmtSel.appendChild(opt);
                        });
                    } else {
                        const err = document.createElement('option');
                        err.textContent = '画质不可用';
                        err.disabled = true;
                        fmtSel.appendChild(err);
                    }
                });
            }
        }

        const foot = document.createElement('div');
        foot.className = 'idm-res-foot';
        const selAll = document.createElement('span');
        selAll.className = 'idm-res-selall';
        selAll.textContent = '全选';
        const picked = document.createElement('span');
        picked.className = 'idm-res-picked';
        const btnDown = document.createElement('button');
        btnDown.className = 'idm-res-down';
        btnDown.appendChild(ico('download', 13));
        const downLbl = document.createElement('span');
        btnDown.appendChild(downLbl);
        const btnQueue = document.createElement('button');
        btnQueue.className = 'idm-res-queue';
        btnQueue.title = '加入队列（稍后开始）';
        btnQueue.appendChild(ico('queue', 13));
        const qLbl = document.createElement('span');
        qLbl.textContent = '队列';
        btnQueue.appendChild(qLbl);
        foot.appendChild(selAll);
        foot.appendChild(picked);
        foot.appendChild(btnQueue);
        foot.appendChild(btnDown);
        panel.appendChild(foot);

        document.body.appendChild(panel);
        requestAnimationFrame(() => panel.classList.add('show'));

        function visibleRows() {
            return rows.filter(({ item }) => !item.hidden);
        }
        /* 计数与提交都只看「当前可见（搜索命中）的行」。
           否则会出现「搜出 2 条、点下载却提交了 7 个」这种事 —— 用户看不到的东西
           不该被默默带上。 */
        function visibleChecked() {
            return visibleRows().filter(({ cb }) => cb.checked);
        }
        function syncFoot() {
            const vis = visibleRows();
            const sel = visibleChecked().length;
            picked.textContent = sel ? ('已选 ' + sel + ' / ' + vis.length + ' 项')
                                     : ('共 ' + vis.length + ' 项');
            downLbl.textContent = sel ? ('下载 (' + sel + ')') : '全部下载';
            const allVis = vis.length > 0 && sel === vis.length;
            selAll.textContent = allVis ? '全不选' : '全选';
            btnDown.disabled = btnQueue.disabled = sel === 0;
        }
        rows.forEach(({ cb }) => cb.addEventListener('change', syncFoot));
        selAll.addEventListener('click', () => {
            const vis = visibleRows();
            const allVis = vis.length > 0 && vis.every(({ cb }) => cb.checked);
            vis.forEach(({ cb }) => { cb.checked = !allVis; });
            syncFoot();
        });
        searchInput.addEventListener('input', () => {
            const q = searchInput.value.trim().toLowerCase();
            rows.forEach(({ item, r }) => {
                if (!q) { item.hidden = false; return; }
                const hay = ((r.filename || '') + ' ' + r.url).toLowerCase();
                item.hidden = hay.indexOf(q) === -1;
            });
            // 命中 0 条时给个明确反馈，避免看着像卡住
            const none = visibleRows().length === 0 && rows.length > 0;
            let noHit = body.querySelector('.idm-res-nohit');
            if (none && !noHit) {
                noHit = document.createElement('div');
                noHit.className = 'idm-res-empty idm-res-nohit';
                noHit.textContent = '没有匹配的资源';
                body.appendChild(noHit);
            } else if (!none && noHit) {
                noHit.remove();
            }
            syncFoot();
        });
        searchInput.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') { e.stopPropagation(); searchInput.value = ''; searchInput.dispatchEvent(new Event('input')); }
        });
        btnDown.addEventListener('click', () => submitResSelection(false));
        btnQueue.addEventListener('click', () => submitResSelection(true));
        syncFoot();

        function submitResSelection(queued) {
            const items = visibleChecked().map(({ r }) => r);
            if (items.length === 0) { showToast('请先勾选要下载的资源', 'error'); return; }
            btnDown.disabled = btnQueue.disabled = true;
            chrome.runtime.sendMessage({
                action: 'batchDownload',
                items: items.map(r => ({ url: r.url, filename: r.filename || '', queued: queued, format: fmtSel ? fmtSel.value : '' }))
            }, (response) => {
                const ok = !!(response && response.success > 0);
                const message = ok ? (response && response.message) || '已批量添加'
                                     : (response && response.message) || '批量添加失败';
                showToast(message, ok ? 'success' : 'error');
                closeResPanel();
            });
        }
    }

    updateVideoMap();
})();
