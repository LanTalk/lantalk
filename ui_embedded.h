#pragma once

namespace lantalk {

inline constexpr const char *kUiHtml = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>LanTalk</title>
<style>
:root {
  --bg-side: #eceff1;
  --bg-main: #ffffff;
  --line: #dde2e7;
  --text: #1f2933;
  --sub: #6b7683;
  --blue: #2383ff;
  --bubble-in: #f2f4f7;
  --bubble-out: #2383ff;
  --bubble-out-text: #ffffff;
  --mid-width: 320px;
  --composer-height: 150px;
}
* { box-sizing: border-box; }
html, body {
  margin: 0;
  width: 100%;
  height: 100%;
  font-family: "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
  color: var(--text);
  background: var(--bg-main);
}
#app {
  width: 100%;
  height: 100%;
  display: grid;
  grid-template-columns: 72px 1px var(--mid-width) 1px 1fr;
}
#side {
  background: var(--bg-side);
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: space-between;
  padding: 12px 0;
}
#sessions {
  background: #f8fafb;
  overflow-y: auto;
}
#chat {
  background: var(--bg-main);
  display: grid;
  grid-template-rows: 64px 1px 1fr 1px var(--composer-height);
  min-width: 420px;
}
.divider-v, .divider-h {
  background: var(--line);
  position: relative;
}
.divider-v { cursor: col-resize; }
.divider-h { cursor: row-resize; }
.divider-v::before {
  content: "";
  position: absolute;
  left: -5px;
  top: 0;
  width: 11px;
  height: 100%;
}
.divider-h::before {
  content: "";
  position: absolute;
  left: 0;
  top: -5px;
  width: 100%;
  height: 11px;
}
.avatar {
  width: 38px;
  height: 38px;
  border-radius: 50%;
  overflow: hidden;
  flex: 0 0 auto;
  display: grid;
  place-items: center;
  color: #fff;
  font-size: 14px;
  user-select: none;
  background: linear-gradient(135deg, #5f9dff, #2f6dff);
}
.avatar img { width: 100%; height: 100%; object-fit: cover; }
#self-avatar { cursor: pointer; }
.icon-btn {
  border: none;
  background: transparent;
  color: #475467;
  font-size: 18px;
  width: 34px;
  height: 34px;
  border-radius: 8px;
  cursor: pointer;
}
.icon-btn:hover { background: rgba(15, 23, 42, 0.06); }
.session-item {
  height: 62px;
  border-bottom: 1px solid transparent;
  display: grid;
  grid-template-columns: 50px 1fr auto;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  cursor: pointer;
}
.session-item:hover { background: #eef2f6; }
.session-item.active { background: #e6edf6; }
.s-main {
  min-width: 0;
  display: grid;
  grid-template-rows: 1fr 1fr;
}
.s-name-line {
  display: flex;
  align-items: center;
  gap: 6px;
  min-width: 0;
}
.s-name {
  font-size: 13px;
  font-weight: 600;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.status-dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: #9aa4af;
}
.status-dot.online { background: #39b46a; }
.s-last {
  font-size: 12px;
  color: var(--sub);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.badge {
  min-width: 18px;
  height: 18px;
  padding: 0 6px;
  border-radius: 10px;
  background: #ff4d4f;
  color: #fff;
  font-size: 11px;
  display: grid;
  place-items: center;
}
#chat-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 14px;
}
#chat-peer {
  display: flex;
  align-items: center;
  gap: 10px;
  min-width: 0;
}
#peer-meta {
  display: grid;
  grid-template-rows: 1fr 1fr;
  min-width: 0;
}
#peer-name {
  font-weight: 600;
  font-size: 14px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
#peer-state {
  font-size: 12px;
  color: var(--sub);
}
#chat-empty {
  display: grid;
  place-items: center;
  color: #98a2b3;
  font-size: 14px;
}
#msg-wrap {
  overflow-y: auto;
  padding: 16px;
  display: none;
}
.msg-row {
  display: flex;
  margin-bottom: 10px;
}
.msg-row.out { justify-content: flex-end; }
.bubble {
  display: inline-block;
  width: fit-content;
  max-width: min(25em, 72%);
  border-radius: 14px;
  padding: 8px 12px;
  white-space: pre-wrap;
  word-break: break-word;
  line-height: 1.45;
  font-size: 14px;
  background: var(--bubble-in);
}
.msg-row.out .bubble {
  background: var(--bubble-out);
  color: var(--bubble-out-text);
}
.bubble .img-msg {
  max-width: 280px;
  max-height: 240px;
  border-radius: 10px;
  display: block;
}
.file-link {
  color: inherit;
  text-decoration: underline;
  cursor: pointer;
}
#composer {
  display: grid;
  grid-template-rows: 40px 1fr;
  padding: 6px 10px 8px;
}
#toolbar {
  display: flex;
  align-items: center;
  gap: 6px;
}
#input {
  width: 100%;
  height: 100%;
  border: none;
  outline: none;
  resize: none;
  padding: 8px 2px;
  font-size: 14px;
  line-height: 1.45;
  background: transparent;
}
#emoji-panel {
  position: absolute;
  bottom: calc(var(--composer-height) + 16px);
  right: 18px;
  width: 320px;
  max-height: 210px;
  overflow-y: auto;
  border: 1px solid var(--line);
  background: #fff;
  border-radius: 10px;
  padding: 8px;
  display: none;
  grid-template-columns: repeat(8, 1fr);
  gap: 6px;
}
.emoji-btn {
  border: none;
  background: transparent;
  font-size: 22px;
  width: 32px;
  height: 32px;
  cursor: pointer;
  border-radius: 6px;
}
.emoji-btn:hover { background: #eef3f8; }
#overlay {
  position: fixed;
  inset: 0;
  background: rgba(17, 24, 39, 0.18);
  display: none;
  align-items: center;
  justify-content: center;
}
.modal {
  width: 420px;
  max-width: calc(100vw - 24px);
  background: #fff;
  border: 1px solid var(--line);
  border-radius: 12px;
  padding: 14px;
}
.modal h3 {
  margin: 0 0 12px;
  font-size: 15px;
}
.f-row {
  margin-bottom: 10px;
}
.f-row label {
  display: block;
  font-size: 12px;
  color: var(--sub);
  margin-bottom: 4px;
}
.f-row input {
  width: 100%;
  border: 1px solid var(--line);
  border-radius: 8px;
  padding: 8px;
  font-size: 13px;
}
.actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 10px;
}
.btn {
  border: 1px solid var(--line);
  border-radius: 8px;
  background: #fff;
  font-size: 13px;
  padding: 6px 12px;
  cursor: pointer;
}
.btn.primary {
  background: var(--blue);
  color: #fff;
  border-color: var(--blue);
}
</style>
</head>
<body>
  <div id="app">
    <aside id="side">
      <div id="self-avatar" class="avatar"></div>
      <button id="btn-settings" class="icon-btn" title="设置">⚙</button>
    </aside>

    <div id="line1" class="divider-v"></div>

    <section id="sessions"></section>

    <div id="line2" class="divider-v"></div>

    <section id="chat">
      <header id="chat-header">
        <div id="chat-peer">
          <div id="peer-avatar" class="avatar"></div>
          <div id="peer-meta">
            <div id="peer-name"></div>
            <div id="peer-state"></div>
          </div>
        </div>
        <button id="btn-profile" class="btn">资料</button>
      </header>
      <div class="divider-h" style="cursor:default"></div>
      <div id="chat-empty">&nbsp;</div>
      <div id="msg-wrap"></div>
      <div id="line3" class="divider-h"></div>
      <div id="composer">
        <div id="toolbar">
          <button id="btn-emoji" class="icon-btn" title="表情">😊</button>
          <button id="btn-image" class="icon-btn" title="图片">🖼</button>
          <button id="btn-file" class="icon-btn" title="文件">📎</button>
        </div>
        <textarea id="input" placeholder=""></textarea>
      </div>
    </section>
  </div>

  <div id="emoji-panel"></div>

  <div id="overlay">
    <div class="modal" id="modal-body"></div>
  </div>

