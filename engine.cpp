#include "engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
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

constexpr std::uint16_t kDiscoveryPort = 48231;
constexpr std::uint16_t kListenPortStart = 48232;
constexpr std::uint16_t kListenPortEnd = 48296;
constexpr char kMulticastAddr[] = "239.255.88.19";
constexpr std::int64_t kAnnounceMs = 1500;
constexpr std::int64_t kOfflineMs = 6000;
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
  if (payload.empty() || payload.size() > kMaxFrameBytes) {
    return false;
  }

  std::array<std::uint8_t, 4> hdr{};
  const auto n = static_cast<std::uint32_t>(payload.size());
  hdr[0] = static_cast<std::uint8_t>(n & 0xff);
  hdr[1] = static_cast<std::uint8_t>((n >> 8) & 0xff);
  hdr[2] = static_cast<std::uint8_t>((n >> 16) & 0xff);
  hdr[3] = static_cast<std::uint8_t>((n >> 24) & 0xff);

  return send_all(sock, hdr.data(), hdr.size()) &&
      send_all(sock, reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
}

bool recv_frame(socket_t sock, std::string &payload) {
  std::array<std::uint8_t, 4> hdr{};
  if (!recv_all(sock, hdr.data(), hdr.size())) {
    return false;
  }

  const std::uint32_t n = static_cast<std::uint32_t>(hdr[0]) |
      (static_cast<std::uint32_t>(hdr[1]) << 8) |
      (static_cast<std::uint32_t>(hdr[2]) << 16) |
      (static_cast<std::uint32_t>(hdr[3]) << 24);

  if (n == 0 || n > kMaxFrameBytes) {
    return false;
  }

  payload.resize(n);
  return recv_all(sock, reinterpret_cast<std::uint8_t *>(payload.data()), payload.size());
}

bool set_blocking(socket_t sock, bool blocking) {
#if defined(_WIN32)
  u_long mode = blocking ? 0 : 1;
  return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  int next = flags;
  if (blocking) {
    next &= ~O_NONBLOCK;
  } else {
    next |= O_NONBLOCK;
  }
  return fcntl(sock, F_SETFL, next) == 0;
#endif
}

bool connect_with_timeout(socket_t sock, const sockaddr_in &addr, int timeout_ms) {
  if (!set_blocking(sock, false)) {
    return false;
  }

  const int rc = connect(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
  if (rc == 0) {
    set_blocking(sock, true);
    return true;
  }

#if defined(_WIN32)
  const int in_progress = WSAGetLastError();
  if (in_progress != WSAEWOULDBLOCK && in_progress != WSAEINPROGRESS && in_progress != WSAEINVAL) {
    set_blocking(sock, true);
    return false;
  }
#else
  if (errno != EINPROGRESS) {
    set_blocking(sock, true);
    return false;
  }
#endif

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(sock, &wfds);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  const int sel = select(static_cast<int>(sock + 1), nullptr, &wfds, nullptr, &tv);
  if (sel <= 0 || !FD_ISSET(sock, &wfds)) {
    set_blocking(sock, true);
    return false;
  }

  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len) != 0 || err != 0) {
    set_blocking(sock, true);
    return false;
  }

  set_blocking(sock, true);
  return true;
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
  std::array<char, 128> host{};
  if (gethostname(host.data(), static_cast<int>(host.size() - 1)) == 0) {
    std::string out(host.data());
    if (!out.empty()) {
      return out;
    }
  }
  return "LanTalk";
}

} // namespace

struct LanTalkEngine::DataSlotLock {
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

struct LanTalkEngine::SocketState {
#if defined(_WIN32)
  WSADATA wsa{};
  bool wsa_ok = false;
#endif
  socket_t udp_recv = kInvalidSocket;
  socket_t udp_send = kInvalidSocket;
  socket_t tcp_listen = kInvalidSocket;
  std::uint16_t listen_port = 0;
  std::thread discovery_thread;
  std::thread probe_thread;
  std::thread accept_thread;
};

LanTalkEngine::LanTalkEngine() = default;

LanTalkEngine::~LanTalkEngine() {
  shutdown();
}

bool LanTalkEngine::boot(std::string &error) {
  work_dir_ = std::filesystem::current_path();

  if (!init_data_slot(error)) {
    return false;
  }
  if (!init_profile(error)) {
    return false;
  }

  load_peers();
  load_messages();

  running_.store(true);
  if (!start_network(error)) {
    running_.store(false);
    return false;
  }

  return true;
}

void LanTalkEngine::shutdown() {
  if (!running_.exchange(false)) {
    return;
  }
  stop_network();
  save_profile();
  save_peers();
}

bool LanTalkEngine::init_data_slot(std::string &error) {
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
    slot_lock_ = std::move(lock);
    data_dir_ = candidate;
    return true;
  }

