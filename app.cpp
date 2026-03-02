#include "app.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <WinSock2.h>
#include <Windows.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lantalk {
namespace {

constexpr std::uint16_t kDiscoveryPort = 47819;
constexpr std::uint16_t kListenPortStart = 47820;
constexpr std::uint16_t kListenPortEnd = 47860;
constexpr char kMulticastAddr[] = "239.255.66.77";
constexpr std::int64_t kPresenceIntervalMs = 2000;
constexpr std::int64_t kOfflineAfterMs = 7000;
constexpr std::size_t kMaxFrameBytes = 1024 * 1024;

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

void close_socket(socket_t s) {
  if (s == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
}

bool send_all(socket_t sock, const std::uint8_t *buf, std::size_t n) {
  std::size_t sent = 0;
  while (sent < n) {
#if defined(_WIN32)
    const int chunk = send(sock, reinterpret_cast<const char *>(buf + sent), static_cast<int>(n - sent), 0);
#else
    const ssize_t chunk = send(sock, buf + sent, n - sent, 0);
#endif
    if (chunk <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(chunk);
  }
  return true;
}

bool recv_all(socket_t sock, std::uint8_t *buf, std::size_t n) {
  std::size_t got = 0;
  while (got < n) {
#if defined(_WIN32)
    const int chunk = recv(sock, reinterpret_cast<char *>(buf + got), static_cast<int>(n - got), 0);
#else
    const ssize_t chunk = recv(sock, buf + got, n - got, 0);
#endif
    if (chunk <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(chunk);
  }
  return true;
}

bool send_frame(socket_t sock, const std::string &payload) {
  if (payload.size() > kMaxFrameBytes) {
    return false;
  }

  std::array<std::uint8_t, 4> hdr{};
  const auto len = static_cast<std::uint32_t>(payload.size());
  hdr[0] = static_cast<std::uint8_t>(len & 0xff);
  hdr[1] = static_cast<std::uint8_t>((len >> 8) & 0xff);
  hdr[2] = static_cast<std::uint8_t>((len >> 16) & 0xff);
  hdr[3] = static_cast<std::uint8_t>((len >> 24) & 0xff);

  return send_all(sock, hdr.data(), hdr.size()) &&
      send_all(sock, reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
}

bool recv_frame(socket_t sock, std::string &payload) {
  std::array<std::uint8_t, 4> hdr{};
  if (!recv_all(sock, hdr.data(), hdr.size())) {
    return false;
  }

  const std::uint32_t len = static_cast<std::uint32_t>(hdr[0]) |
      (static_cast<std::uint32_t>(hdr[1]) << 8) |
      (static_cast<std::uint32_t>(hdr[2]) << 16) |
      (static_cast<std::uint32_t>(hdr[3]) << 24);

  if (len == 0 || len > kMaxFrameBytes) {
    return false;
  }

  payload.resize(len);
  return recv_all(sock, reinterpret_cast<std::uint8_t *>(payload.data()), payload.size());
}

bool parse_u16(const std::string &s, std::uint16_t &out) {
  try {
    const auto v = std::stoul(s);
    if (v > 65535) {
      return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

std::string default_name() {
  std::array<char, 128> buf{};
  if (gethostname(buf.data(), static_cast<int>(buf.size() - 1)) == 0) {
    std::string n(buf.data());
    if (!n.empty()) {
      return n;
    }
  }
  return "LanTalk";
}

} // namespace

struct App::SocketState {
#if defined(_WIN32)
  WSADATA wsa{};
  bool wsa_ready = false;
#endif
  socket_t udp_recv = kInvalidSocket;
  socket_t udp_send = kInvalidSocket;
  socket_t tcp_listen = kInvalidSocket;
  std::uint16_t listen_port = 0;
  std::thread discovery_thread;
  std::thread accept_thread;
};

struct App::DataSlotLock {
  std::filesystem::path dir;
#if defined(_WIN32)
  HANDLE handle = nullptr;
#else
  int fd = -1;
#endif

  ~DataSlotLock() {
#if defined(_WIN32)
    if (handle != nullptr) {
      OVERLAPPED ov{};
      UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &ov);
      CloseHandle(handle);
      handle = nullptr;
    }
#else
    if (fd >= 0) {
      flock(fd, LOCK_UN);
      close(fd);
      fd = -1;
    }
#endif
  }

  bool try_lock(const std::filesystem::path &lock_file) {
#if defined(_WIN32)
    handle = CreateFileW(lock_file.c_str(),
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr,
                         OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      handle = nullptr;
      return false;
    }
    OVERLAPPED ov{};
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov)) {
      CloseHandle(handle);
      handle = nullptr;
      return false;
    }
    return true;
#else
    fd = open(lock_file.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      close(fd);
      fd = -1;
      return false;
    }
    return true;
#endif
  }
};

App::App() = default;

App::~App() {
  shutdown();
}

bool App::boot(std::string &error) {
  work_dir_ = std::filesystem::current_path();
  if (!init_data_dir(error)) {
    return false;
  }
  if (!init_profile(error)) {
    return false;
  }

  load_peers();
  load_chats();

  running_.store(true);
  if (!start_network(error)) {
    running_.store(false);
    return false;
  }

  bump_revision();
  return true;
}

void App::shutdown() {
  if (!running_.exchange(false)) {
    return;
  }
  stop_network();
  save_profile();
  save_peers();
}

bool App::init_data_dir(std::string &error) {
  for (int i = 0; i < 64; ++i) {
    const std::string name = (i == 0) ? "data" : ("data(" + std::to_string(i) + ")");
    auto candidate = work_dir_ / name;

    std::error_code ec;
    std::filesystem::create_directories(candidate, ec);
    if (ec) {
      continue;
    }

    auto lock = std::make_unique<DataSlotLock>();
    if (!lock->try_lock(candidate / ".slot.lock")) {
      continue;
    }

    lock->dir = candidate;
    data_lock_ = std::move(lock);
    data_dir_ = candidate;
    return true;
  }

  error = "cannot lock data directory slot";
  return false;
}

bool App::init_profile(std::string &error) {
  const auto file = data_dir_ / "profile.txt";

  if (std::filesystem::exists(file)) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
      const auto pos = line.find('=');
      if (pos == std::string::npos) {
        continue;
      }
      const auto key = trim(line.substr(0, pos));
      const auto val = line.substr(pos + 1);
      if (key == "id") {
        self_id_ = trim(val);
      } else if (key == "name") {
        self_name_ = b64_decode(val);
      }
    }
  }

  if (self_id_.empty()) {
    std::array<std::uint8_t, 16> id_bytes{};
    if (!random_bytes(id_bytes.data(), id_bytes.size())) {
      error = "failed to generate identity";
      return false;
    }
    self_id_ = hex_encode(id_bytes.data(), id_bytes.size());
  }
  if (self_name_.empty()) {
    self_name_ = default_name();
  }

  save_profile();
  return true;
}

void App::save_profile() {
  const auto file = data_dir_ / "profile.txt";
  std::ofstream out(file, std::ios::trunc);
  out << "id=" << self_id_ << '\n';
  out << "name=" << b64_encode(self_name_) << '\n';
}

void App::load_peers() {
  const auto file = data_dir_ / "peers.tsv";
  if (!std::filesystem::exists(file)) {
    return;
  }

  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }

    const auto parts = split(line, '\t');
    if (parts.size() < 5) {
      continue;
    }

    Peer p;
    p.id = parts[0];
    p.name = b64_decode(parts[1]);
    p.first_seen_ms = std::stoll(parts[2]);
    p.last_seen_ms = std::stoll(parts[3]);
    p.last_chat_ms = std::stoll(parts[4]);
    p.online = false;

    if (!p.id.empty()) {
      peers_[p.id] = std::move(p);
    }
  }
}