<script>
const state = {
  ready: false,
  revision: 0,
  self: null,
  conversations: [],
  active: "",
  messages: []
};

const EMOJIS = ["😀","😄","😁","😉","😊","😍","🥳","🤔","😎","😭","😡","🥲","😅","🤝","🙏","💪","🎉","❤️","🔥","👍","👀","🤖","🍻","🌈","📌","✅","🌟","💼","🧠","🚀","🐱","🐶"];

function b64e(str) {
  return btoa(unescape(encodeURIComponent(str || "")));
}
function b64d(str) {
  try { return decodeURIComponent(escape(atob(str || ""))); } catch { return ""; }
}

function splitCommandSafe(parts) {
  return parts.join("\t");
}

async function rpc(commandLine) {
  try {
    const raw = await window.native(commandLine);
    return JSON.parse(raw);
  } catch {
    return { ok: false, error: "rpc failed" };
  }
}

function initials(name, id) {
  if (name && name.trim().length) {
    return name.trim().slice(0, 1).toUpperCase();
  }
  return (id || "?").slice(0, 1).toUpperCase();
}

function avatarGradient(seed) {
  let h = 0;
  for (const c of (seed || "x")) h = (h * 31 + c.charCodeAt(0)) % 360;
  const h2 = (h + 40) % 360;
  return `linear-gradient(135deg,hsl(${h},75%,62%),hsl(${h2},72%,48%))`;
}