  error = "unable to lock data slot";
  return false;
}

bool LanTalkEngine::init_profile(std::string &error) {
  const auto file = data_dir_ / "profile.cfg";

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
      } else if (key == "name_b64") {
        self_name_ = b64_decode(val);
      }
    }
  }

  if (self_id_.empty()) {
    std::array<std::uint8_t, 16> buf{};
    if (!random_bytes(buf.data(), buf.size())) {
      error = "failed to generate id";
      return false;
    }
    self_id_ = hex_encode(buf.data(), buf.size());
  }
  if (self_name_.empty()) {
    self_name_ = default_name();
  }

  save_profile();
  return true;
}

void LanTalkEngine::save_profile() {
  const auto file = data_dir_ / "profile.cfg";
  std::ofstream out(file, std::ios::trunc);
  out << "id=" << self_id_ << '\n';
  out << "name_b64=" << b64_encode(self_name_) << '\n';
}

void LanTalkEngine::load_peers() {
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

void LanTalkEngine::save_peers() {
  std::unordered_map<std::string, Peer> snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot = peers_;
  }

  const auto file = data_dir_ / "peers.tsv";
  std::ofstream out(file, std::ios::trunc);
  for (const auto &[id, p] : snapshot) {
    out << id << '\t'
        << b64_encode(p.name) << '\t'
        << p.first_seen_ms << '\t'
        << p.last_seen_ms << '\t'
        << p.last_chat_ms << '\n';
  }
}

void LanTalkEngine::load_messages() {
  for (const auto &entry : std::filesystem::directory_iterator(data_dir_)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto fn = entry.path().filename().string();
    if (fn.rfind("chat_", 0) != 0 || fn.size() <= 9 || fn.substr(fn.size() - 4) != ".log") {
      continue;
    }

    const auto peer_id = fn.substr(5, fn.size() - 9);
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
      messages_[peer_id].push_back(m);

      auto it = peers_.find(peer_id);
      if (it != peers_.end()) {
        it->second.last_chat_ms = std::max(it->second.last_chat_ms, m.ts_ms);
      }
    }
  }
}

void LanTalkEngine::append_message_to_disk(const std::string &peer_id, const Message &m) {
  const auto file = data_dir_ / ("chat_" + peer_id + ".log");
  std::ofstream out(file, std::ios::app);
  out << m.ts_ms << '\t'
      << (m.outgoing ? "1" : "0") << '\t'
      << b64_encode(m.text) << '\t'
      << m.id << '\n';
}

bool LanTalkEngine::start_network(std::string &error) {
  sockets_ = std::make_unique<SocketState>();

#if defined(_WIN32)
  if (WSAStartup(MAKEWORD(2, 2), &sockets_->wsa) != 0) {
    error = "WSAStartup failed";
    return false;
  }
  sockets_->wsa_ok = true;
#endif

  for (std::uint16_t p = kListenPortStart; p <= kListenPortEnd; ++p) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket) {
      continue;
    }

    int yes = 1;