void App::save_peers() {
  const auto file = data_dir_ / "peers.tsv";
  std::ofstream out(file, std::ios::trunc);
  for (const auto &[id, p] : peers_) {
    out << id << '\t'
        << b64_encode(p.name) << '\t'
        << p.first_seen_ms << '\t'
        << p.last_seen_ms << '\t'
        << p.last_chat_ms << '\n';
  }
}

void App::load_chats() {
  for (const auto &entry : std::filesystem::directory_iterator(data_dir_)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto fn = entry.path().filename().string();
    if (fn.rfind("chat_", 0) != 0 || fn.size() < 10 || fn.substr(fn.size() - 4) != ".log") {
      continue;
    }

    const std::string peer_id = fn.substr(5, fn.size() - 9);
    std::ifstream in(entry.path());
    std::string line;

    while (std::getline(in, line)) {
      const auto parts = split(line, '\t');
      if (parts.size() < 4) {
        continue;
      }

      Message m;
      m.ts_ms = std::stoll(parts[0]);
      m.outgoing = parts[1] == "1";
      m.text = b64_decode(parts[2]);
      m.id = parts[3];
      chats_[peer_id].push_back(m);

      auto &peer = peers_[peer_id];
      if (peer.id.empty()) {
        peer.id = peer_id;
        peer.name = peer_id;
        peer.first_seen_ms = m.ts_ms;
      }
      peer.last_chat_ms = std::max(peer.last_chat_ms, m.ts_ms);
    }
  }
}