function renderAvatar(el, name, id, avatarPath) {
  el.innerHTML = "";
  el.style.background = avatarGradient(id || name || "x");
  if (avatarPath) {
    const img = document.createElement("img");
    img.src = `file:///${avatarPath.replace(/\\\\/g, "/")}`;
    img.onerror = () => { el.textContent = initials(name, id); };
    el.appendChild(img);
  } else {
    el.textContent = initials(name, id);
  }
}

function renderSessions() {
  const box = document.getElementById("sessions");
  box.innerHTML = "";
  state.conversations.forEach(c => {
    const row = document.createElement("div");
    row.className = "session-item" + (state.active === c.id ? " active" : "");
    row.onclick = async () => {
      state.active = c.id;
      await rpc(splitCommandSafe(["open", c.id]));
      await pull(true);
    };

    const av = document.createElement("div");
    av.className = "avatar";
    renderAvatar(av, c.name, c.id, c.avatar);

    const main = document.createElement("div");
    main.className = "s-main";

    const top = document.createElement("div");
    top.className = "s-name-line";
    const name = document.createElement("div");
    name.className = "s-name";
    name.textContent = c.name || c.id;
    const dot = document.createElement("div");
    dot.className = "status-dot" + (c.online ? " online" : "");
    top.appendChild(name);
    top.appendChild(dot);

    const last = document.createElement("div");
    last.className = "s-last";
    last.textContent = c.last || "";

    main.appendChild(top);
    main.appendChild(last);

    const badge = document.createElement("div");
    badge.className = "badge";
    badge.style.visibility = c.unread > 0 ? "visible" : "hidden";
    badge.textContent = c.unread > 99 ? "99+" : String(c.unread || "");

    row.appendChild(av);
    row.appendChild(main);
    row.appendChild(badge);
    box.appendChild(row);
  });
}

function renderMessages() {
  const empty = document.getElementById("chat-empty");
  const wrap = document.getElementById("msg-wrap");

  if (!state.active) {
    empty.style.display = "grid";
    wrap.style.display = "none";
    document.getElementById("peer-name").textContent = "";
    document.getElementById("peer-state").textContent = "";
    document.getElementById("peer-avatar").textContent = "";
    return;
  }

  const peer = state.conversations.find(v => v.id === state.active);
  if (peer) {
    document.getElementById("peer-name").textContent = peer.name || peer.id;
    document.getElementById("peer-state").textContent = peer.online ? "在线" : "离线";
    renderAvatar(document.getElementById("peer-avatar"), peer.name, peer.id, peer.avatar);
  }

  empty.style.display = "none";
  wrap.style.display = "block";
  wrap.innerHTML = "";

  state.messages.forEach(m => {
    const row = document.createElement("div");
    row.className = "msg-row" + (m.out ? " out" : "");

    const bubble = document.createElement("div");
    bubble.className = "bubble";

    if (m.kind === "text" || m.kind === "emoji") {
      bubble.textContent = m.text || "";
    } else if (m.kind === "image") {
      const img = document.createElement("img");
      img.className = "img-msg";
      img.src = m.uri || "";
      img.alt = m.fileName || "image";
      img.onerror = () => {
        bubble.textContent = `[图片] ${m.fileName || ""}`;
      };
      bubble.appendChild(img);
    } else {
      const link = document.createElement("a");
      link.className = "file-link";
      link.textContent = `[文件] ${m.fileName || ""}`;
      link.href = m.uri || "#";
      link.target = "_blank";
      bubble.appendChild(link);
    }

    row.appendChild(bubble);
    wrap.appendChild(row);
  });

  wrap.scrollTop = wrap.scrollHeight;
}

