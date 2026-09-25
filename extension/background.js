// IDM Next 资源嗅探器（MV3 service worker）
// 经 native messaging 把嗅探到的下载地址发给宿主进程 idm_nmhost.exe，
// 宿主再转交给常驻的 idm.exe。对标 IDM 的浏览器捕获。

const HOST = "com.tencent.idm_next";

// 发消息给宿主。cb(ok, resp, err) —— ok=false 表示宿主不可用/未注册。
function sendToHost(payload, cb) {
  let called = false;
  const finish = (ok, resp, err) => {
    if (called) return;
    called = true;
    if (cb) cb(ok, resp, err);
  };
  try {
    chrome.runtime.sendNativeMessage(HOST, payload, (resp) => {
      const err = chrome.runtime.lastError;
      if (err) {
        console.warn("[IDM Next] 宿主不可用:", err.message,
                     "（请先运行 idm_nmhost.exe --register-nmhost <扩展ID> 注册）");
      }
      finish(!err, resp, err);
    });
  } catch (e) {
    console.warn("[IDM Next]", e);
    finish(false, null, e);
  }
}

chrome.runtime.onInstalled.addListener(() => {
  // 重装/更新时先清掉旧的，否则 create 会报 duplicate id
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: "idm-next-download",
      title: "用 IDM Next 下载",
      contexts: ["link", "image", "video", "audio", "selection"]
    });
  });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  let url = info.linkUrl || info.srcUrl;
  if (!url && info.selectionText) {
    const m = info.selectionText.match(/https?:\/\/\S+/);
    if (m) url = m[0];
  }
  if (!url) return;
  // 右键菜单是用户主动点的：转交失败就明确告知，不要静默吞掉
  sendToHost(
    { action: "download", url, filename: "", referer: (tab && tab.url) || "" },
    (ok) => { if (!ok) console.warn("[IDM Next] 转交失败，未创建下载任务:", url); }
  );
});

// 捕获浏览器自带下载 → 转交 IDM Next，**成功后才**取消浏览器那份。
// ⚠️ 顺序很重要：宿主不可用时绝不能取消，否则浏览器下载被抹掉、
//    新任务也没建 —— 用户点了一次下载，结果什么都没拿到。
chrome.downloads.onCreated.addListener((item) => {
  if (!item || !item.url) return;
  if (/^(blob|data|filesystem):/i.test(item.url)) return;
  const guess = (item.filename || "").split(/[\\/]/).pop() || "";
  sendToHost(
    {
      action: "download",
      url: item.url,
      filename: guess,
      referer: item.referrer || ""
    },
    (ok) => {
      if (!ok) {
        console.warn("[IDM Next] 未接住，保留浏览器下载。");
        return;                       // 让浏览器继续原来的下载
      }
      chrome.downloads.cancel(item.id, () => {
        if (!chrome.runtime.lastError) chrome.downloads.erase({ id: item.id });
      });
    }
  );
});