void App::append_chat(const std::string &peer_id, const Message &m) {
  const auto file = data_dir_ / ("chat_" + peer_id + ".log");
  std::ofstream out(file, std::ios::app);
  out << m.ts_ms << '\t'
      << (m.outgoing ? "1" : "0") << '\t'
      << b64_encode(m.text) << '\t'
      << m.id << '\n';
}

bool App::start_network(std::string &error) {
  sockets_ = std::make_unique<SocketState>();

#if defined(_WIN32)
  if (WSAStartup(MAKEWORD(2, 2), &sockets_->wsa) != 0) {
    error = "WSAStartup failed";
    return false;
  }
  sockets_->wsa_ready = true;
#endif

  for (std::uint16_t p = kListenPortStart; p <= kListenPortEnd; ++p) {
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) {
      continue;
    }

    int yes = 1;
#if defined(_WIN32)
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(p);

    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
        listen(s, 32) == 0) {
      sockets_->tcp_listen = s;
      sockets_->listen_port = p;
      break;
    }

    close_socket(s);
  }

  if (sockets_->tcp_listen == kInvalidSocket) {
    error = "cannot bind tcp port";
    stop_network();
    return false;
  }

  sockets_->udp_recv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockets_->udp_send = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockets_->udp_recv == kInvalidSocket || sockets_->udp_send == kInvalidSocket) {
    error = "cannot create udp socket";
    stop_network();
    return false;
  }

  int yes = 1;
