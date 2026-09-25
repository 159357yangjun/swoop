const $ = (id) => document.getElementById(id);
const state = { tab: null, resources: [] };

function send(type, payload = {}) {
  return new Promise((resolve) => {
    chrome.runtime.sendMessage({ type, ...payload }, (response) => {
      const error = chrome.runtime.lastError;
      resolve(error ? { ok: false, error: error.message } : (response || { ok: false, error: "没有收到响应" }));
    });
  });
}

function showMessage(text, bad = false) {
  const box = $("message");
  box.textContent = text;
  box.classList.toggle("bad", bad);
  box.hidden = !text;
}

function setConnection(ok, label) {
  const node = $("connection");
  node.textContent = label;
  node.className = `connection ${ok ? "connection-ok" : "connection-bad"}`;
}

function iconFor(type) {
  return { video: "▶", audio: "♫", archive: "▦", document: "▤", file: "↘" }[type] || "↗";
}

function renderRecent(items) {
  const list = $("recent-list");
  if (!items.length) {
    list.innerHTML = '<div class="empty">还没有接管记录</div>';
    return;
  }
  list.innerHTML = items.slice(0, 5).map((item) => `
    <div class="recent">
      <div class="resource-icon">${iconFor(item.type)}</div>
      <div class="recent-copy"><span class="recent-name" title="${escapeHtml(item.url)}">${escapeHtml(item.filename)}</span><span class="recent-meta">已交给 Swoop</span></div>
    </div>`).join("");
}

function renderResources(items) {
  state.resources = items;
  $("resource-count").textContent = String(items.length);
  $("resource-section").classList.toggle("hidden", !items.length);
  $("resource-list").innerHTML = items.map((item, index) => `
    <div class="resource">
      <div class="resource-icon">${iconFor(item.type)}</div>
      <div class="resource-copy"><span class="resource-name" title="${escapeHtml(item.url)}">${escapeHtml(item.filename)}</span><span class="resource-meta">${item.type} · ${hostOf(item.url)}</span></div>
      <button class="take" data-index="${index}" type="button">接管</button>
    </div>`).join("");
  document.querySelectorAll(".take").forEach((button) => {
    button.addEventListener("click", () => takeResource(Number(button.dataset.index), button));
  });
}

function escapeHtml(value) {
  return String(value || "").replace(/[&<>"']/g, (char) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[char]));
}

function hostOf(value) {
  try { return new URL(value).host; } catch (_) { return "网页资源"; }
}

async function refresh() {
  const [ping, recent] = await Promise.all([send("ping"), send("recent")]);
  if (ping.ok) setConnection(true, "● 宿主已连接");
  else setConnection(false, "● 宿主未连接");
  renderRecent(recent.items || []);
  if (!ping.ok) showMessage("宿主未连接：请在解压目录运行 swoop_nmhost.exe --register-nmhost <扩展ID>", true);
}

async function scanPage() {
  $("scan").disabled = true;
  showMessage("");
  try {
    const tabs = await chrome.tabs.query({ active: true, currentWindow: true });
    state.tab = tabs[0];
    if (!state.tab || !state.tab.id) throw new Error("没有找到当前页面");
    $("page-label").textContent = state.tab.title || state.tab.url || "当前页面";
    const response = await new Promise((resolve) => chrome.tabs.sendMessage(state.tab.id, { type: "scan" }, (value) => {
      const error = chrome.runtime.lastError;
      resolve(error ? { ok: false, error: "此页面不允许扫描" } : (value || { ok: false, error: "页面没有响应" }));
    }));
    if (!response.ok) throw new Error(response.error || "无法扫描当前页面");
    $("page-label").textContent = `${response.pageTitle || "当前页面"} · ${response.items.length} 个资源`;
    renderResources(response.items);
    if (!response.items.length) showMessage("没有发现可下载资源，可尝试右键链接使用 Swoop。", false);
  } catch (error) {
    showMessage(error.message || "扫描失败", true);
  } finally {
    $("scan").disabled = false;
  }
}

async function takeResource(index, button) {
  const item = state.resources[index];
  if (!item) return;
  button.disabled = true;
  const response = await send("download", { item: { ...item, referer: state.tab && state.tab.url } });
  button.disabled = false;
  if (response.ok) {
    button.textContent = "已接管";
    button.disabled = true;
    showMessage(`${item.filename} 已交给 Swoop。`);
    refresh();
  } else {
    showMessage(response.error || "宿主未连接，浏览器下载未受影响。", true);
  }
}

async function openMain() {
  const response = await send("open-main");
  if (!response.ok) showMessage("宿主未连接，暂时无法打开 Swoop。", true);
}

$("scan").addEventListener("click", scanPage);
$("open-main").addEventListener("click", openMain);
refresh();
