const DOWNLOAD_EXTENSIONS = new Set([
  "7z", "avi", "bin", "bz2", "csv", "doc", "docx", "epub", "flac",
  "gz", "iso", "m4a", "m4v", "mkv", "mov", "mp3", "mp4", "mpeg",
  "msi", "pdf", "ppt", "pptx", "rar", "tar", "tgz", "txt", "wav",
  "webm", "webp", "xls", "xlsx", "zip"
]);

function filenameFromUrl(url, downloadName) {
  if (downloadName) return downloadName.split(/[\\/]/).pop() || "download";
  const pathname = new URL(url).pathname;
  const leaf = pathname.split("/").pop() || "";
  try {
    return decodeURIComponent(leaf) || "download";
  } catch (_) {
    return leaf || "download";
  }
}

function typeFor(url, tagName) {
  const ext = filenameFromUrl(url, "").split(".").pop().toLowerCase();
  if (tagName === "VIDEO" || ["avi", "m4v", "mkv", "mov", "mp4", "mpeg", "webm"].includes(ext)) return "video";
  if (tagName === "AUDIO" || ["flac", "m4a", "mp3", "wav"].includes(ext)) return "audio";
  if (["7z", "bz2", "gz", "rar", "tar", "tgz", "zip"].includes(ext)) return "archive";
  if (["doc", "docx", "epub", "pdf", "ppt", "pptx", "txt", "xls", "xlsx"].includes(ext)) return "document";
  return DOWNLOAD_EXTENSIONS.has(ext) ? "file" : "link";
}

function collectCandidates(doc, pageUrl) {
  const found = new Map();
  const selectors = ["a[href]", "video[src]", "audio[src]", "source[src]"];
  for (const selector of selectors) {
    for (const element of doc.querySelectorAll(selector)) {
      const raw = element.getAttribute("href") || element.getAttribute("src");
      if (!raw) continue;
      let url;
      try {
        url = new URL(raw, pageUrl);
      } catch (_) {
        continue;
      }
      if (!["http:", "https:"].includes(url.protocol)) continue;
      const normalized = url.href;
      if (found.has(normalized)) continue;
      const filename = filenameFromUrl(normalized, element.getAttribute("download"));
      const type = typeFor(normalized, element.tagName || "A");
      if (type === "link" && !DOWNLOAD_EXTENSIONS.has(filename.split(".").pop().toLowerCase())) continue;
      found.set(normalized, {
        url: normalized,
        filename,
        type,
      });
      if (found.size >= 40) return Array.from(found.values());
    }
  }
  return Array.from(found.values());
}

if (typeof chrome !== "undefined" && chrome.runtime) {
  chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    if (!message || message.type !== "scan") return false;
    try {
      sendResponse({
        ok: true,
        pageTitle: document.title || "当前页面",
        items: collectCandidates(document, location.href),
      });
    } catch (error) {
      sendResponse({ ok: false, error: error.message || "无法扫描当前页面" });
    }
    return true;
  });
}

if (typeof module !== "undefined") {
  module.exports = { collectCandidates };
}
