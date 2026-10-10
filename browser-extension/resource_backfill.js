/* Captures already-finished ResourceTiming entries missed by an early MV3 listener.
 * Intentionally limited to HTTP(S) URLs with a media extension in the pathname.
 * Cannot recover cross-context requests, cleared/evicted timing buffers, or
 * extensionless media: this is an additive hint, NOT a webRequest replacement. */
(() => {
    'use strict';
    const MEDIA_EXT = /\.(?:mp4|m4v|webm|mkv|mov|avi|flv|ts|m2ts|mp3|m4a|aac|ogg|opus|flac|wav|m3u8|mpd)$/i;
    const VIDEO_EXT = /\.(?:mp4|m4v|webm|mkv|mov|avi|flv|ts|m2ts|m3u8|mpd)$/i;
    function collect(entries, seen, baseUrl) {
        const found = [];
        for (const entry of Array.isArray(entries) ? entries : []) {
            if (!entry || typeof entry.name !== 'string') continue;
            let u;
            try { u = new URL(entry.name, baseUrl); } catch { continue; }
            if (u.protocol !== 'https:' && u.protocol !== 'http:') continue;
            if (!MEDIA_EXT.test(u.pathname)) continue;
            const href = u.href;
            if (seen.has(href)) continue;
            seen.add(href);
            const filename = u.pathname.split('/').pop() || '';
            found.push({
                url: href, filename,
                type: VIDEO_EXT.test(u.pathname) ? 'video' : 'audio',
                size: -1, source: 'performance-backfill'
            });
        }
        return found;
    }
    globalThis.IDMResourceBackfill = Object.freeze({ collect });
})();