#if defined(_WIN32)
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(p);

    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 && listen(sock, 32) == 0) {
      sockets_->tcp_listen = sock;
      sockets_->listen_port = p;
      break;
    }

    close_socket(sock);
  }

  if (sockets_->tcp_listen == kInvalidSocket) {
    error = "cannot bind tcp listen port";
    stop_network();
    return false;
  }

  sockets_->udp_send = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockets_->udp_send != kInvalidSocket) {
    int yes = 1;
#if defined(_WIN32)
    setsockopt(sockets_->udp_send, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
    setsockopt(sockets_->udp_send, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#endif
  }

  sockets_->udp_recv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockets_->udp_recv != kInvalidSocket) {
    int yes = 1;
#if defined(_WIN32)
    setsockopt(sockets_->udp_recv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
    setsockopt(sockets_->udp_recv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in recv_addr{};
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    recv_addr.sin_port = htons(kDiscoveryPort);
    if (bind(sockets_->udp_recv, reinterpret_cast<sockaddr *>(&recv_addr), sizeof(recv_addr)) != 0) {
      close_socket(sockets_->udp_recv);
      sockets_->udp_recv = kInvalidSocket;
    } else {
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
    }
  }

  sockets_->discovery_thread = std::thread([this]() { discovery_loop(); });
  sockets_->probe_thread = std::thread([this]() { probe_loop(); });
  sockets_->accept_thread = std::thread([this]() { accept_loop(); });
  return true;
}

void LanTalkEngine::stop_network() {
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
  if (sockets_->probe_thread.joinable()) {
    sockets_->probe_thread.join();
  }
  if (sockets_->accept_thread.joinable()) {
    sockets_->accept_thread.join();
  }

#if defined(_WIN32)
  if (sockets_->wsa_ok) {
    WSACleanup();
    sockets_->wsa_ok = false;
  }
#endif

  sockets_.reset();
}

void LanTalkEngine::announce_presence(bool reply_only, const std::string &target_ip) {
  if (sockets_ == nullptr || sockets_->udp_send == kInvalidSocket) {
    return;
  }

  std::string packet;
  std::vector<std::string> known_ips;
  {
    std::lock_guard<std::mutex> lock(mu_);
    packet = "LT5|" + std::string(reply_only ? "R" : "A") + "|" + self_id_ + "|" + b64_encode(self_name_) + "|" +
        std::to_string(sockets_->listen_port) + "|" + std::to_string(now_ms());

    if (!reply_only) {
      known_ips.reserve(peers_.size());
      for (const auto &[id, p] : peers_) {
        if (!p.ip.empty()) {
          known_ips.push_back(p.ip);
        }
      }
    }
  }

  auto send_to_ip = [&](const std::string &ip, std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
      return;
    }
    sendto(sockets_->udp_send,
           packet.data(),
           static_cast<int>(packet.size()),
           0,
           reinterpret_cast<sockaddr *>(&addr),
           sizeof(addr));
  };

  if (reply_only) {
    if (!target_ip.empty()) {
      send_to_ip(target_ip, kDiscoveryPort);
    }
    return;
  }

  send_to_ip("255.255.255.255", kDiscoveryPort);
  send_to_ip(kMulticastAddr, kDiscoveryPort);
  const auto local = primary_ipv4();
  if (!local.empty()) {
    const auto parts = split(local, '.');
    if (parts.size() == 4) {
      send_to_ip(parts[0] + "." + parts[1] + "." + parts[2] + ".255", kDiscoveryPort);
    }
  }
  for (const auto &ip : known_ips) {
    send_to_ip(ip, kDiscoveryPort);
  }
}

void LanTalkEngine::consume_presence(const std::string &packet, const std::string &source_ip) {
  const auto parts = split(packet, '|');
  if (parts.size() < 6 || parts[0] != "LT5") {
    return;
  }

  const auto kind = parts[1];
  const auto peer_id = trim(parts[2]);
  if (peer_id.empty() || peer_id == self_id_) {
    return;
  }

  std::uint16_t port = 0;
  if (!parse_u16(parts[4], port)) {
    return;
  }

  const auto peer_name = b64_decode(parts[3]);
  const auto tnow = now_ms();

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto &p = peers_[peer_id];
    if (p.id.empty()) {
      p.id = peer_id;
      p.first_seen_ms = tnow;
      p.name = peer_name.empty() ? peer_id : peer_name;
      changed = true;
    }
    if (!peer_name.empty() && p.name != peer_name) {
      p.name = peer_name;
      changed = true;
    }
    if (p.ip != source_ip || p.port != port) {
      p.ip = source_ip;
      p.port = port;
      changed = true;
    }
    if (!p.online) {
      p.online = true;
      changed = true;
    }
    p.last_seen_ms = tnow;
  }

  if (changed) {
    save_peers();
  }

  if (kind == "A") {
    announce_presence(true, source_ip);
  }
}

