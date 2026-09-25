// Swoop 资源嗅探器（MV3 service worker）
// 经 native messaging 把嗅探到的下载地址发给宿主进程 swoop_nmhost.exe，
// 宿主再转交给常驻的 swoop.exe。对标 IDM 的浏览器捕获。

const HOST = "com.yangjun.swoop";

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
        console.warn("[Swoop] 宿主不可用:", err.message,
                     "（请先运行 swoop_nmhost.exe --register-nmhost <扩展ID> 注册）");
      }
      finish(!err, resp, err);
    });
  } catch (e) {
    console.warn("[Swoop]", e);
    finish(false, null, e);
  }
}

function saveRecentTask(item) {
  chrome.storage.local.get({ swoopRecent: [] }, (data) => {
    const recent = [item].concat(data.swoopRecent || [])
      .filter((task, index, all) => all.findIndex((x) => x.url === task.url) === index)
      .slice(0, 5);
    chrome.storage.local.set({ swoopRecent: recent });
  });
}

function popupDownload(item, sendResponse) {
  if (!item || !/^https?:/i.test(item.url || "")) {
    sendResponse({ ok: false, error: "只支持 HTTP(S) 下载地址" });
    return;
  }
  const payload = {
    action: "download",
    url: item.url,
    filename: item.filename || "",
    referer: item.referer || ""
  };
  sendToHost(payload, (ok, resp, err) => {
    if (ok) {
      saveRecentTask({
        url: item.url,
        filename: item.filename || item.url,
        type: item.type || "file",
        at: Date.now()
      });
    }
    sendResponse({ ok, response: resp || null, error: err ? err.message : "宿主未连接" });
  });
}

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (!message) return false;
  if (message.type === "ping") {
    sendToHost({ action: "ping" }, (ok, resp, err) => {
      sendResponse({ ok, app: resp && resp.app, error: err ? err.message : "宿主未连接" });
    });
    return true;
  }
  if (message.type === "recent") {
    chrome.storage.local.get({ swoopRecent: [] }, (data) => {
      sendResponse({ ok: true, items: data.swoopRecent || [] });
    });
    return true;
  }
  if (message.type === "download") {
    popupDownload(message.item, sendResponse);
    return true;
  }
  if (message.type === "open-main") {
    sendToHost({ action: "show" }, (ok, resp, err) => {
      sendResponse({ ok, response: resp || null, error: err ? err.message : "宿主未连接" });
    });
    return true;
  }
  return false;
});

chrome.runtime.onInstalled.addListener(() => {
  // 重装/更新时先清掉旧的，否则 create 会报 duplicate id
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: "swoop-download",
      title: "用 Swoop 下载",
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
    (ok) => { if (!ok) console.warn("[Swoop] 转交失败，未创建下载任务:", url); }
  );
});

// 捕获浏览器自带下载 → 转交 Swoop，**成功后才**取消浏览器那份。
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
        console.warn("[Swoop] 未接住，保留浏览器下载。");
        return;                       // 让浏览器继续原来的下载
      }
      chrome.downloads.cancel(item.id, () => {
        if (!chrome.runtime.lastError) chrome.downloads.erase({ id: item.id });
      });
    }
  );
});
