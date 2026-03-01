#include "app_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#if defined(_WIN32)
#define NOMINMAX
#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lantalk {
namespace {

constexpr char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr int k_discovery_port = 43999;
constexpr std::int64_t k_presence_ttl_sec = 20;
constexpr std::int64_t k_announce_every_sec = 3;

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t k_invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t k_invalid_socket = -1;
#endif

bool is_b64(unsigned char c) {
  return std::isalnum(c) || c == '+' || c == '/';
}

void close_socket(socket_t sock) {
#if defined(_WIN32)
  closesocket(sock);
#else
  close(sock);
#endif
}

} // namespace

app_core::app_core() {
  base_dir_ = exe_dir();
  data_dir_ = (std::filesystem::path(base_dir_) / "data").string();
  chats_dir_ = (std::filesystem::path(data_dir_) / "chats").string();
  profile_file_ = (std::filesystem::path(data_dir_) / "profile.txt").string();
  peers_file_ = (std::filesystem::path(data_dir_) / "peers.txt").string();
}

app_core::~app_core() {
  stop_discovery();
}

void app_core::boot() {
  {
    std::scoped_lock lk(mu_);
    ensure_dirs();
    load_profile();
    load_peers();
    load_chats();
  }
  start_discovery();
}

std::string app_core::self_json() {
  std::scoped_lock lk(mu_);
  std::ostringstream out;
  out << "{";
  out << "\"id\":\"" << json_escape(self_id_) << "\",";
  out << "\"name\":\"" << json_escape(self_name_) << "\"";
  out << "}";
  return out.str();
}

std::string app_core::conversations_json() {
  std::scoped_lock lk(mu_);
  refresh_online_locked(now_unix());
  std::vector<peer> rows;
  rows.reserve(peers_.size());
  for (const auto &it : peers_) {
    rows.push_back(it.second);
  }

  std::sort(rows.begin(), rows.end(), [](const peer &a, const peer &b) {
    const bool a_has_chat = a.last_sent_at > 0;
    const bool b_has_chat = b.last_sent_at > 0;
    if (a_has_chat != b_has_chat) {
      return a_has_chat;
    }
    if (a_has_chat && b_has_chat && a.last_sent_at != b.last_sent_at) {
      return a.last_sent_at > b.last_sent_at;
    }
    if (!a_has_chat && !b_has_chat && a.last_seen_at != b.last_seen_at) {
      return a.last_seen_at > b.last_seen_at;
    }
    return a.name < b.name;
  });

  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto &p = rows[i];
    if (i) out << ",";
    out << "{";
    out << "\"id\":\"" << json_escape(p.id) << "\",";
    out << "\"name\":\"" << json_escape(p.name) << "\",";
    out << "\"online\":" << (p.online ? "true" : "false") << ",";
    out << "\"lastSeenAt\":" << p.last_seen_at << ",";
    out << "\"lastSentAt\":" << p.last_sent_at << ",";
    out << "\"lastText\":\"" << json_escape(p.last_text) << "\"";
    out << "}";
  }
  out << "]";
  return out.str();
}

std::string app_core::messages_json(const std::string &peer_id) {
  const auto id = sanitize_id(peer_id);
  std::scoped_lock lk(mu_);
  auto it = chats_.find(id);
  if (it == chats_.end()) return "[]";
  const auto &items = it->second;

  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto &m = items[i];
    if (i) out << ",";
    out << "{";
    out << "\"id\":\"" << json_escape(m.id) << "\",";
    out << "\"peerId\":\"" << json_escape(m.peer_id) << "\",";
    out << "\"outgoing\":" << (m.outgoing ? "true" : "false") << ",";
    out << "\"sentAt\":" << m.sent_at << ",";
    out << "\"text\":\"" << json_escape(m.text) << "\"";
    out << "}";
  }
  out << "]";
  return out.str();
}

bool app_core::rename_self(const std::string &name) {
  const auto clean = trim(name);
  if (clean.empty()) return false;
  std::scoped_lock lk(mu_);
  self_name_ = clean;
  save_profile();
  return true;
}

bool app_core::send_text(const std::string &peer_id, const std::string &text) {
  const auto id = sanitize_id(peer_id);
  const auto body = trim(text);
  if (id.empty() || body.empty()) return false;

  std::scoped_lock lk(mu_);
  if (!peers_.contains(id)) {
    peer p;
    p.id = id;
    p.name = id;
    p.online = false;
    p.last_seen_at = now_unix();
    peers_[id] = p;
  }

  message msg;
  msg.id = make_msg_id();
  msg.peer_id = id;
  msg.outgoing = true;
  msg.sent_at = now_unix();
  msg.text = body;

  chats_[id].push_back(msg);
  auto &p = peers_[id];
  p.last_text = body;
  p.last_sent_at = msg.sent_at;

  save_chat(id);
  save_peers();
  return true;
}