void LanTalkEngine::discovery_loop() {
  int burst = 3;
  auto next_announce = now_ms();

  while (running_.load()) {
    const auto tnow = now_ms();
    if (burst > 0 || tnow >= next_announce) {
      announce_presence(false);
      if (burst > 0) {
        --burst;
        next_announce = tnow + 350;
      } else {
        next_announce = tnow + kAnnounceMs;
      }
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      for (auto &[id, p] : peers_) {
        p.online = (tnow - p.last_seen_ms) <= kOfflineMs;
      }
    }

    if (sockets_ == nullptr || sockets_->udp_recv == kInvalidSocket) {
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
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

    std::array<char, 1500> buf{};
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    const int n = recvfrom(sockets_->udp_recv,
                           buf.data(),
                           static_cast<int>(buf.size() - 1),
                           0,
                           reinterpret_cast<sockaddr *>(&from),
                           &flen);
    if (n <= 0) {
      continue;
    }

    buf[static_cast<std::size_t>(n)] = '\0';
    char ip[64] = {0};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    consume_presence(std::string(buf.data(), static_cast<std::size_t>(n)), ip);
  }
}

void LanTalkEngine::probe_loop() {
  while (running_.load()) {
    std::set<std::pair<std::string, std::uint16_t>> targets;

    std::string local_ip = primary_ipv4();
    if (!local_ip.empty()) {
      for (std::uint16_t p = kListenPortStart; p <= kListenPortEnd; ++p) {
        targets.insert({local_ip, p});
      }
    }
    for (std::uint16_t p = kListenPortStart; p <= kListenPortEnd; ++p) {
      targets.insert({"127.0.0.1", p});
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      for (const auto &[id, peer] : peers_) {
        if (!peer.ip.empty()) {
          if (peer.port != 0) {
            targets.insert({peer.ip, peer.port});
          }
          targets.insert({peer.ip, kListenPortStart});
        }
      }
    }

    if (!local_ip.empty()) {
      const auto parts = split(local_ip, '.');
      if (parts.size() == 4) {
        for (int i = 0; i < 64; ++i) {
          const int host = static_cast<int>((probe_cursor_ + i) % 254) + 1;
          const auto ip = parts[0] + "." + parts[1] + "." + parts[2] + "." + std::to_string(host);
          if (ip != local_ip) {
            targets.insert({ip, kListenPortStart});
          }
        }
        probe_cursor_ = (probe_cursor_ + 64) % 254;
      }
    }

    for (const auto &[ip, port] : targets) {
      if (!running_.load()) {
        break;
      }
      probe_target(ip, port);
    }

    for (int i = 0; i < 20 && running_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }
}

void LanTalkEngine::probe_target(const std::string &ip, std::uint16_t port) {
  if (ip.empty() || port == 0) {
    return;
  }

  socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == kInvalidSocket) {
    return;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close_socket(sock);
    return;
  }

  if (!connect_with_timeout(sock, addr, 220)) {
    close_socket(sock);
    return;
  }

  std::uint16_t self_port = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (sockets_ != nullptr) {
      self_port = sockets_->listen_port;
    }
  }

  const auto probe = "PROBE\t" + self_id_ + "\t" + b64_encode(self_name_) + "\t" + std::to_string(self_port);
  if (!send_frame(sock, probe)) {
    close_socket(sock);
    return;
  }

  std::string ack;
  if (!recv_frame(sock, ack)) {
    close_socket(sock);
    return;
  }
  close_socket(sock);

  const auto parts = split(ack, '\t');
  if (parts.size() < 4 || parts[0] != "PROBE_ACK") {
    return;
  }

  const auto peer_id = trim(parts[1]);
  if (peer_id.empty() || peer_id == self_id_) {
    return;
  }

  const auto peer_name = b64_decode(parts[2]);
  std::uint16_t peer_port = 0;
  if (!parse_u16(parts[3], peer_port)) {
    peer_port = port;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto &p = peers_[peer_id];
    if (p.id.empty()) {
      p.id = peer_id;
      p.first_seen_ms = now_ms();
    }
    p.name = peer_name.empty() ? (p.name.empty() ? peer_id : p.name) : peer_name;
    p.ip = ip;
    p.port = peer_port;
    p.online = true;
    p.last_seen_ms = now_ms();
  }
  save_peers();
}

