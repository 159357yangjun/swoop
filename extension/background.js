// IDM Next 资源嗅探器（MV3 service worker）
// 经 native messaging 把嗅探到的下载地址发给宿主进程 idm_nmhost.exe，
// 宿主再转交给常驻的 idm.exe。对标 IDM 的浏览器捕获。

const HOST = "com.tencent.idm_next";

function sendToHost(payload) {
  try {
    chrome.runtime.sendNativeMessage(HOST, payload, (resp) => {
      if (chrome.runtime.lastError) {
        console.warn("[IDM Next] 宿主不可用:", chrome.runtime.lastError.message,
                     "（请先运行 idm_nmhost.exe --register-nmhost <扩展ID> 注册）");
        return;
      }
      console.debug("[IDM Next] 宿主回执:", resp);
    });
  } catch (e) {
    console.warn("[IDM Next]", e);
  }
}

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: "idm-next-download",
    title: "用 IDM Next 下载",
    contexts: ["link", "image", "video", "audio", "selection"]
  });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  let url = info.linkUrl || info.srcUrl;
  if (!url && info.selectionText) {
    const m = info.selectionText.match(/https?:\/\/\S+/);
    if (m) url = m[0];
  }
  if (!url) return;
  sendToHost({ action: "download", url, filename: "", referer: (tab && tab.url) || "" });
});

// 捕获浏览器自带下载 → 取消并转交 IDM Next。
// 如需关闭捕获，删除下面这段 downloads.onCreated 监听即可。
chrome.downloads.onCreated.addListener((item) => {
  if (!item || !item.url) return;
  if (/^(blob|data|filesystem):/i.test(item.url)) return;
  const guess = (item.filename || "").split(/[\\/]/).pop() || "";
  sendToHost({
    action: "download",
    url: item.url,
    filename: guess,
    referer: item.referrer || ""
  });
  chrome.downloads.cancel(item.id, () => {
    if (!chrome.runtime.lastError) chrome.downloads.erase({ id: item.id });
  });
});
