#pragma once

#include <string>

namespace lantalk {

inline std::string ui_html() {
  return R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>LanTalk</title>
<style>
:root {
  --bg: #f5f7fa;
  --panel: #ffffff;
  --line: #dbe2ea;
  --text: #1d2833;
  --muted: #6e7b88;
  --blue: #2387ff;
  --in: #f1f4f8;
}
* { box-sizing: border-box; }
html, body { margin: 0; width: 100%; height: 100%; font-family: "Segoe UI", "Microsoft YaHei", sans-serif; background: var(--bg); color: var(--text); }
#app { width: 100%; height: 100%; display: grid; grid-template-rows: 58px 1px 1fr; }
#top { background: var(--panel); display: flex; align-items: center; justify-content: space-between; padding: 0 14px; }
#top .meta { min-width: 0; }
#self-name { font-size: 14px; font-weight: 600; }
#self-id { font-size: 12px; color: var(--muted); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 680px; }
#rename-btn { border: 1px solid var(--line); background: #fff; border-radius: 8px; height: 34px; padding: 0 12px; cursor: pointer; }
#line { background: var(--line); }
#main { min-height: 0; display: grid; grid-template-columns: 300px 1px 1fr; }
#peers { background: #f8fafc; overflow-y: auto; }
#vline { background: var(--line); }
#chat { display: grid; grid-template-rows: 52px 1px 1fr 1px 96px; background: var(--panel); }
#peer-head { display: flex; align-items: center; padding: 0 14px; font-weight: 600; }
#peer-head span { margin-left: 8px; color: var(--muted); font-size: 12px; }
#msgs { overflow-y: auto; padding: 14px; }
#empty { display: grid; place-items: center; color: #98a4b1; }
#compose { padding: 8px 12px; display: grid; grid-template-rows: 1fr 34px; gap: 8px; }
#input { width: 100%; height: 100%; border: none; resize: none; outline: none; background: transparent; font-size: 14px; line-height: 1.5; }
#send { justify-self: end; width: 92px; border: none; border-radius: 8px; background: var(--blue); color: #fff; cursor: pointer; }
.peer-item { height: 62px; display: grid; grid-template-columns: 1fr auto; align-items: center; padding: 8px 12px; cursor: pointer; }
.peer-item.active { background: #e8eff8; }
.peer-item:hover { background: #eef4fa; }
.peer-title { display: flex; align-items: center; gap: 6px; }
.dot { width: 7px; height: 7px; border-radius: 50%; background: #97a3af; }
.dot.on { background: #35b768; }
.peer-name { font-size: 13px; font-weight: 600; max-width: 220px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.peer-last { font-size: 12px; color: var(--muted); max-width: 240px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.badge { min-width: 18px; padding: 0 6px; height: 18px; border-radius: 10px; background: #ff4d4f; color: #fff; font-size: 11px; display: grid; place-items: center; }
.msg { display: flex; margin-bottom: 10px; }
.msg.out { justify-content: flex-end; }
.bubble { width: fit-content; max-width: min(25em, 72%); background: var(--in); border-radius: 14px; padding: 8px 12px; white-space: pre-wrap; word-break: break-word; }
.msg.out .bubble { background: var(--blue); color: #fff; }
.time { margin-top: 4px; font-size: 11px; color: #90a0ae; }
#rename-dialog { position: fixed; inset: 0; display: none; align-items: center; justify-content: center; background: rgba(9, 18, 32, 0.2); }
#rename-card { width: 340px; background: #fff; border-radius: 10px; border: 1px solid var(--line); padding: 12px; }
#rename-input { width: 100%; border: 1px solid var(--line); border-radius: 8px; padding: 8px; margin-top: 8px; }
#rename-actions { margin-top: 12px; display: flex; justify-content: flex-end; gap: 8px; }
#rename-actions button { height: 32px; border-radius: 8px; padding: 0 10px; border: 1px solid var(--line); background: #fff; cursor: pointer; }
#rename-actions .ok { background: var(--blue); color: #fff; border-color: var(--blue); }
</style>
</head>
<body>
<div id="app">
  <header id="top">
    <div class="meta">
      <div id="self-name"></div>
      <div id="self-id"></div>
    </div>
    <button id="rename-btn">修改昵称</button>
  </header>
  <div id="line"></div>
  <section id="main">
    <aside id="peers"></aside>
    <div id="vline"></div>
    <div id="chat">
      <div id="peer-head">未选择会话<span></span></div>
      <div id="line"></div>
      <div id="empty">等待局域网用户上线</div>
      <div id="msgs" style="display:none"></div>
      <div id="line"></div>
      <div id="compose">
        <textarea id="input" placeholder="回车发送，Shift+回车换行"></textarea>
        <button id="send">发送</button>
      </div>
    </div>
  </section>
</div>

<div id="rename-dialog">
  <div id="rename-card">
    <div style="font-size:14px;font-weight:600;">修改昵称</div>
    <input id="rename-input" maxlength="32" />
    <div id="rename-actions">
      <button id="rename-cancel">取消</button>
      <button class="ok" id="rename-save">保存</button>
    </div>
  </div>
</div>

<script>
const state = { revision: 0, self: null, peers: [], active: "", messages: [] };

function b64e(s){ return btoa(unescape(encodeURIComponent(s || ""))); }
function b64d(s){ try { return decodeURIComponent(escape(atob(s || ""))); } catch { return ""; } }

function safe(parts){ return parts.join("\t"); }

async function rpc(cmd){
  try {
    const raw = await window.native(cmd);
    return JSON.parse(raw);
  } catch {
    return { ok: false, error: "rpc failed" };
  }
}

function renderTop(){
  if(!state.self) return;
  document.getElementById("self-name").textContent = state.self.name || "";
  document.getElementById("self-id").textContent = state.self.id || "";
}

function renderPeers(){
  const box = document.getElementById("peers");
  box.innerHTML = "";
  state.peers.forEach(p => {
    const row = document.createElement("div");
    row.className = "peer-item" + (state.active === p.id ? " active" : "");
    row.onclick = async () => {
      state.active = p.id;
      await rpc(safe(["open", p.id]));
      await pull(true);
    };

    const left = document.createElement("div");
    const title = document.createElement("div");
    title.className = "peer-title";
    const name = document.createElement("div");
    name.className = "peer-name";
    name.textContent = p.name || p.id;
    const dot = document.createElement("div");
    dot.className = "dot" + (p.online ? " on" : "");
    title.appendChild(name);
    title.appendChild(dot);

    const last = document.createElement("div");
    last.className = "peer-last";
    last.textContent = p.last || "";

    left.appendChild(title);
    left.appendChild(last);

    const badge = document.createElement("div");
    badge.className = "badge";
    badge.style.visibility = p.unread > 0 ? "visible" : "hidden";
    badge.textContent = p.unread > 99 ? "99+" : String(p.unread || "");

    row.appendChild(left);
    row.appendChild(badge);
    box.appendChild(row);
  });
}

function renderMessages(){
  const empty = document.getElementById("empty");
  const msgs = document.getElementById("msgs");
  const head = document.getElementById("peer-head");

  if(!state.active){
    head.innerHTML = "未选择会话<span></span>";
    empty.style.display = "grid";
    msgs.style.display = "none";
    return;
  }

  const p = state.peers.find(v => v.id === state.active);
  head.innerHTML = `${(p?.name || state.active)}<span>${p?.online ? "在线" : "离线"}</span>`;
  empty.style.display = "none";
  msgs.style.display = "block";
  msgs.innerHTML = "";

  state.messages.forEach(m => {
    const row = document.createElement("div");
    row.className = "msg" + (m.out ? " out" : "");

    const wrap = document.createElement("div");
    const bubble = document.createElement("div");
    bubble.className = "bubble";
    bubble.textContent = m.text || "";

    const tm = document.createElement("div");
    tm.className = "time";
    tm.textContent = m.time || "";

    wrap.appendChild(bubble);
    wrap.appendChild(tm);
    row.appendChild(wrap);
    msgs.appendChild(row);
  });
  msgs.scrollTop = msgs.scrollHeight;
}

async function pull(force = false){
  const res = await rpc(safe(["snapshot", state.active || "", String(state.revision || 0)]));
  if(!res.ok) return;
  if(!force && !res.changed) return;

  state.revision = res.revision || state.revision;
  state.self = res.self || state.self;
  state.peers = res.peers || [];
  state.active = res.active || "";
  state.messages = res.messages || [];

  renderTop();
  renderPeers();
  renderMessages();
}

async function sendNow(){
  if(!state.active) return;
  const input = document.getElementById("input");
  const text = input.value;
  if(!text.trim()) return;
  input.value = "";
  await rpc(safe(["send", state.active, b64e(text)]));
  await pull(true);
}

function bindEvents(){
  const input = document.getElementById("input");
  input.addEventListener("keydown", async e => {
    if(e.key === "Enter" && !e.shiftKey){
      e.preventDefault();
      await sendNow();
    }
  });
  document.getElementById("send").onclick = sendNow;

  const dlg = document.getElementById("rename-dialog");
  document.getElementById("rename-btn").onclick = () => {
    document.getElementById("rename-input").value = state.self?.name || "";
    dlg.style.display = "flex";
  };
  document.getElementById("rename-cancel").onclick = () => { dlg.style.display = "none"; };
  document.getElementById("rename-save").onclick = async () => {
    const v = (document.getElementById("rename-input").value || "").trim();
    if(v){
      await rpc(safe(["set_name", b64e(v)]));
      dlg.style.display = "none";
      await pull(true);
    }
  };
  dlg.addEventListener("click", e => { if(e.target.id === "rename-dialog") dlg.style.display = "none"; });
}

async function init(){
  bindEvents();
  const boot = await rpc("bootstrap");
  if(boot.ok){
    state.revision = boot.revision || 0;
    state.self = boot.self || null;
    state.peers = boot.peers || [];
    state.active = boot.active || "";
    state.messages = boot.messages || [];
    renderTop();
    renderPeers();
    renderMessages();
  }
  setInterval(() => pull(false), 900);
}

init();
</script>
</body>
</html>
)HTML";
}

} // namespace lantalk
