const state = {
  config: loadConfig(),
  contacts: new Map(),
  activeId: "",
  pullAfter: new Map(),
};

const selfAvatar = document.getElementById("selfAvatar");
const contactList = document.getElementById("contactList");
const chatTitle = document.getElementById("chatTitle");
const messages = document.getElementById("messages");
const inputBox = document.getElementById("inputBox");
const sendBtn = document.getElementById("sendBtn");
const searchInput = document.getElementById("searchInput");

const settingsDialog = document.getElementById("settingsDialog");
const openSettingsBtn = document.getElementById("openSettingsBtn");
const cancelSettingsBtn = document.getElementById("cancelSettingsBtn");
const saveSettingsBtn = document.getElementById("saveSettingsBtn");
const nameInput = document.getElementById("nameInput");
const idInput = document.getElementById("idInput");
const serversInput = document.getElementById("serversInput");

function loadConfig() {
  const raw = localStorage.getItem("lantalk_web_config");
  if (raw) {
    try {
      return JSON.parse(raw);
    } catch {
      // ignore
    }
  }
  return {
    userId: crypto.randomUUID().replace(/-/g, "").slice(0, 16),
    name: `Web用户${Math.floor(Math.random() * 9000 + 1000)}`,
    avatar: randomAvatar(),
    servers: [],
  };
}

function saveConfig() {
  localStorage.setItem("lantalk_web_config", JSON.stringify(state.config));
}