#if defined(_WIN32)
  setsockopt(sockets_->udp_recv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
  setsockopt(sockets_->udp_send, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
  setsockopt(sockets_->udp_recv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(sockets_->udp_send, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#endif

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(kDiscoveryPort);
  if (bind(sockets_->udp_recv, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) != 0) {
    error = "cannot bind discovery port";
    stop_network();
    return false;
  }

  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr(kMulticastAddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
#if defined(_WIN32)
  setsockopt(sockets_->udp_recv,
             IPPROTO_IP,
             IP_ADD_MEMBERSHIP,
             reinterpret_cast<const char *>(&mreq),
             sizeof(mreq));
#else
  setsockopt(sockets_->udp_recv, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
#endif

  sockets_->discovery_thread = std::thread([this]() { discovery_loop(); });
  sockets_->accept_thread = std::thread([this]() { accept_loop(); });

  return true;
}

void App::stop_network() {
  if (sockets_ == nullptr) {
    return;
  }

  close_socket(sockets_->udp_recv);
  sockets_->udp_recv = kInvalidSocket;
  close_socket(sockets_->udp_send);
  sockets_->udp_send = kInvalidSocket;
  close_socket(sockets_->tcp_listen);
  sockets_->tcp_listen = kInvalidSocket;

  if (sockets_->discovery_thread.joinable()) {
    sockets_->discovery_thread.join();
  }
  if (sockets_->accept_thread.joinable()) {
    sockets_->accept_thread.join();
  }

#if defined(_WIN32)
  if (sockets_->wsa_ready) {
    WSACleanup();
    sockets_->wsa_ready = false;
  }
#endif

  sockets_.reset();
}

void App::announce_presence() {
  if (sockets_ == nullptr || sockets_->udp_send == kInvalidSocket) {
    return;
  }

  std::string packet;
  std::vector<std::string> known_ips;

  {
    std::lock_guard<std::mutex> lock(mu_);
    packet = "DISC\t" + self_id_ + "\t" + b64_encode(self_name_) + "\t" + std::to_string(sockets_->listen_port);
    known_ips.reserve(peers_.size());
    for (const auto &[id, p] : peers_) {
      if (!p.ip.empty()) {
        known_ips.push_back(p.ip);
      }
    }
  }

  sockaddr_in baddr{};
  baddr.sin_family = AF_INET;
  baddr.sin_port = htons(kDiscoveryPort);
  baddr.sin_addr.s_addr = inet_addr("255.255.255.255");
  sendto(sockets_->udp_send,
         packet.data(),
         static_cast<int>(packet.size()),
         0,
         reinterpret_cast<sockaddr *>(&baddr),
         sizeof(baddr));

  sockaddr_in maddr{};
  maddr.sin_family = AF_INET;
  maddr.sin_port = htons(kDiscoveryPort);
  maddr.sin_addr.s_addr = inet_addr(kMulticastAddr);
  sendto(sockets_->udp_send,
         packet.data(),
         static_cast<int>(packet.size()),
         0,
         reinterpret_cast<sockaddr *>(&maddr),
         sizeof(maddr));

  for (const auto &ip : known_ips) {
    sockaddr_in uaddr{};
    uaddr.sin_family = AF_INET;
    uaddr.sin_port = htons(kDiscoveryPort);
    if (inet_pton(AF_INET, ip.c_str(), &uaddr.sin_addr) != 1) {
      continue;
    }
    sendto(sockets_->udp_send,
           packet.data(),
           static_cast<int>(packet.size()),
           0,
           reinterpret_cast<sockaddr *>(&uaddr),
           sizeof(uaddr));
  }
}

void App::consume_presence(const std::string &payload, const std::string &source_ip) {
  const auto parts = split(payload, '\t');
  if (parts.size() < 4 || parts[0] != "DISC") {
    return;
  }

  const auto peer_id = trim(parts[1]);
  if (peer_id.empty() || peer_id == self_id_) {
    return;
  }

  std::uint16_t port = 0;
  if (!parse_u16(parts[3], port)) {
    return;
  }

  const auto peer_name = b64_decode(parts[2]);
  const auto now = now_ms();

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto &peer = peers_[peer_id];
    if (peer.id.empty()) {
      peer.id = peer_id;
      peer.name = peer_name.empty() ? peer_id : peer_name;
      peer.first_seen_ms = now;
      changed = true;
    }

    if (!peer_name.empty() && peer.name != peer_name) {
      peer.name = peer_name;
      changed = true;
    }
    if (peer.ip != source_ip || peer.port != port) {
      peer.ip = source_ip;
      peer.port = port;
      changed = true;
    }
    if (!peer.online) {
      peer.online = true;
      changed = true;
    }
    peer.last_seen_ms = now;
  }

  if (changed) {
    save_peers();
    bump_revision();
  }
}

void App::discovery_loop() {
  auto next_announce = now_ms();

  while (running_.load()) {
    const auto now = now_ms();
    if (now >= next_announce) {
      announce_presence();
      next_announce = now + kPresenceIntervalMs;

      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto &[id, peer] : peers_) {
          const bool should_online = (now - peer.last_seen_ms) <= kOfflineAfterMs;
          if (peer.online != should_online) {
            peer.online = should_online;
            changed = true;
          }
        }
      }
      if (changed) {
        bump_revision();
      }
    }

    if (sockets_ == nullptr || sockets_->udp_recv == kInvalidSocket) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sockets_->udp_recv, &rfds);

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000;

    const int rc = select(static_cast<int>(sockets_->udp_recv + 1), &rfds, nullptr, nullptr, &tv);
    if (rc <= 0 || !FD_ISSET(sockets_->udp_recv, &rfds)) {
      continue;
    }

    std::array<char, 1024> buf{};
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const int n = recvfrom(sockets_->udp_recv,
                           buf.data(),
                           static_cast<int>(buf.size() - 1),
                           0,
                           reinterpret_cast<sockaddr *>(&from),
                           &from_len);
    if (n <= 0) {
      continue;
    }

    buf[static_cast<std::size_t>(n)] = '\0';
    char ip[64] = {0};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    consume_presence(std::string(buf.data(), static_cast<std::size_t>(n)), ip);
  }
}

void App::accept_loop() {
  while (running_.load()) {
    if (sockets_ == nullptr || sockets_->tcp_listen == kInvalidSocket) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }

    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    socket_t client = accept(sockets_->tcp_listen, reinterpret_cast<sockaddr *>(&from), &from_len);
    if (client == kInvalidSocket) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    char ip[64] = {0};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    std::thread([this, c = static_cast<std::intptr_t>(client), sip = std::string(ip)]() {
      handle_client(c, sip);
      close_socket(static_cast<socket_t>(c));
    }).detach();
  }
}

