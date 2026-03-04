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

const CLASSIC_NAMES = [
  "贾宝玉", "林黛玉", "薛宝钗", "王熙凤",
  "孙悟空", "唐三藏", "猪八戒", "沙悟净",
  "宋江", "武松", "林冲", "鲁智深",
  "诸葛亮", "刘备", "关羽", "张飞", "赵云", "周瑜",
];
const DEFAULT_SIGNAL_SERVER = "https://lantalk-web.pages.dev";
const AVATAR_POOL = Array.from({ length: 104 }, (_, i) => `/avatars/default_${String(i + 1).padStart(3, "0")}.png`);

const state = {
  config: loadConfig(),
  contacts: new Map(),
  activeId: "",
  pullAfter: new Map(),
  selfAvatarPayload: "",
  selfAvatarPayloadKey: "",
};

function loadConfig() {
  const raw = localStorage.getItem("lantalk_web_config");
  if (raw) {
    try {
      const parsed = JSON.parse(raw);
      parsed.userId = String(parsed.userId || "").trim() || crypto.randomUUID().replace(/-/g, "").slice(0, 16);
      parsed.name = String(parsed.name || "").trim() || randomClassicName();
      parsed.avatar = String(parsed.avatar || "").trim() || randomAvatar();
      parsed.servers = normalizeServers(parsed.servers || [DEFAULT_SIGNAL_SERVER]);
      if (!parsed.servers.length) {
        parsed.servers = [DEFAULT_SIGNAL_SERVER];
      }
      return parsed;
    } catch {
      // ignore
    }
  }
  return {
    userId: crypto.randomUUID().replace(/-/g, "").slice(0, 16),
    name: randomClassicName(),
    avatar: randomAvatar(),
    servers: [DEFAULT_SIGNAL_SERVER],
  };
}

function saveConfig() {
  localStorage.setItem("lantalk_web_config", JSON.stringify(state.config));
}

function randomAvatar() {
  const idx = Math.floor(Math.random() * AVATAR_POOL.length);
  return AVATAR_POOL[idx];
}

function randomClassicName() {
  return CLASSIC_NAMES[Math.floor(Math.random() * CLASSIC_NAMES.length)];
}

function hashText(value) {
  let h = 2166136261 >>> 0;
  const text = String(value || "");
  for (let i = 0; i < text.length; i += 1) {
    h ^= text.charCodeAt(i);
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h >>> 0;
}

function stableFallbackAvatar(seed) {
  if (!AVATAR_POOL.length) return "";
  const idx = hashText(seed) % AVATAR_POOL.length;
  return AVATAR_POOL[idx];
}

function isLikelyBase64Payload(text) {
  const raw = String(text || "").trim().replace(/\s+/g, "");
  if (!raw || raw.length < 64 || raw.length % 4 !== 0) return false;
  return /^[A-Za-z0-9+/]+={0,2}$/.test(raw);
}

function avatarPayloadToSrc(value, fallbackSeed = "") {
  const raw = String(value || "").trim();
  const fallback = stableFallbackAvatar(fallbackSeed || state.config.userId || state.config.name || "avatar");
  if (!raw) return fallback;
  if (raw.startsWith("data:image/")) return raw;
  if (raw.startsWith(":/avatars/")) return raw.slice(1);
  if (raw.startsWith("/avatars/") || raw.startsWith("http://") || raw.startsWith("https://") || raw.startsWith("blob:")) {
    return raw;
  }
  if (isLikelyBase64Payload(raw)) return `data:image/jpeg;base64,${raw.replace(/\s+/g, "")}`;
  return fallback;
}

function encodeAvatarPayloadFromImage(image, size, quality) {
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext("2d");
  const srcSize = Math.min(image.naturalWidth || image.width, image.naturalHeight || image.height);
  const sx = Math.max(0, ((image.naturalWidth || image.width) - srcSize) / 2);
  const sy = Math.max(0, ((image.naturalHeight || image.height) - srcSize) / 2);
  ctx.drawImage(image, sx, sy, srcSize, srcSize, 0, 0, size, size);
  const dataUrl = canvas.toDataURL("image/jpeg", quality);
  const comma = dataUrl.indexOf(",");
  return comma > 0 ? dataUrl.slice(comma + 1) : "";
}

async function buildAvatarPayloadFromSrc(src) {
  const image = await new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error("avatar_load_failed"));
    img.src = src;
  });
  const attempts = [
    [56, 0.68],
    [56, 0.6],
    [50, 0.58],
    [46, 0.56],
    [42, 0.54],
    [38, 0.5],
  ];
  let last = "";
  for (const [size, quality] of attempts) {
    const payload = encodeAvatarPayloadFromImage(image, size, quality);
    if (payload) {
      last = payload;
      if (payload.length <= 3900) {
        return payload;
      }
    }
  }
  return last;
}

