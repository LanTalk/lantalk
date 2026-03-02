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
  --bg: #f3f5f8;
  --left: #edf1f5;
  --panel: #ffffff;
  --line: #dbe3ec;
  --text: #1f2933;
  --muted: #718096;
  --blue: #2d8cff;
  --msg-in: #eff3f7;
}
* { box-sizing: border-box; }
html, body { margin: 0; width: 100%; height: 100%; background: var(--bg); color: var(--text); font-family: "Segoe UI", "Microsoft YaHei", sans-serif; }
#app { width: 100%; height: 100%; display: grid; grid-template-columns: 340px 1px 1fr; }
#left { min-height: 0; background: var(--left); display: grid; grid-template-rows: 76px 1px 1fr; }
#self { display: flex; align-items: center; justify-content: space-between; padding: 0 14px; }
#self-main { min-width: 0; }
#self-name { font-size: 14px; font-weight: 700; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
#self-id { margin-top: 4px; color: var(--muted); font-size: 12px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
#rename { height: 32px; border-radius: 8px; border: 1px solid var(--line); background: #fff; padding: 0 10px; cursor: pointer; }
.vline, .hline { background: var(--line); }
#peers { overflow-y: auto; }
.peer { height: 66px; display: grid; grid-template-columns: 1fr auto; align-items: center; padding: 10px 12px; cursor: pointer; }
.peer:hover { background: #e8eef5; }
.peer.active { background: #dde8f5; }
.peer-top { display: flex; align-items: center; gap: 6px; }
.peer-name { font-size: 13px; font-weight: 600; max-width: 230px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.dot { width: 7px; height: 7px; border-radius: 50%; background: #9aa6b2; }
.dot.on { background: #37b669; }
.peer-last { margin-top: 4px; color: var(--muted); font-size: 12px; max-width: 248px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.badge { min-width: 18px; height: 18px; border-radius: 10px; background: #ff4d4f; color: #fff; font-size: 11px; display: grid; place-items: center; padding: 0 5px; }
#chat { min-height: 0; background: var(--panel); display: grid; grid-template-rows: 58px 1px 1fr 1px 120px; }
#chat-head { display: flex; align-items: center; padding: 0 14px; font-weight: 700; }
#chat-state { margin-left: 8px; font-size: 12px; color: var(--muted); font-weight: 500; }
#empty { display: grid; place-items: center; color: #98a2ad; }
#messages { display: none; overflow-y: auto; padding: 14px; }
.msg-row { display: flex; margin-bottom: 10px; }
.msg-row.out { justify-content: flex-end; }
.bubble-wrap { max-width: min(25em, 72%); }
.bubble { border-radius: 14px; padding: 8px 12px; background: var(--msg-in); white-space: pre-wrap; word-break: break-word; line-height: 1.45; font-size: 14px; width: fit-content; }
.msg-row.out .bubble { background: var(--blue); color: #fff; margin-left: auto; }
.time { margin-top: 4px; color: #93a0ad; font-size: 11px; }
.msg-row.out .time { text-align: right; }
#composer { display: grid; grid-template-rows: 1fr 34px; gap: 8px; padding: 10px 12px; }
#input { width: 100%; height: 100%; border: none; outline: none; resize: none; background: transparent; font-size: 14px; line-height: 1.5; }
#send { justify-self: end; width: 92px; border-radius: 8px; border: none; background: var(--blue); color: #fff; cursor: pointer; }
#rename-mask { position: fixed; inset: 0; display: none; align-items: center; justify-content: center; background: rgba(0,0,0,0.2); }
#rename-card { width: 340px; background: #fff; border: 1px solid var(--line); border-radius: 10px; padding: 12px; }
#rename-input { margin-top: 8px; width: 100%; border-radius: 8px; border: 1px solid var(--line); padding: 8px; }
#rename-actions { margin-top: 12px; display: flex; justify-content: flex-end; gap: 8px; }
#rename-actions button { height: 32px; border-radius: 8px; border: 1px solid var(--line); background: #fff; padding: 0 10px; cursor: pointer; }
#rename-ok { background: var(--blue) !important; color: #fff; border-color: var(--blue) !important; }
</style>
</head>
<body>
<div id="app">
  <section id="left">
    <div id="self">
      <div id="self-main">
        <div id="self-name"></div>
        <div id="self-id"></div>
      </div>
      <button id="rename">昵称</button>
    </div>
    <div class="hline"></div>
    <div id="peers"></div>
  </section>
  <div class="vline"></div>
  <section id="chat">
    <div id="chat-head">未选择会话<span id="chat-state"></span></div>
    <div class="hline"></div>
    <div id="empty">等待局域网好友上线</div>
    <div id="messages"></div>
    <div class="hline"></div>
    <div id="composer">
      <textarea id="input" placeholder="回车发送，Shift + 回车换行"></textarea>
      <button id="send">发送</button>
    </div>
  </section>
</div>

<div id="rename-mask">
  <div id="rename-card">
    <div style="font-weight:700;font-size:14px;">修改昵称</div>
    <input id="rename-input" maxlength="32" />
    <div id="rename-actions">
      <button id="rename-cancel">取消</button>
      <button id="rename-ok">保存</button>
    </div>
  </div>
</div>

<script>
const state = { self: null, peers: [], active: "", messages: [] };

function b64e(s){ return btoa(unescape(encodeURIComponent(s || ""))); }

function esc(s){ return s == null ? "" : String(s); }

function mk(parts){ return parts.join("\t"); }

async function rpc(cmd){
  try {
    const raw = await window.native(cmd);
    return JSON.parse(raw);
  } catch {
    return { ok: false };
  }
}

async function pull(forceOpen){
  const res = await rpc(mk(["snapshot", forceOpen || state.active || ""]));
  if(!res.ok){ return; }
  state.self = res.self || state.self;
  state.peers = res.peers || [];
  state.active = res.active || "";
  state.messages = res.messages || [];
  render();
}

function renderTop(){
  if(!state.self){ return; }
  document.getElementById("self-name").textContent = esc(state.self.name);
  document.getElementById("self-id").textContent = esc(state.self.id);
}

function renderPeers(){
  const box = document.getElementById("peers");
  box.innerHTML = "";
  state.peers.forEach(p => {
    const row = document.createElement("div");
    row.className = "peer" + (state.active === p.id ? " active" : "");
    row.onclick = async () => {
      state.active = p.id;
      await rpc(mk(["open", p.id]));
      await pull(p.id);
    };

    const left = document.createElement("div");

    const top = document.createElement("div");
    top.className = "peer-top";

    const name = document.createElement("div");
    name.className = "peer-name";
    name.textContent = esc(p.name || p.id);

    const dot = document.createElement("div");
    dot.className = "dot" + (p.online ? " on" : "");

    top.appendChild(name);
    top.appendChild(dot);

    const last = document.createElement("div");
    last.className = "peer-last";
    last.textContent = esc(p.last || "");

    left.appendChild(top);
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
  const msgs = document.getElementById("messages");
  const head = document.getElementById("chat-head");
  const headState = document.getElementById("chat-state");

  if(!state.active){
    head.childNodes[0].nodeValue = "未选择会话";
    headState.textContent = "";
    empty.style.display = "grid";
    msgs.style.display = "none";
    return;
  }

  const p = state.peers.find(v => v.id === state.active);
  head.childNodes[0].nodeValue = esc((p && p.name) ? p.name : state.active);
  headState.textContent = p ? (p.online ? " 在线" : " 离线") : "";

  empty.style.display = "none";
  msgs.style.display = "block";
  msgs.innerHTML = "";

  state.messages.forEach(m => {
    const row = document.createElement("div");
    row.className = "msg-row" + (m.out ? " out" : "");

    const wrap = document.createElement("div");
    wrap.className = "bubble-wrap";

    const bubble = document.createElement("div");
    bubble.className = "bubble";
    bubble.textContent = esc(m.text);

    const time = document.createElement("div");
    time.className = "time";
    time.textContent = esc(m.time || "");

    wrap.appendChild(bubble);
    wrap.appendChild(time);
    row.appendChild(wrap);
    msgs.appendChild(row);
  });

  msgs.scrollTop = msgs.scrollHeight;
}

function render(){
  renderTop();
  renderPeers();
  renderMessages();
}

async function sendNow(){
  if(!state.active){ return; }
  const input = document.getElementById("input");
  const text = input.value;
  if(!text.trim()){ return; }
  input.value = "";
  await rpc(mk(["send", state.active, b64e(text)]));
  await pull(state.active);
}

function bind(){
  document.getElementById("send").onclick = sendNow;
  document.getElementById("input").addEventListener("keydown", async e => {
    if(e.key === "Enter" && !e.shiftKey){
      e.preventDefault();
      await sendNow();
    }
  });

  const mask = document.getElementById("rename-mask");
  document.getElementById("rename").onclick = () => {
    document.getElementById("rename-input").value = state.self ? state.self.name : "";
    mask.style.display = "flex";
  };
  document.getElementById("rename-cancel").onclick = () => { mask.style.display = "none"; };
  document.getElementById("rename-ok").onclick = async () => {
    const v = (document.getElementById("rename-input").value || "").trim();
    if(!v){ return; }
    await rpc(mk(["set_name", b64e(v)]));
    mask.style.display = "none";
    await pull(state.active);
  };
  mask.addEventListener("click", e => { if(e.target.id === "rename-mask") mask.style.display = "none"; });
}

async function init(){
  bind();
  await pull("");
  setInterval(() => { pull(state.active); }, 800);
}

init();
</script>
</body>
</html>
)HTML";
}

} // namespace lantalk