void App::handle_client(std::intptr_t sock_value, const std::string &source_ip) {
  const auto sock = static_cast<socket_t>(sock_value);

  while (running_.load()) {
    std::string payload;
    if (!recv_frame(sock, payload)) {
      return;
    }

    const auto parts = split(payload, '\t');
    if (parts.size() < 5 || parts[0] != "MSG") {
      continue;
    }

    const auto sender_id = trim(parts[1]);
    if (sender_id.empty() || sender_id == self_id_) {
      continue;
    }

    const auto sender_name = b64_decode(parts[2]);
    const auto text = b64_decode(parts[4]);

    std::int64_t ts = now_ms();
    try {
      ts = std::stoll(parts[3]);
    } catch (...) {
    }

    Message msg;
    msg.ts_ms = ts;
    msg.outgoing = false;
    msg.text = text;
    std::array<std::uint8_t, 8> mid{};
    random_bytes(mid.data(), mid.size());
    msg.id = hex_encode(mid.data(), mid.size());

    {
      std::lock_guard<std::mutex> lock(mu_);
      auto &peer = peers_[sender_id];
      if (peer.id.empty()) {
        peer.id = sender_id;
        peer.name = sender_name.empty() ? sender_id : sender_name;
        peer.first_seen_ms = ts;
      }
      if (!sender_name.empty()) {
        peer.name = sender_name;
      }
      if (!source_ip.empty()) {
        peer.ip = source_ip;
      }
      if (peer.port == 0 && sockets_ != nullptr) {
        peer.port = sockets_->listen_port;
      }
      peer.online = true;
      peer.last_seen_ms = now_ms();
      peer.last_chat_ms = msg.ts_ms;
      if (active_peer_ != sender_id) {
        peer.unread += 1;
      }

      chats_[sender_id].push_back(msg);
    }

    append_chat(sender_id, msg);
    save_peers();
    bump_revision();
  }
}