async function buildAvatarPayloadFromValue(value) {
  const raw = String(value || "").trim();
  if (!raw) return "";
  if (raw.startsWith("data:image/")) return buildAvatarPayloadFromSrc(raw);
  if (raw.startsWith(":/avatars/")) return buildAvatarPayloadFromSrc(raw.slice(1));
  if (raw.startsWith("/avatars/") || raw.startsWith("http://") || raw.startsWith("https://") || raw.startsWith("blob:")) {
    return buildAvatarPayloadFromSrc(raw);
  }
  if (isLikelyBase64Payload(raw)) {
    return buildAvatarPayloadFromSrc(`data:image/jpeg;base64,${raw.replace(/\s+/g, "")}`);
  }
  return buildAvatarPayloadFromSrc(raw);
}

async function ensureSelfAvatarPayload() {
  let avatar = String(state.config.avatar || "").trim();
  if (!avatar) {
    avatar = stableFallbackAvatar(state.config.userId || state.config.name || "self");
    state.config.avatar = avatar;
    saveConfig();
  }
  if (state.selfAvatarPayload && state.selfAvatarPayloadKey === avatar) return state.selfAvatarPayload;

  try {
    state.selfAvatarPayload = await buildAvatarPayloadFromValue(avatar);
    state.selfAvatarPayloadKey = avatar;
    return state.selfAvatarPayload;
  } catch {
    state.config.avatar = stableFallbackAvatar(state.config.userId || state.config.name || "self");
    saveConfig();
    selfAvatar.src = avatarPayloadToSrc(state.config.avatar, state.config.userId);
    state.selfAvatarPayload = await buildAvatarPayloadFromValue(state.config.avatar);
    state.selfAvatarPayloadKey = state.config.avatar;
    return state.selfAvatarPayload;
  }
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

function apiUrl(serverBase, path) {
  let base = String(serverBase || "").trim().replace(/\/+$/, "");
  if (!base) base = DEFAULT_SIGNAL_SERVER;
  let cleanPath = String(path || "");
  if (!cleanPath.startsWith("/")) cleanPath = `/${cleanPath}`;

  try {
    const u = new URL(base);
    if (u.pathname && u.pathname !== "/") {
      base = `${u.protocol}//${u.host}${u.pathname.replace(/\/+$/, "")}`;
    } else {
      base = `${u.protocol}//${u.host}`;
    }
    if (base.endsWith("/api")) {
      return `${base}${cleanPath}`;
    }
    if (u.host.endsWith(".pages.dev")) {
      return `${base}/api${cleanPath}`;
    }
    return `${base}${cleanPath}`;
  } catch {
    return `${DEFAULT_SIGNAL_SERVER}/api${cleanPath}`;
  }
}

function ensureContact(userId) {
  if (!state.contacts.has(userId)) {
    state.contacts.set(userId, {
      userId,
      name: randomClassicName(),
      avatar: stableFallbackAvatar(userId),
      avatarPayload: "",
      status: "gray",
      modeText: "离线",
      server: "",
      messages: [],
      lastSeenMs: 0,
    });
  }
  return state.contacts.get(userId);
}

function pruneEphemeralOfflineContacts() {
  let changed = false;
  for (const [userId, contact] of state.contacts.entries()) {
    if (contact.status === "gray" && (!contact.messages || contact.messages.length === 0)) {
      state.contacts.delete(userId);
      if (state.activeId === userId) {
        state.activeId = "";
      }
      changed = true;
    }
  }
  return changed;
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
    const avatarSrc = avatarPayloadToSrc(c.avatar, c.userId);
    const avatarFallback = stableFallbackAvatar(c.userId);
    el.innerHTML = `
      <img class="avatar-sm" src="${avatarSrc}" alt="avatar" onerror="this.onerror=null;this.src='${avatarFallback}'" />
      <div class="name-wrap">
        <div class="name-line"><span>${escapeHtml(c.name)}</span><span class="dot ${c.status}"></span></div>
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
    const incomingAvatarSrc = avatarPayloadToSrc(c.avatar, c.userId);
    const incomingAvatarFallback = stableFallbackAvatar(c.userId);
    const selfAvatarSrc = avatarPayloadToSrc(state.config.avatar, state.config.userId);
    const selfAvatarFallback = stableFallbackAvatar(state.config.userId);

    row.innerHTML = `
      <div class="msg-meta" style="text-align:${m.incoming ? "left" : "right"};padding:${
      m.incoming ? "0 0 0 42px" : "0 42px 0 0"
    }">${m.incoming ? escapeHtml(c.name) : "我"}  ${t}</div>
      <div class="msg-content ${m.incoming ? "in" : "out"}">
        ${m.incoming ? `<img class="avatar-sm" src="${incomingAvatarSrc}" alt="avatar" onerror="this.onerror=null;this.src='${incomingAvatarFallback}'" />` : ""}
        <div class="bubble ${m.incoming ? "in" : "out"}">${escapeHtml(m.text)}</div>
        ${!m.incoming ? `<img class="avatar-sm" src="${selfAvatarSrc}" alt="avatar" onerror="this.onerror=null;this.src='${selfAvatarFallback}'" />` : ""}
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
  let selfPayload = "";
  try {
    selfPayload = await ensureSelfAvatarPayload();
  } catch {
    selfPayload = "";
  }
  const servers = state.config.servers;
  if (!servers.length) {
    for (const c of state.contacts.values()) {
      c.status = "gray";
      c.modeText = "离线";
      c.server = "";
    }
    const changed = pruneEphemeralOfflineContacts();
    renderContacts();
    if (changed) {
      renderConversation();
    }
    return;
  }

  const now = Date.now();
  const p2p = new Set();
  const ws = new Set();
  const byServer = new Map();

  for (const server of servers) {
    try {
      await requestJson(apiUrl(server, "/v1/presence"), "POST", {
        userId: state.config.userId,
        name: state.config.name,
        avatarPayload: selfPayload,
        listenPort: 39001,
        e2eePublic: "0",
        localIps: [],
      });

      const peersData = await requestJson(
        `${apiUrl(server, "/v1/peers")}?userId=${encodeURIComponent(state.config.userId)}`
      );
      for (const peer of peersData.peers || []) {
        const userId = String(peer.userId || "").trim();
        if (!userId || userId === state.config.userId) continue;
        const c = ensureContact(userId);
        c.name = String(peer.name || userId);
        const peerAvatarPayload = String(peer.avatarPayload || "").trim();
        if (peerAvatarPayload) {
          c.avatarPayload = peerAvatarPayload;
          c.avatar = avatarPayloadToSrc(peerAvatarPayload, userId);
        }
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
      c.modeText = "P2P在线";
      c.server = byServer.get(c.userId) || "";
    } else if (ws.has(c.userId)) {
      c.status = "orange";
      c.modeText = "WS在线";
      c.server = byServer.get(c.userId) || "";
    } else {
      c.status = "gray";
      c.modeText = "离线";
      c.server = "";
    }
  }

  const changed = pruneEphemeralOfflineContacts();
  renderContacts();
  if (changed) {
    renderConversation();
  }
}

async function pullMessages() {
  for (const server of state.config.servers) {
    const after = state.pullAfter.get(server) || 0;
    try {
      const data = await requestJson(
        `${apiUrl(server, "/v1/messages/pull")}?userId=${encodeURIComponent(state.config.userId)}&after=${after}`
      );
      let maxAfter = after;
      for (const msg of data.messages || []) {
        const fromUserId = String(msg.fromUserId || "").trim();
        const text = String(msg.text || "");
        const ts = Number(msg.timestampMs || Date.now());
        if (!fromUserId || !text) continue;
        const c = ensureContact(fromUserId);
        c.name = String(msg.fromName || fromUserId);
        const fromAvatarPayload = String(msg.fromAvatar || "").trim();
        if (fromAvatarPayload) {
          c.avatarPayload = fromAvatarPayload;
          c.avatar = avatarPayloadToSrc(fromAvatarPayload, fromUserId);
        }
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

  const selfPayload = await ensureSelfAvatarPayload();
  await requestJson(apiUrl(contact.server, "/v1/messages/send"), "POST", {
    fromUserId: state.config.userId,
    fromName: state.config.name,
    fromAvatar: selfPayload,
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
  state.selfAvatarPayload = "";
  state.selfAvatarPayloadKey = "";
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

selfAvatar.src = avatarPayloadToSrc(state.config.avatar, state.config.userId);
renderContacts();
renderConversation();
ensureSelfAvatarPayload().then(() => {
  refreshPresence();
});
pullMessages().catch(() => {});
setInterval(() => {
  refreshPresence().catch(() => {});
}, 3000);
setInterval(() => {
  pullMessages().catch(() => {});
}, 1200);