void LanTalkEngine::accept_loop() {
  while (running_.load()) {
    if (sockets_ == nullptr || sockets_->tcp_listen == kInvalidSocket) {
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      continue;
    }

    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    socket_t client = accept(sockets_->tcp_listen, reinterpret_cast<sockaddr *>(&from), &from_len);
    if (client == kInvalidSocket) {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      continue;
    }

    char ip[64] = {0};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    std::thread([this, c = static_cast<std::intptr_t>(client), sip = std::string(ip)]() {
      handle_connection(c, sip);
      close_socket(static_cast<socket_t>(c));
    }).detach();
  }
}

void LanTalkEngine::handle_connection(std::intptr_t sock_value, const std::string &source_ip) {
  const auto sock = static_cast<socket_t>(sock_value);

  while (running_.load()) {
    std::string frame;
    if (!recv_frame(sock, frame)) {
      return;
    }

    const auto parts = split(frame, '\t');
    if (parts[0] == "PROBE") {
      if (parts.size() >= 4) {
        const auto sender_id = trim(parts[1]);
        const auto sender_name = b64_decode(parts[2]);
        std::uint16_t sender_port = 0;
        parse_u16(parts[3], sender_port);

        if (!sender_id.empty() && sender_id != self_id_) {
          std::lock_guard<std::mutex> lock(mu_);
          auto &p = peers_[sender_id];
          if (p.id.empty()) {
            p.id = sender_id;
            p.first_seen_ms = now_ms();
          }
          if (!sender_name.empty()) {
            p.name = sender_name;
          } else if (p.name.empty()) {
            p.name = sender_id;
          }
          p.ip = source_ip;
          if (sender_port != 0) {
            p.port = sender_port;
          }
          p.online = true;
          p.last_seen_ms = now_ms();
        }
      }

      std::uint16_t self_port = 0;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (sockets_ != nullptr) {
          self_port = sockets_->listen_port;
        }
      }
      const auto ack =
          "PROBE_ACK\t" + self_id_ + "\t" + b64_encode(self_name_) + "\t" + std::to_string(self_port);
      const auto _ = send_frame(sock, ack);
      (void)_;
      save_peers();
      continue;
    }

    if (parts[0] == "PROBE_ACK") {
      if (parts.size() >= 4) {
        const auto sender_id = trim(parts[1]);
        const auto sender_name = b64_decode(parts[2]);
        std::uint16_t sender_port = 0;
        parse_u16(parts[3], sender_port);
        if (!sender_id.empty() && sender_id != self_id_) {
          std::lock_guard<std::mutex> lock(mu_);
          auto &p = peers_[sender_id];
          if (p.id.empty()) {
            p.id = sender_id;
            p.first_seen_ms = now_ms();
          }
          if (!sender_name.empty()) {
            p.name = sender_name;
          } else if (p.name.empty()) {
            p.name = sender_id;
          }
          p.ip = source_ip;
          if (sender_port != 0) {
            p.port = sender_port;
          }
          p.online = true;
          p.last_seen_ms = now_ms();
        }
      }
      save_peers();
      continue;
    }

    if (parts.size() < 5 || parts[0] != "MSG") {
      continue;
    }

    const auto sender_id = trim(parts[1]);
    if (sender_id.empty() || sender_id == self_id_) {
      continue;
    }

    const auto sender_name = b64_decode(parts[2]);
    std::int64_t ts = now_ms();
    try {
      ts = std::stoll(parts[3]);
    } catch (...) {
      ts = now_ms();
    }
    const auto text = b64_decode(parts[4]);

    std::array<std::uint8_t, 8> bid{};
    random_bytes(bid.data(), bid.size());

    Message m;
    m.id = hex_encode(bid.data(), bid.size());
    m.ts_ms = ts;
    m.outgoing = false;
    m.text = text;

    {
      std::lock_guard<std::mutex> lock(mu_);
      auto &p = peers_[sender_id];
      if (p.id.empty()) {
        p.id = sender_id;
        p.first_seen_ms = ts;
        p.name = sender_name.empty() ? sender_id : sender_name;
      }
      if (!sender_name.empty()) {
        p.name = sender_name;
      }
      p.ip = source_ip;
      p.online = true;
      p.last_seen_ms = now_ms();
      p.last_chat_ms = ts;
      if (active_peer_ != sender_id) {
        p.unread += 1;
      }

      messages_[sender_id].push_back(m);
    }

    append_message_to_disk(sender_id, m);
    save_peers();
  }
}