bool App::send_text_to_peer(const std::string &peer_id, const std::string &text, std::string &error) {
  if (text.empty()) {
    error = "empty message";
    return false;
  }

  Peer peer;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
      error = "peer not found";
      return false;
    }
    peer = it->second;
  }

  if (peer.ip.empty() || peer.port == 0) {
    error = "peer offline";
    return false;
  }

  socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == kInvalidSocket) {
    error = "cannot create socket";
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(peer.port);
  if (inet_pton(AF_INET, peer.ip.c_str(), &addr.sin_addr) != 1) {
    close_socket(sock);
    error = "invalid peer ip";
    return false;
  }
  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close_socket(sock);
    error = "connect failed";
    return false;
  }

  const auto ts = now_ms();
  const std::string payload =
      "MSG\t" + self_id_ + "\t" + b64_encode(self_name_) + "\t" + std::to_string(ts) + "\t" + b64_encode(text);

  if (!send_frame(sock, payload)) {
    close_socket(sock);
    error = "send failed";
    return false;
  }

  close_socket(sock);

  Message msg;
  msg.ts_ms = ts;
  msg.outgoing = true;
  msg.text = text;
  std::array<std::uint8_t, 8> mid{};
  random_bytes(mid.data(), mid.size());
  msg.id = hex_encode(mid.data(), mid.size());

  {
    std::lock_guard<std::mutex> lock(mu_);
    chats_[peer_id].push_back(msg);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
      it->second.last_chat_ms = ts;
    }
  }

  append_chat(peer_id, msg);
  save_peers();
  bump_revision();
  return true;
}

std::string App::rpc_bootstrap() {
  return snapshot_json(active_peer_, 0);
}

std::string App::rpc_snapshot(const std::vector<std::string> &parts) {
  const auto active = parts.size() > 1 ? parts[1] : active_peer_;
  std::uint64_t known = 0;
  if (parts.size() > 2) {
    try {
      known = std::stoull(parts[2]);
    } catch (...) {
      known = 0;
    }
  }
  return snapshot_json(active, known);
}

std::string App::rpc_open(const std::vector<std::string> &parts) {
  if (parts.size() < 2) {
    return R"({"ok":false,"error":"missing peer id"})";
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    active_peer_ = parts[1];
    auto it = peers_.find(active_peer_);
    if (it != peers_.end()) {
      it->second.unread = 0;
    }
  }

  save_peers();
  bump_revision();
  return R"({"ok":true})";
}

std::string App::rpc_send(const std::vector<std::string> &parts) {
  if (parts.size() < 3) {
    return R"({"ok":false,"error":"invalid args"})";
  }

  const auto peer_id = parts[1];
  const auto text = b64_decode(parts[2]);

  std::string error;
  if (!send_text_to_peer(peer_id, text, error)) {
    return std::string("{\"ok\":false,\"error\":\"") + json_escape(error) + "\"}";
  }

  return R"({"ok":true})";
}

std::string App::rpc_set_name(const std::vector<std::string> &parts) {
  if (parts.size() < 2) {
    return R"({"ok":false,"error":"invalid args"})";
  }

  const auto name = trim(b64_decode(parts[1]));
  if (name.empty()) {
    return R"({"ok":false,"error":"empty name"})";
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    self_name_ = name;
  }

  save_profile();
  announce_presence();
  bump_revision();
  return R"({"ok":true})";
}

std::string App::rpc(const std::string &line) {
  const auto parts = split(line, '\t');
  if (parts.empty()) {
    return R"({"ok":false,"error":"empty command"})";
  }

  const auto &cmd = parts[0];
  if (cmd == "bootstrap") {
    return rpc_bootstrap();
  }
  if (cmd == "snapshot") {
    return rpc_snapshot(parts);
  }
  if (cmd == "open") {
    return rpc_open(parts);
  }
  if (cmd == "send") {
    return rpc_send(parts);
  }
  if (cmd == "set_name") {
    return rpc_set_name(parts);
  }

  return R"({"ok":false,"error":"unknown command"})";
}