std::string app_core::trim(const std::string &in) {
  std::size_t s = 0;
  while (s < in.size() && std::isspace(static_cast<unsigned char>(in[s]))) ++s;
  std::size_t e = in.size();
  while (e > s && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
  return in.substr(s, e - s);
}

std::string app_core::to_upper(std::string in) {
  std::transform(in.begin(), in.end(), in.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return in;
}

std::string app_core::json_escape(const std::string &in) {
  std::ostringstream out;
  for (const unsigned char c : in) {
    switch (c) {
    case '"': out << "\\\""; break;
    case '\\': out << "\\\\"; break;
    case '\b': out << "\\b"; break;
    case '\f': out << "\\f"; break;
    case '\n': out << "\\n"; break;
    case '\r': out << "\\r"; break;
    case '\t': out << "\\t"; break;
    default:
      if (c < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
      } else {
        out << static_cast<char>(c);
      }
      break;
    }
  }
  return out.str();
}

std::int64_t app_core::now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

std::string app_core::random_hex(std::size_t bytes) {
  std::random_device rd;
  std::mt19937_64 eng(rd());
  std::uniform_int_distribution<int> dist(0, 255);

  std::ostringstream out;
  for (std::size_t i = 0; i < bytes; ++i) {
    out << std::hex << std::setw(2) << std::setfill('0') << dist(eng);
  }
  return to_upper(out.str());
}

std::string app_core::sanitize_id(const std::string &raw) {
  std::string out;
  out.reserve(raw.size());
  for (const auto ch : raw) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::toupper(c)));
    }
  }
  return out;
}