bool LanTalkEngine::send_text(const std::string &peer_id, const std::string &text, std::string &error) {
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

  if (peer.ip.empty() || peer.port == 0 || text.empty()) {
    error = "peer offline or empty message";
    return false;
  }

  socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == kInvalidSocket) {
    error = "create socket failed";
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
  const std::string frame =
      "MSG\t" + self_id_ + "\t" + b64_encode(self_name_) + "\t" + std::to_string(ts) + "\t" + b64_encode(text);

  if (!send_frame(sock, frame)) {
    close_socket(sock);
    error = "send failed";
    return false;
  }

  close_socket(sock);

  std::array<std::uint8_t, 8> bid{};
  random_bytes(bid.data(), bid.size());

  Message m;
  m.id = hex_encode(bid.data(), bid.size());
  m.ts_ms = ts;
  m.outgoing = true;
  m.text = text;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto &vec = messages_[peer_id];
    vec.push_back(m);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
      it->second.last_chat_ms = ts;
    }
  }

  append_message_to_disk(peer_id, m);
  save_peers();
  return true;
}

std::string LanTalkEngine::rpc_bootstrap() {
  return snapshot_json("");
}

std::string LanTalkEngine::rpc_snapshot(const std::vector<std::string> &parts) {
  if (parts.size() > 1) {
    return snapshot_json(parts[1]);
  }
  return snapshot_json("");
}

std::string LanTalkEngine::rpc_open(const std::vector<std::string> &parts) {
  if (parts.size() < 2) {
    return R"({"ok":false,"error":"missing peer"})";
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
  return R"({"ok":true})";
}

std::string LanTalkEngine::rpc_send(const std::vector<std::string> &parts) {
  if (parts.size() < 3) {
    return R"({"ok":false,"error":"invalid args"})";
  }

  std::string error;
  if (!send_text(parts[1], b64_decode(parts[2]), error)) {
    return std::string("{\"ok\":false,\"error\":\"") + json_escape(error) + "\"}";
  }
  return R"({"ok":true})";
}

std::string LanTalkEngine::rpc_set_name(const std::vector<std::string> &parts) {
  if (parts.size() < 2) {
    return R"({"ok":false,"error":"invalid args"})";
  }

  const auto n = trim(b64_decode(parts[1]));
  if (n.empty()) {
    return R"({"ok":false,"error":"empty name"})";
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    self_name_ = n;
  }

  save_profile();
  announce_presence(false);
  return R"({"ok":true})";
}

std::string LanTalkEngine::handle_rpc(const std::string &line) {
  const auto parts = split(line, '\t');
  if (parts.empty()) {
    return R"({"ok":false,"error":"empty command"})";
  }

  if (parts[0] == "bootstrap") {
    return rpc_bootstrap();
  }
  if (parts[0] == "snapshot") {
    return rpc_snapshot(parts);
  }
  if (parts[0] == "open") {
    return rpc_open(parts);
  }
  if (parts[0] == "send") {
    return rpc_send(parts);
  }
  if (parts[0] == "set_name") {
    return rpc_set_name(parts);
  }

  return R"({"ok":false,"error":"unknown command"})";
}

std::string LanTalkEngine::snapshot_json(const std::string &active_override) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_override.empty()) {
      active_peer_ = active_override;
      auto it = peers_.find(active_peer_);
      if (it != peers_.end()) {
        it->second.unread = 0;
      }
    }
  }

  std::ostringstream os;
  os << "{\"ok\":true,";
  os << "\"self\":" << self_json() << ',';
  os << "\"peers\":" << peers_json() << ',';
  {
    std::lock_guard<std::mutex> lock(mu_);
    os << "\"active\":\"" << json_escape(active_peer_) << "\",";
  }
  std::string active;
  {
    std::lock_guard<std::mutex> lock(mu_);
    active = active_peer_;
  }
  os << "\"messages\":" << messages_json(active);
  os << '}';
  return os.str();
}