function renderSelf() {
  if (!state.self) return;
  renderAvatar(document.getElementById("self-avatar"), state.self.name, state.self.id, state.self.avatar);
}

async function pull(force = false) {
  const res = await rpc(splitCommandSafe(["snapshot", state.active || "", String(state.revision || 0)]));
  if (!res.ok) return;
  if (!force && !res.changed) return;

  state.revision = res.revision || state.revision;
  state.self = res.self || state.self;
  state.conversations = res.conversations || [];
  state.active = res.active || "";
  state.messages = res.messages || [];

  renderSelf();
  renderSessions();
  renderMessages();
}

function showModal(html) {
  const ov = document.getElementById("overlay");
  const body = document.getElementById("modal-body");
  body.innerHTML = html;
  ov.style.display = "flex";
}

function closeModal() {
  document.getElementById("overlay").style.display = "none";
}

function bindSplitters() {
  const app = document.documentElement;
  const line2 = document.getElementById("line2");
  line2.addEventListener("mousedown", e => {
    const startX = e.clientX;
    const start = parseInt(getComputedStyle(app).getPropertyValue("--mid-width"), 10) || 320;
    const move = ev => {
      const w = Math.max(240, Math.min(420, start + ev.clientX - startX));
      app.style.setProperty("--mid-width", `${w}px`);
    };
    const up = () => {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
  });

  const line3 = document.getElementById("line3");
  line3.addEventListener("mousedown", e => {
    const startY = e.clientY;
    const start = parseInt(getComputedStyle(app).getPropertyValue("--composer-height"), 10) || 150;
    const move = ev => {
      const h = Math.max(120, Math.min(280, start - (ev.clientY - startY)));
      app.style.setProperty("--composer-height", `${h}px`);
    };
    const up = () => {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
  });
}

function bindEmojiPanel() {
  const panel = document.getElementById("emoji-panel");
  panel.innerHTML = "";
  EMOJIS.forEach(e => {
    const btn = document.createElement("button");
    btn.className = "emoji-btn";
    btn.textContent = e;
    btn.onclick = () => {
      const input = document.getElementById("input");
      input.value += e;
      panel.style.display = "none";
      input.focus();
    };
    panel.appendChild(btn);
  });

  document.getElementById("btn-emoji").onclick = () => {
    panel.style.display = panel.style.display === "grid" ? "none" : "grid";
  };

  document.addEventListener("click", ev => {
    if (!panel.contains(ev.target) && ev.target.id !== "btn-emoji") {
      panel.style.display = "none";
    }
  });
}

async function doSendText(emoji = false) {
  if (!state.active) return;
  const input = document.getElementById("input");
  const txt = input.value;
  if (!txt.trim()) return;
  input.value = "";
  await rpc(splitCommandSafe([emoji ? "send_emoji" : "send_text", state.active, b64e(txt)]));
  await pull(true);
}

async function doPickAndSend(image) {
  if (!state.active) return;
  const picked = await rpc(image ? "pick_image" : "pick_file");
  if (!picked.ok || !picked.path) return;
  await rpc(splitCommandSafe([image ? "send_image" : "send_file", state.active, picked.path]));
  await pull(true);
}

function bindComposer() {
  const input = document.getElementById("input");
  input.addEventListener("keydown", async e => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      await doSendText(false);
    }
  });

  input.addEventListener("dragover", e => {
    e.preventDefault();
  });

  input.addEventListener("drop", async e => {
    e.preventDefault();
    if (!state.active) return;
    const files = Array.from(e.dataTransfer?.files || []);
    for (const f of files) {
      const path = f.path || "";
      if (!path) continue;
      const low = path.toLowerCase();
      const isImage = [".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp"].some(ext => low.endsWith(ext));
      await rpc(splitCommandSafe([isImage ? "send_image" : "send_file", state.active, b64e(path)]));
    }
    await pull(true);
  });

  document.getElementById("btn-file").onclick = () => doPickAndSend(false);
  document.getElementById("btn-image").onclick = () => doPickAndSend(true);
}