std::string app_core::b64_encode(const std::string &in) {
  std::string out;
  int val = 0;
  int valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(k_b64[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(k_b64[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string app_core::b64_decode(const std::string &in) {
  std::vector<int> t(256, -1);
  for (int i = 0; i < 64; i++) t[k_b64[i]] = i;
  std::string out;
  int val = 0;
  int valb = -8;
  for (unsigned char c : in) {
    if (c == '=') break;
    if (!is_b64(c)) continue;
    val = (val << 6) + t[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(char((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

std::string app_core::exe_dir() {
#if defined(_WIN32)
  char path[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path().string();
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) == 0) {
    return std::filesystem::path(buf.c_str()).parent_path().string();
  }
  return std::filesystem::current_path().string();
#else
  std::string buf(4096, '\0');
  const auto n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (n > 0) {
    buf.resize(static_cast<std::size_t>(n));
    return std::filesystem::path(buf).parent_path().string();
  }
  return std::filesystem::current_path().string();
#endif
}

std::vector<std::string> app_core::split_tab(const std::string &line) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  return parts;
}

void app_core::ensure_dirs() {
  std::filesystem::create_directories(data_dir_);
  std::filesystem::create_directories(chats_dir_);
}

void app_core::load_profile() {
  std::ifstream in(profile_file_);
  if (!in.good()) {
    self_id_ = random_hex(8);
    self_name_ = "LanTalk 用户";
    save_profile();
    return;
  }

  std::string line;
  while (std::getline(in, line)) {
    auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    auto key = trim(line.substr(0, pos));
    auto val = trim(line.substr(pos + 1));
    if (key == "id") self_id_ = sanitize_id(val);
    if (key == "name") self_name_ = val;
  }

  if (self_id_.empty()) self_id_ = random_hex(8);
  if (self_name_.empty()) self_name_ = "LanTalk 用户";
  save_profile();
}

void app_core::save_profile() {
  std::ofstream out(profile_file_, std::ios::trunc);
  out << "id=" << self_id_ << "\n";
  out << "name=" << self_name_ << "\n";
}

void app_core::load_peers() {
  peers_.clear();
  std::ifstream in(peers_file_);
  if (!in.good()) return;

  std::string line;
  while (std::getline(in, line)) {
    auto p = split_tab(line);
    if (p.size() < 6) continue;
    peer item;
    item.id = sanitize_id(p[0]);
    item.name = p[1].empty() ? item.id : p[1];
    item.online = (p[2] == "1");
    item.last_seen_at = std::stoll(p[3].empty() ? "0" : p[3]);
    item.last_text = b64_decode(p[4]);
    item.last_sent_at = std::stoll(p[5].empty() ? "0" : p[5]);
    if (!item.id.empty()) peers_[item.id] = item;
  }
}

void app_core::save_peers() {
  std::ofstream out(peers_file_, std::ios::trunc);
  for (const auto &it : peers_) {
    const auto &p = it.second;
    out << p.id << "\t"
        << p.name << "\t"
        << (p.online ? "1" : "0") << "\t"
        << p.last_seen_at << "\t"
        << b64_encode(p.last_text) << "\t"
        << p.last_sent_at << "\n";
  }
}

void app_core::load_chats() {
  chats_.clear();
  if (!std::filesystem::exists(chats_dir_)) return;

  for (const auto &entry : std::filesystem::directory_iterator(chats_dir_)) {
    if (!entry.is_regular_file()) continue;
    auto id = sanitize_id(entry.path().stem().string());
    if (id.empty()) continue;

    std::ifstream in(entry.path());
    if (!in.good()) continue;

    std::string line;
    std::vector<message> items;
    while (std::getline(in, line)) {
      auto p = split_tab(line);
      if (p.size() < 4) continue;
      message m;
      m.id = p[0];
      m.peer_id = id;
      m.outgoing = (p[1] == "1");
      m.sent_at = std::stoll(p[2].empty() ? "0" : p[2]);
      m.text = b64_decode(p[3]);
      items.push_back(std::move(m));
    }
    chats_[id] = std::move(items);
  }
}

void app_core::save_chat(const std::string &peer_id) {
  const auto id = sanitize_id(peer_id);
  if (id.empty()) return;
  auto it = chats_.find(id);
  if (it == chats_.end()) return;

  const auto path = (std::filesystem::path(chats_dir_) / (id + ".log")).string();
  std::ofstream out(path, std::ios::trunc);
  for (const auto &m : it->second) {
    out << m.id << "\t"
        << (m.outgoing ? "1" : "0") << "\t"
        << m.sent_at << "\t"
        << b64_encode(m.text) << "\n";
  }
}

void app_core::refresh_online_locked(std::int64_t now) {
  for (auto &it : peers_) {
    auto &p = it.second;
    p.online = (p.last_seen_at > 0 && (now - p.last_seen_at) <= k_presence_ttl_sec);
  }
}

void app_core::upsert_peer_seen(const std::string &id, const std::string &name, std::int64_t seen_at) {
  if (id.empty()) return;
  bool need_save = false;
  {
    std::scoped_lock lk(mu_);
    if (id == self_id_) return;
    auto it = peers_.find(id);
    if (it == peers_.end()) {
      peer p;
      p.id = id;
      p.name = name.empty() ? id : name;
      p.online = true;
      p.last_seen_at = seen_at;
      peers_[id] = p;
      need_save = true;
    } else {
      auto &p = it->second;
      if (!name.empty() && p.name != name) {
        p.name = name;
        need_save = true;
      }
      p.online = true;
      p.last_seen_at = seen_at;
    }
  }
  if (need_save) {
    std::scoped_lock lk(mu_);
    save_peers();
  }
}

void app_core::start_discovery() {
  if (running_.exchange(true)) {
    return;
  }
  discover_thread_ = std::thread([this]() { discovery_loop(); });
}

void app_core::stop_discovery() {
  if (!running_.exchange(false)) {
    return;
  }
  if (discover_thread_.joinable()) {
    discover_thread_.join();
  }
}

void app_core::discovery_loop() {
#if defined(_WIN32)
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return;
  }
#endif

  socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == k_invalid_socket) {
#if defined(_WIN32)
    WSACleanup();
#endif
    return;
  }

  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&yes), sizeof(yes));
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(static_cast<std::uint16_t>(k_discovery_port));
  if (bind(sock, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
    close_socket(sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    return;
  }

  std::int64_t last_announce = 0;
  std::int64_t last_online_refresh = 0;

  while (running_.load()) {
    const auto now = now_unix();
    if (now - last_announce >= k_announce_every_sec) {
      std::string self_id;
      std::string self_name;
      {
        std::scoped_lock lk(mu_);
        self_id = self_id_;
        self_name = self_name_;
      }
      const std::string payload = "LT_DISCOVERY\t" + self_id + "\t" + b64_encode(self_name) + "\t" + std::to_string(now);
      sockaddr_in bcast{};
      bcast.sin_family = AF_INET;
      bcast.sin_port = htons(static_cast<std::uint16_t>(k_discovery_port));
      bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
      sendto(sock, payload.c_str(), static_cast<int>(payload.size()), 0, reinterpret_cast<const sockaddr *>(&bcast), sizeof(bcast));
      last_announce = now;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    const int ready = select(static_cast<int>(sock + 1), &rfds, nullptr, nullptr, &tv);
    if (ready > 0 && FD_ISSET(sock, &rfds)) {
      char buf[1024] = {0};
      sockaddr_in from{};
#if defined(_WIN32)
      int from_len = sizeof(from);
#else
      socklen_t from_len = sizeof(from);
#endif
      const int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
      if (n > 0) {
        const std::string line(buf, static_cast<std::size_t>(n));
        auto parts = split_tab(line);
        if (parts.size() >= 4 && parts[0] == "LT_DISCOVERY") {
          const auto id = sanitize_id(parts[1]);
          const auto name = trim(b64_decode(parts[2]));
          std::int64_t seen = now;
          try {
            seen = std::stoll(parts[3]);
          } catch (...) {
            seen = now;
          }
          upsert_peer_seen(id, name, seen);
        }
      }
    }

    if (now - last_online_refresh >= 2) {
      bool need_save = false;
      {
        std::scoped_lock lk(mu_);
        for (auto &it : peers_) {
          auto &p = it.second;
          const bool was_online = p.online;
          p.online = (p.last_seen_at > 0 && (now - p.last_seen_at) <= k_presence_ttl_sec);
          if (was_online != p.online && !p.online) {
            need_save = true;
          }
        }
        if (need_save) {
          save_peers();
        }
      }
      last_online_refresh = now;
    }
  }

  close_socket(sock);
#if defined(_WIN32)
  WSACleanup();
#endif
}

std::string app_core::make_msg_id() const {
  return random_hex(6);
}

} // namespace lantalk