std::string LanTalkEngine::self_json() const {
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

std::string LanTalkEngine::peers_json() const {
  std::vector<Peer> list;
  {
    std::lock_guard<std::mutex> lock(mu_);
    list.reserve(peers_.size());
    for (const auto &[id, p] : peers_) {
      if (id != self_id_) {
        list.push_back(p);
      }
    }
  }

  std::sort(list.begin(), list.end(), [](const Peer &a, const Peer &b) {
    const bool a_chat = a.last_chat_ms > 0;
    const bool b_chat = b.last_chat_ms > 0;
    if (a_chat != b_chat) {
      return a_chat > b_chat;
    }
    if (a_chat && b_chat && a.last_chat_ms != b.last_chat_ms) {
      return a.last_chat_ms > b.last_chat_ms;
    }
    if (a.last_seen_ms != b.last_seen_ms) {
      return a.last_seen_ms > b.last_seen_ms;
    }
    return a.id < b.id;
  });

  std::ostringstream os;
  os << '[';
  bool first = true;
  for (const auto &p : list) {
    std::string last_text;
    std::int64_t last_ts = p.last_chat_ms > 0 ? p.last_chat_ms : p.last_seen_ms;

    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = messages_.find(p.id);
      if (it != messages_.end() && !it->second.empty()) {
        last_text = it->second.back().text;
        last_ts = it->second.back().ts_ms;
      }
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

std::string LanTalkEngine::messages_json(const std::string &peer_id) const {
  if (peer_id.empty()) {
    return "[]";
  }

  std::vector<Message> msgs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = messages_.find(peer_id);
    if (it == messages_.end()) {
      return "[]";
    }
    msgs = it->second;
  }

  std::ostringstream os;
  os << '[';
  bool first = true;
  for (const auto &m : msgs) {
    if (!first) {
      os << ',';
    }
    first = false;

    os << '{';
    os << "\"id\":\"" << json_escape(m.id) << "\",";
    os << "\"ts\":" << m.ts_ms << ',';
    os << "\"time\":\"" << format_hhmm(m.ts_ms) << "\",";
    os << "\"out\":" << (m.outgoing ? "true" : "false") << ',';
    os << "\"text\":\"" << json_escape(m.text) << "\"";
    os << '}';
  }
  os << ']';
  return os.str();
}

std::vector<std::string> LanTalkEngine::split(const std::string &in, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : in) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

std::string LanTalkEngine::trim(const std::string &in) {
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

std::string LanTalkEngine::json_escape(const std::string &in) {
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

std::string LanTalkEngine::b64_encode(const std::string &in) {
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

std::string LanTalkEngine::b64_decode(const std::string &in) {
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

bool LanTalkEngine::random_bytes(std::uint8_t *dst, std::size_t n) {
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

std::string LanTalkEngine::hex_encode(const std::uint8_t *data, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out[i * 2] = kHex[(data[i] >> 4) & 0x0f];
    out[i * 2 + 1] = kHex[data[i] & 0x0f];
  }
  return out;
}

std::int64_t LanTalkEngine::now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string LanTalkEngine::format_hhmm(std::int64_t ts_ms) {
  const auto sec = static_cast<std::time_t>(ts_ms / 1000);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &sec);
#else
  localtime_r(&sec, &tmv);
#endif
  std::ostringstream os;
  os << std::put_time(&tmv, "%H:%M");
  return os.str();
}

std::string LanTalkEngine::primary_ipv4() {
  socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == kInvalidSocket) {
    return "";
  }

  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
  const auto _ = connect(sock, reinterpret_cast<sockaddr *>(&remote), sizeof(remote));
  (void)_;

  sockaddr_in local{};
  socklen_t len = sizeof(local);
  if (getsockname(sock, reinterpret_cast<sockaddr *>(&local), &len) != 0) {
    close_socket(sock);
    return "";
  }
  close_socket(sock);

  char ip[64] = {0};
  if (inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip)) == nullptr) {
    return "";
  }
  return ip;
}

} // namespace lantalk