function bindProfile() {
  document.getElementById("btn-profile").onclick = async () => {
    if (!state.active) return;
    const res = await rpc(splitCommandSafe(["peer_profile", state.active]));
    if (!res.ok) return;
    const p = res.peer;
    showModal(`
      <h3>好友资料</h3>
      <div class="f-row"><label>识别码</label><input value="${(p.id || "").replace(/"/g, "&quot;")}" readonly /></div>
      <div class="f-row"><label>IP</label><input value="${(p.ip || "").replace(/"/g, "&quot;")}" readonly /></div>
      <div class="f-row"><label>备注</label><input id="remark-input" value="${(p.remark || "").replace(/"/g, "&quot;")}" /></div>
      <div class="actions">
        <button class="btn" id="modal-cancel">取消</button>
        <button class="btn primary" id="modal-save">保存</button>
      </div>
    `);
    document.getElementById("modal-cancel").onclick = closeModal;
    document.getElementById("modal-save").onclick = async () => {
      const remark = document.getElementById("remark-input").value || "";
      await rpc(splitCommandSafe(["set_remark", state.active, b64e(remark)]));
      closeModal();
      await pull(true);
    };
  };
}

function bindSettings() {
  document.getElementById("btn-settings").onclick = () => {
    const name = state.self?.name || "";
    showModal(`
      <h3>设置</h3>
      <div class="f-row"><label>昵称</label><input id="self-name" value="${name.replace(/"/g, "&quot;")}" /></div>
      <div class="f-row"><label>头像</label><input id="self-avatar-path" value="${(state.self?.avatar || "").replace(/"/g, "&quot;")}" readonly /></div>
      <div class="actions">
        <button class="btn" id="pick-avatar">选择头像</button>
        <button class="btn" id="modal-cancel">取消</button>
        <button class="btn primary" id="modal-save">保存</button>
      </div>
    `);

    document.getElementById("modal-cancel").onclick = closeModal;
    document.getElementById("pick-avatar").onclick = async () => {
      const picked = await rpc("pick_image");
      if (picked.ok && picked.path) {
        const p = b64d(picked.path);
        document.getElementById("self-avatar-path").value = p;
      }
    };

    document.getElementById("modal-save").onclick = async () => {
      const newName = document.getElementById("self-name").value || "";
      const avatarPath = document.getElementById("self-avatar-path").value || "";
      if (newName.trim()) {
        await rpc(splitCommandSafe(["set_name", b64e(newName.trim())]));
      }
      await rpc(splitCommandSafe(["set_avatar", b64e(avatarPath)]));
      closeModal();
      await pull(true);
    };
  };

  document.getElementById("overlay").addEventListener("click", e => {
    if (e.target.id === "overlay") closeModal();
  });
}

async function init() {
  bindSplitters();
  bindEmojiPanel();
  bindComposer();
  bindProfile();
  bindSettings();

  const boot = await rpc("bootstrap");
  if (boot.ok) {
    state.ready = true;
    state.revision = boot.revision || 0;
    state.self = boot.self || null;
    state.conversations = boot.conversations || [];
    state.active = boot.active || "";
    state.messages = boot.messages || [];
    renderSelf();
    renderSessions();
    renderMessages();
  }

  setInterval(() => {
    if (state.ready) {
      pull(false);
    }
  }, 900);
}

init();
</script>
</body>
</html>
)HTML";

} // namespace lantalk