std::string App::snapshot_json(const std::string &active_peer, std::uint64_t known_revision) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_peer.empty()) {
      active_peer_ = active_peer;
      auto it = peers_.find(active_peer_);
      if (it != peers_.end()) {
        it->second.unread = 0;
      }
    }
  }

  const auto rev = revision_.load();

  std::ostringstream os;
  os << "{\"ok\":true,";
  os << "\"revision\":" << rev << ',';
  os << "\"changed\":" << (rev != known_revision ? "true" : "false") << ',';
  os << "\"self\":" << self_json() << ',';
  os << "\"peers\":" << peers_json() << ',';
  os << "\"active\":\"" << json_escape(active_peer_) << "\",";
  os << "\"messages\":" << messages_json(active_peer_);
  os << '}';
  return os.str();
}

std::string App::self_json() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ostringstream os;
  os << '{';
  os << "\"id\":\"" << json_escape(self_id_) << "\",";
  os << "\"name\":\"" << json_escape(self_name_) << "\",";
  os << "\"workDir\":\"" << json_escape(work_dir_.string()) << "\",";
  os << "\"dataDir\":\"" << json_escape(data_dir_.string()) << "\"";
  os << '}';
  return os.str();
}

std::string App::peers_json() const {
  std::lock_guard<std::mutex> lock(mu_);

  std::vector<std::reference_wrapper<const Peer>> sorted;
  sorted.reserve(peers_.size());
  for (const auto &[id, p] : peers_) {
    if (id == self_id_) {
      continue;
    }
    sorted.push_back(std::cref(p));
  }

  std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
    const auto &pa = a.get();
    const auto &pb = b.get();

    const bool a_chatted = pa.last_chat_ms > 0;
    const bool b_chatted = pb.last_chat_ms > 0;
    if (a_chatted != b_chatted) {
      return a_chatted > b_chatted;
    }
    if (a_chatted && b_chatted && pa.last_chat_ms != pb.last_chat_ms) {
      return pa.last_chat_ms > pb.last_chat_ms;
    }
    if (pa.last_seen_ms != pb.last_seen_ms) {
      return pa.last_seen_ms > pb.last_seen_ms;
    }
    return pa.id < pb.id;
  });

  std::ostringstream os;
  os << '[';

  bool first = true;
  for (const auto &ref : sorted) {
    const auto &p = ref.get();

    std::string last_text;
    std::int64_t last_ts = p.last_chat_ms > 0 ? p.last_chat_ms : p.last_seen_ms;
    const auto it = chats_.find(p.id);
    if (it != chats_.end() && !it->second.empty()) {
      last_text = it->second.back().text;
      last_ts = it->second.back().ts_ms;
    }

    if (!first) {
      os << ',';
    }
    first = false;

    os << '{';
    os << "\"id\":\"" << json_escape(p.id) << "\",";
    os << "\"name\":\"" << json_escape(p.name.empty() ? p.id : p.name) << "\",";
    os << "\"ip\":\"" << json_escape(p.ip) << "\",";
    os << "\"online\":" << (p.online ? "true" : "false") << ',';
    os << "\"unread\":" << p.unread << ',';
    os << "\"lastTs\":" << last_ts << ',';
    os << "\"last\":\"" << json_escape(last_text) << "\"";
    os << '}';
  }

  os << ']';
  return os.str();
}

std::string App::messages_json(const std::string &peer_id) const {
  if (peer_id.empty()) {
    return "[]";
  }

  std::lock_guard<std::mutex> lock(mu_);
  const auto it = chats_.find(peer_id);
  if (it == chats_.end()) {
    return "[]";
  }

  std::ostringstream os;
  os << '[';
  bool first = true;
  for (const auto &m : it->second) {
    if (!first) {
      os << ',';
    }
    first = false;

    os << '{';
    os << "\"id\":\"" << json_escape(m.id) << "\",";
    os << "\"ts\":" << m.ts_ms << ',';
    os << "\"time\":\"" << now_local_time(m.ts_ms) << "\",";
    os << "\"out\":" << (m.outgoing ? "true" : "false") << ',';
    os << "\"text\":\"" << json_escape(m.text) << "\"";
    os << '}';
  }
  os << ']';
  return os.str();
}