function randomAvatar() {
  const hue = Math.floor(Math.random() * 360);
  const canvas = document.createElement("canvas");
  canvas.width = 80;
  canvas.height = 80;
  const ctx = canvas.getContext("2d");
  const grad = ctx.createLinearGradient(0, 0, 80, 80);
  grad.addColorStop(0, `hsl(${hue},70%,62%)`);
  grad.addColorStop(1, `hsl(${(hue + 30) % 360},66%,52%)`);
  ctx.fillStyle = grad;
  roundRect(ctx, 0, 0, 80, 80, 18);
  ctx.fill();
  ctx.fillStyle = "rgba(255,255,255,0.9)";
  ctx.beginPath();
  ctx.arc(40, 30, 12, 0, Math.PI * 2);
  ctx.fill();
  ctx.beginPath();
  ctx.roundRect?.(16, 46, 48, 24, 12);
  if (!ctx.roundRect) {
    roundRect(ctx, 16, 46, 48, 24, 12);
  }
  ctx.fill();
  return canvas.toDataURL("image/png");
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

function normalizeServers(lines) {
  const out = [];
  const seen = new Set();
  for (let v of lines) {
    v = String(v || "").trim();
    if (!v) continue;
    if (!v.includes("://")) v = `https://${v}`;
    v = v.replace(/\/+$/, "");
    try {
      const u = new URL(v);
      const n = `${u.protocol}//${u.host}`;
      if (!seen.has(n)) {
        seen.add(n);
        out.push(n);
      }
    } catch {
      // ignore invalid URL
    }
  }
  return out;
}

function ensureContact(userId) {
  if (!state.contacts.has(userId)) {
    state.contacts.set(userId, {
      userId,
      name: userId,
      avatar: randomAvatar(),
      status: "gray",
      modeText: "离线",
      server: "",
      messages: [],
      lastSeenMs: 0,
    });
  }
  return state.contacts.get(userId);
}

function renderContacts() {
  const keyword = searchInput.value.trim().toLowerCase();
  const list = [...state.contacts.values()].sort((a, b) => {
    const aTs = a.messages.length ? a.messages[a.messages.length - 1].timestampMs : a.lastSeenMs;
    const bTs = b.messages.length ? b.messages[b.messages.length - 1].timestampMs : b.lastSeenMs;
    return bTs - aTs;
  });

  contactList.innerHTML = "";
  for (const c of list) {
    const hay = `${c.name}\n${c.userId}`.toLowerCase();
    if (keyword && !hay.includes(keyword)) continue;
    const el = document.createElement("div");
    el.className = `contact-item ${state.activeId === c.userId ? "active" : ""}`;
    el.innerHTML = `
      <img class="avatar-sm" src="${c.avatar}" alt="avatar" />
      <div class="name-wrap">
        <div class="name-line"><span>${escapeHtml(c.name)}</span><span class="dot ${c.status}"></span></div>
        <div class="meta-line">${escapeHtml(c.modeText)}</div>
      </div>
    `;
    el.onclick = () => {
      state.activeId = c.userId;
      renderContacts();
      renderConversation();
    };
    contactList.appendChild(el);
  }
}

function renderConversation() {
  const c = state.contacts.get(state.activeId);
  if (!c) {
    chatTitle.textContent = "聊天窗口";
    messages.innerHTML = `<div style="color:#6b7280;padding:26px 20px;font-size:16px;">请选择联系人开始聊天。</div>`;
    return;
  }
  chatTitle.textContent = c.name;
  messages.innerHTML = "";
  for (const m of c.messages) {
    const row = document.createElement("div");
    row.className = "msg-row";
    const d = new Date(m.timestampMs);
    const t = `${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")} ${String(
      d.getHours()
    ).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;

    row.innerHTML = `
      <div class="msg-meta" style="text-align:${m.incoming ? "left" : "right"};padding:${
      m.incoming ? "0 0 0 42px" : "0 42px 0 0"
    }">${m.incoming ? escapeHtml(c.name) : "我"}  ${t}</div>
      <div class="msg-content ${m.incoming ? "in" : "out"}">
        ${m.incoming ? `<img class="avatar-sm" src="${c.avatar}" alt="avatar" />` : ""}
        <div class="bubble ${m.incoming ? "in" : "out"}">${escapeHtml(m.text)}</div>
        ${!m.incoming ? `<img class="avatar-sm" src="${state.config.avatar}" alt="avatar" />` : ""}
      </div>
    `;
    messages.appendChild(row);
  }
  messages.scrollTop = messages.scrollHeight;
}

function escapeHtml(v) {
  return String(v)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;")
    .replaceAll("\n", "<br/>");
}

async function requestJson(url, method = "GET", body = null) {
  const res = await fetch(url, {
    method,
    headers: { "content-type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

async function refreshPresence() {
  const servers = state.config.servers;
  if (!servers.length) {
    for (const c of state.contacts.values()) {
      c.status = "gray";
      c.modeText = "离线";
      c.server = "";
    }
    renderContacts();
    return;
  }

  const now = Date.now();
  const p2p = new Set();
  const ws = new Set();
  const byServer = new Map();

  for (const server of servers) {
    try {
      await requestJson(`${server}/v1/presence`, "POST", {
        userId: state.config.userId,
        name: state.config.name,
        avatarPayload: state.config.avatar,
        listenPort: 39001,
        e2eePublic: "0",
        localIps: [],
      });

      const peersData = await requestJson(`${server}/v1/peers?userId=${encodeURIComponent(state.config.userId)}`);
      for (const peer of peersData.peers || []) {
        const userId = String(peer.userId || "").trim();
        if (!userId || userId === state.config.userId) continue;
        const c = ensureContact(userId);
        c.name = String(peer.name || userId);
        c.avatar = String(peer.avatarPayload || randomAvatar());
        c.lastSeenMs = now;

        const mode = String(peer.mode || "ws");
        if (mode === "p2p") {
          p2p.add(userId);
          ws.delete(userId);
          byServer.set(userId, server);
        } else if (!p2p.has(userId)) {
          ws.add(userId);
          if (!byServer.has(userId)) byServer.set(userId, server);
        }
      }
    } catch {
      // ignore this server cycle
    }
  }

  for (const c of state.contacts.values()) {
    if (p2p.has(c.userId)) {
      c.status = "blue";
      c.modeText = "打洞在线";
      c.server = byServer.get(c.userId) || "";
    } else if (ws.has(c.userId)) {
      c.status = "orange";
      c.modeText = "WS兜底在线";
      c.server = byServer.get(c.userId) || "";
    } else {
      c.status = "gray";
      c.modeText = "离线";
      c.server = "";
    }
  }

  renderContacts();
}

async function pullMessages() {
  for (const server of state.config.servers) {
    const after = state.pullAfter.get(server) || 0;
    try {
      const data = await requestJson(
        `${server}/v1/messages/pull?userId=${encodeURIComponent(state.config.userId)}&after=${after}`
      );
      let maxAfter = after;
      for (const msg of data.messages || []) {
        const fromUserId = String(msg.fromUserId || "").trim();
        const text = String(msg.text || "");
        const ts = Number(msg.timestampMs || Date.now());
        if (!fromUserId || !text) continue;
        const c = ensureContact(fromUserId);
        c.name = String(msg.fromName || fromUserId);
        c.avatar = String(msg.fromAvatar || c.avatar);
        c.messages.push({ incoming: true, text, timestampMs: ts });
        c.lastSeenMs = Date.now();
        if (ts > maxAfter) maxAfter = ts;
      }
      state.pullAfter.set(server, maxAfter);
    } catch {
      // ignore
    }
  }
  renderContacts();
  renderConversation();
}

async function sendMessage() {
  const text = inputBox.value.trim();
  if (!text || !state.activeId) return;
  const contact = state.contacts.get(state.activeId);
  if (!contact || !contact.server) {
    alert("联系人当前离线或未配置信令服务器。");
    return;
  }

  await requestJson(`${contact.server}/v1/messages/send`, "POST", {
    fromUserId: state.config.userId,
    fromName: state.config.name,
    fromAvatar: state.config.avatar,
    toUserId: contact.userId,
    text,
    timestampMs: Date.now(),
  });

  contact.messages.push({ incoming: false, text, timestampMs: Date.now() });
  inputBox.value = "";
  renderContacts();
  renderConversation();
}

openSettingsBtn.onclick = () => {
  nameInput.value = state.config.name;
  idInput.value = state.config.userId;
  serversInput.value = state.config.servers.join("\n");
  settingsDialog.showModal();
};
cancelSettingsBtn.onclick = () => settingsDialog.close();
saveSettingsBtn.onclick = () => {
  state.config.name = nameInput.value.trim() || state.config.name;
  state.config.servers = normalizeServers(serversInput.value.split(/\r?\n/));
  saveConfig();
  settingsDialog.close();
};

sendBtn.onclick = () => {
  sendMessage().catch((e) => alert(`发送失败: ${e.message}`));
};
inputBox.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey) {
    e.preventDefault();
    sendMessage().catch((err) => alert(`发送失败: ${err.message}`));
  }
});
searchInput.addEventListener("input", renderContacts);

selfAvatar.src = state.config.avatar;
renderContacts();
renderConversation();
refreshPresence();
pullMessages();
setInterval(refreshPresence, 3000);
setInterval(pullMessages, 1200);