std::vector<std::string> App::split(const std::string &in, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : in) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

std::string App::trim(const std::string &in) {
  std::size_t s = 0;
  while (s < in.size() && std::isspace(static_cast<unsigned char>(in[s])) != 0) {
    ++s;
  }
  std::size_t e = in.size();
  while (e > s && std::isspace(static_cast<unsigned char>(in[e - 1])) != 0) {
    --e;
  }
  return in.substr(s, e - s);
}

std::string App::json_escape(const std::string &in) {
  std::ostringstream os;
  for (const auto c : in) {
    switch (c) {
    case '"': os << "\\\""; break;
    case '\\': os << "\\\\"; break;
    case '\n': os << "\\n"; break;
    case '\r': os << "\\r"; break;
    case '\t': os << "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
           << static_cast<int>(static_cast<unsigned char>(c)) << std::dec;
      } else {
        os << c;
      }
      break;
    }
  }
  return os.str();
}

std::string App::b64_encode(const std::string &in) {
  static constexpr char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);

  std::size_t i = 0;
  while (i + 2 < in.size()) {
    const std::uint32_t n = (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 2]));

    out.push_back(kChars[(n >> 18) & 63]);
    out.push_back(kChars[(n >> 12) & 63]);
    out.push_back(kChars[(n >> 6) & 63]);
    out.push_back(kChars[n & 63]);
    i += 3;
  }

  if (i < in.size()) {
    const std::uint32_t a = static_cast<unsigned char>(in[i]);
    const std::uint32_t b = (i + 1 < in.size()) ? static_cast<unsigned char>(in[i + 1]) : 0;
    const std::uint32_t n = (a << 16) | (b << 8);

    out.push_back(kChars[(n >> 18) & 63]);
    out.push_back(kChars[(n >> 12) & 63]);
    out.push_back(i + 1 < in.size() ? kChars[(n >> 6) & 63] : '=');
    out.push_back('=');
  }

  return out;
}

std::string App::b64_decode(const std::string &in) {
  static std::array<int, 256> table = [] {
    std::array<int, 256> t{};
    t.fill(-1);
    const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < static_cast<int>(chars.size()); ++i) {
      t[static_cast<unsigned char>(chars[static_cast<std::size_t>(i)])] = i;
    }
    return t;
  }();

  std::string out;
  int val = 0;
  int bits = -8;

  for (const auto c : in) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    if (c == '=') {
      break;
    }
    const int d = table[static_cast<unsigned char>(c)];
    if (d < 0) {
      continue;
    }

    val = (val << 6) | d;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xff));
      bits -= 8;
    }
  }

  return out;
}

bool App::random_bytes(std::uint8_t *dst, std::size_t n) {
  try {
    std::random_device rd;
    for (std::size_t i = 0; i < n; ++i) {
      dst[i] = static_cast<std::uint8_t>(rd() & 0xff);
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::string App::hex_encode(const std::uint8_t *data, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out[2 * i] = kHex[(data[i] >> 4) & 0x0f];
    out[2 * i + 1] = kHex[data[i] & 0x0f];
  }
  return out;
}

std::string App::now_local_time(std::int64_t ts_ms) {
  const auto sec = static_cast<std::time_t>(ts_ms / 1000);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &sec);
#else
  localtime_r(&sec, &local);
#endif

  std::ostringstream os;
  os << std::put_time(&local, "%H:%M");
  return os.str();
}

std::int64_t App::now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void App::bump_revision() {
  revision_.fetch_add(1);
}

} // namespace lantalk
