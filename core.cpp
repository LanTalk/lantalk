#include "core.h"

#include "monocypher.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <ShlObj.h>
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lantalk {
namespace {

constexpr std::uint16_t kDiscoveryPort = 39091;
constexpr std::uint16_t kListenPortStart = 39092;
constexpr std::uint16_t kListenPortEnd = 39150;
constexpr char kMulticastAddr[] = "239.255.77.77";
constexpr std::size_t kFrameAdBytes = 100;
constexpr std::size_t kFrameMacBytes = 16;
constexpr std::size_t kChunkBytes = 1024 * 1024;
constexpr std::int64_t kPresenceEveryMs = 2000;
constexpr std::int64_t kOfflineAfterMs = 8000;

constexpr std::uint8_t kPayloadText = 1;
constexpr std::uint8_t kPayloadEmoji = 2;
constexpr std::uint8_t kPayloadFileStart = 3;
constexpr std::uint8_t kPayloadFileChunk = 4;
constexpr std::uint8_t kPayloadFileEnd = 5;

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

std::string trim(const std::string &in) {
  std::size_t start = 0;
  while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start])) != 0) {
    ++start;
  }
  std::size_t end = in.size();
  while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) {
    --end;
  }
  return in.substr(start, end - start);
}

std::string utf8_from_path(const std::filesystem::path &p) {
  auto u8 = p.u8string();
  return std::string(reinterpret_cast<const char *>(u8.c_str()), u8.size());
}

std::filesystem::path path_from_utf8(const std::string &u8) {
  return std::filesystem::u8path(u8);
}

std::filesystem::path current_exe_path() {
#if defined(_WIN32)
  std::wstring buf;
  buf.resize(32768);
  const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (len == 0 || len >= buf.size()) {
    return std::filesystem::current_path() / "lantalk";
  }
  buf.resize(len);
  return std::filesystem::path(buf);
#else
  std::array<char, 4096> buf{};
  const auto n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (n <= 0) {
    return std::filesystem::current_path() / "lantalk";
  }
  buf[static_cast<std::size_t>(n)] = '\0';
  return std::filesystem::path(buf.data());
#endif
}

std::int64_t ms_since_epoch() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void write_u16(std::vector<std::uint8_t> &out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xff));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}

void write_u32(std::vector<std::uint8_t> &out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xff));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
}

void write_u64(std::vector<std::uint8_t> &out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
  }
}

bool read_u16(const std::vector<std::uint8_t> &in, std::size_t &cursor, std::uint16_t &out) {
  if (cursor + 2 > in.size()) {
    return false;
  }
  out = static_cast<std::uint16_t>(in[cursor]) |
      (static_cast<std::uint16_t>(in[cursor + 1]) << 8);
  cursor += 2;
  return true;
}

bool read_u32(const std::vector<std::uint8_t> &in, std::size_t &cursor, std::uint32_t &out) {
  if (cursor + 4 > in.size()) {
    return false;
  }
  out = static_cast<std::uint32_t>(in[cursor]) |
      (static_cast<std::uint32_t>(in[cursor + 1]) << 8) |
      (static_cast<std::uint32_t>(in[cursor + 2]) << 16) |
      (static_cast<std::uint32_t>(in[cursor + 3]) << 24);
  cursor += 4;
  return true;
}

bool read_u64(const std::vector<std::uint8_t> &in, std::size_t &cursor, std::uint64_t &out) {
  if (cursor + 8 > in.size()) {
    return false;
  }
  out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= (static_cast<std::uint64_t>(in[cursor + i]) << (i * 8));
  }
  cursor += 8;
  return true;
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

std::string to_hex16(const std::array<std::uint8_t, 16> &v) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(32);
  for (std::size_t i = 0; i < v.size(); ++i) {
    out[i * 2] = kHex[(v[i] >> 4) & 0x0f];
    out[i * 2 + 1] = kHex[v[i] & 0x0f];
  }
  return out;
}

std::array<std::uint8_t, 16> random_id16() {
  std::array<std::uint8_t, 16> out{};
#if defined(_WIN32)
  BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
  std::ifstream in("/dev/urandom", std::ios::binary);
  if (in.is_open()) {
    in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
  }
#endif
  return out;
}

std::string hostname_default() {
  std::array<char, 256> buf{};
  if (gethostname(buf.data(), static_cast<int>(buf.size() - 1)) == 0) {
    std::string name = trim(buf.data());
    if (!name.empty()) {
      return name;
    }
  }
  return "LanTalk User";
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

} // namespace

struct AppCore::IncomingTransfer {
  std::string peer_id;
  bool image = false;
  std::string file_name;
  std::uint64_t expected_size = 0;
  std::uint64_t written_size = 0;
  std::filesystem::path local_path;
  std::ofstream out;
};

struct AppCore::InstanceLock {
#if defined(_WIN32)
  HANDLE handle = nullptr;
#else
  int fd = -1;
#endif

  ~InstanceLock() {
#if defined(_WIN32)
    if (handle != nullptr) {
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

  static std::string lock_name(const std::filesystem::path &exe_path) {
    const auto raw = exe_path.lexically_normal().string();
    std::uint8_t hash[16]{};
    crypto_blake2b(hash, sizeof(hash), reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size());
    return "lantalk_instance_" + AppCore::hex_encode(hash, sizeof(hash));
  }

  bool acquire(const std::filesystem::path &exe_path) {
    const auto key = lock_name(exe_path);
#if defined(_WIN32)
    const auto win_name = std::string("Local\\") + key;
    handle = CreateMutexA(nullptr, FALSE, win_name.c_str());
    if (handle == nullptr) {
      return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(handle);
      handle = nullptr;
      return false;
    }
    return true;
#else
    std::filesystem::path file = std::filesystem::temp_directory_path() / (key + ".lock");
    fd = open(file.c_str(), O_RDWR | O_CREAT, 0644);
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

struct AppCore::DataSlotLock {
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

  bool try_lock_file(const std::filesystem::path &lock_file) {
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

struct AppCore::SocketState {
#if defined(_WIN32)
  WSADATA wsa{};
  bool wsa_ok = false;
#endif
  socket_t udp_send = kInvalidSocket;
  socket_t udp_recv = kInvalidSocket;
  socket_t tcp_listen = kInvalidSocket;
  std::uint16_t listen_port = 0;
  std::thread discovery_thread;
  std::thread accept_thread;
};

AppCore::AppCore() = default;

AppCore::~AppCore() {
  shutdown();
}

bool AppCore::boot(std::string &error) {
  exe_path_ = current_exe_path();
  exe_dir_ = exe_path_.parent_path();

  instance_lock_ = std::make_unique<InstanceLock>();
  const bool allow_same_exe = std::getenv("LANTALK_DEV_MULTI") != nullptr;
  if (!allow_same_exe && !instance_lock_->acquire(exe_path_)) {
    error = "instance already running";
    return false;
  }

  if (!init_data_slot(error)) {
    return false;
  }
  if (!init_identity(error)) {
    return false;
  }
  if (!init_profile()) {
    error = "failed to load profile";
    return false;
  }

  load_peers();
  load_messages();

  if (!start_network(error)) {
    return false;
  }

  running_.store(true);
  bump_revision();
  return true;
}

void AppCore::shutdown() {
  if (!running_.exchange(false)) {
    return;
  }
  stop_network();
  save_profile();
  save_peers();
  incoming_transfers_.clear();
}

void AppCore::set_file_picker(std::function<std::string(bool image_only)> picker) {
  std::lock_guard<std::mutex> lock(mu_);
  file_picker_ = std::move(picker);
}

bool AppCore::init_data_slot(std::string &error) {
  for (int i = 0; i < 64; ++i) {
    const std::string name = (i == 0) ? "data" : ("data(" + std::to_string(i) + ")");
    auto candidate = exe_dir_ / name;
    std::error_code ec;
    std::filesystem::create_directories(candidate, ec);
    if (ec) {
      continue;
    }

    auto lock = std::make_unique<DataSlotLock>();
    if (!lock->try_lock_file(candidate / ".slot.lock")) {
      continue;
    }

    lock->dir = candidate;
    slot_lock_ = std::move(lock);
    data_dir_ = candidate;
    return true;
  }
  error = "failed to lock data slot";
  return false;
}

bool AppCore::init_identity(std::string &error) {
  const auto file = data_dir_ / "identity.bin";
  std::array<std::uint8_t, 32> secret{};

  if (std::filesystem::exists(file)) {
    std::ifstream in(file, std::ios::binary);
    in.read(reinterpret_cast<char *>(secret.data()), static_cast<std::streamsize>(secret.size()));
    if (in.gcount() != static_cast<std::streamsize>(secret.size())) {
      error = "identity.bin corrupted";
      return false;
    }
  } else {
    if (!random_bytes(secret.data(), secret.size())) {
      error = "rng failed";
      return false;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(secret.data()), static_cast<std::streamsize>(secret.size()));
    out.flush();
  }

  self_.secret = secret;
  crypto_x25519_public_key(self_.pub.data(), self_.secret.data());

  std::array<std::uint8_t, 16> digest{};
  crypto_blake2b(digest.data(), digest.size(), self_.pub.data(), self_.pub.size());
  self_.id = hex_encode(digest.data(), digest.size());
  self_.pub_hex = hex_encode(self_.pub.data(), self_.pub.size());
  return true;
}

bool AppCore::init_profile() {
  const auto profile_file = data_dir_ / "profile.txt";
  if (!std::filesystem::exists(profile_file)) {
    self_name_ = hostname_default();
    self_avatar_.clear();
    save_profile();
    return true;
  }

  std::ifstream in(profile_file);
  std::string line;
  while (std::getline(in, line)) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    const auto key = trim(line.substr(0, pos));
    const auto val = line.substr(pos + 1);
    if (key == "name_b64") {
      self_name_ = b64_decode(val);
    } else if (key == "avatar_b64") {
      self_avatar_ = b64_decode(val);
    }
  }

  if (self_name_.empty()) {
    self_name_ = hostname_default();
  }
  return true;
}

void AppCore::save_profile() {
  const auto profile_file = data_dir_ / "profile.txt";
  std::ofstream out(profile_file, std::ios::trunc);
  out << "name_b64=" << b64_encode(self_name_) << '\n';
  out << "avatar_b64=" << b64_encode(self_avatar_) << '\n';
}

void AppCore::load_peers() {
  const auto peers_file = data_dir_ / "peers.tsv";
  if (!std::filesystem::exists(peers_file)) {
    return;
  }
  std::ifstream in(peers_file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const auto parts = split(line, '\t');
    if (parts.size() < 10) {
      continue;
    }
    Peer p;
    p.id = parts[0];
    p.public_key_hex = parts[1];
    p.name = b64_decode(parts[2]);
    p.remark = b64_decode(parts[3]);
    p.avatar = b64_decode(parts[4]);
    p.ip = parts[5];
    p.port = static_cast<std::uint16_t>(std::stoul(parts[6]));
    p.first_seen_ms = std::stoll(parts[7]);
    p.last_seen_ms = std::stoll(parts[8]);
    p.last_chat_ms = std::stoll(parts[9]);
    if (parts.size() > 10) {
      p.unread = static_cast<std::uint32_t>(std::stoul(parts[10]));
    }
    p.online = false;
    if (!p.id.empty()) {
      peers_[p.id] = std::move(p);
    }
  }
}

void AppCore::save_peers() {
  const auto peers_file = data_dir_ / "peers.tsv";
  std::ofstream out(peers_file, std::ios::trunc);
  for (const auto &[id, p] : peers_) {
    out << id << '\t'
        << p.public_key_hex << '\t'
        << b64_encode(p.name) << '\t'
        << b64_encode(p.remark) << '\t'
        << b64_encode(p.avatar) << '\t'
        << p.ip << '\t'
        << p.port << '\t'
        << p.first_seen_ms << '\t'
        << p.last_seen_ms << '\t'
        << p.last_chat_ms << '\t'
        << p.unread << '\n';
  }
}

void AppCore::load_messages() {
  for (const auto &entry : std::filesystem::directory_iterator(data_dir_)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto fn = entry.path().filename().string();
    if (fn.rfind("chat_", 0) != 0 || fn.size() < 6 || fn.find(".log") == std::string::npos) {
      continue;
    }
    const auto peer_id = fn.substr(5, fn.size() - 9);
    std::ifstream in(entry.path());
    std::string line;
    while (std::getline(in, line)) {
      const auto parts = split(line, '\t');
      if (parts.size() < 8) {
        continue;
      }
      Message m;
      m.ts_ms = std::stoll(parts[0]);
      m.outgoing = (parts[1] == "o");
      m.kind = parts[2];
      m.text = b64_decode(parts[3]);
      m.file_name = b64_decode(parts[4]);
      m.file_size = static_cast<std::uint64_t>(std::stoull(parts[5]));
      m.local_path = b64_decode(parts[6]);
      m.id = parts[7];
      messages_[peer_id].push_back(m);
      auto it = peers_.find(peer_id);
      if (it != peers_.end()) {
        it->second.last_chat_ms = std::max(it->second.last_chat_ms, m.ts_ms);
      }
    }
  }
}

void AppCore::append_message_to_disk(const std::string &peer_id, const Message &m) {
  const auto file = data_dir_ / ("chat_" + peer_id + ".log");
  std::ofstream out(file, std::ios::app);
  out << m.ts_ms << '\t'
      << (m.outgoing ? "o" : "i") << '\t'
      << m.kind << '\t'
      << b64_encode(m.text) << '\t'
      << b64_encode(m.file_name) << '\t'
      << m.file_size << '\t'
      << b64_encode(m.local_path) << '\t'
      << m.id << '\n';
}

bool AppCore::start_network(std::string &error) {
  sockets_ = std::make_unique<SocketState>();
#if defined(_WIN32)
  if (WSAStartup(MAKEWORD(2, 2), &sockets_->wsa) != 0) {
    error = "WSAStartup failed";
    return false;
  }
  sockets_->wsa_ok = true;
#endif

  for (std::uint16_t port = kListenPortStart; port <= kListenPortEnd; ++port) {
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
    addr.sin_port = htons(port);

    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
        listen(s, 32) == 0) {
      sockets_->tcp_listen = s;
      sockets_->listen_port = port;
      break;
    }

    close_socket(s);
  }

  if (sockets_->tcp_listen == kInvalidSocket) {
    error = "failed to bind tcp listen port";
    stop_network();
    return false;
  }

  sockets_->udp_recv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockets_->udp_send = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockets_->udp_recv == kInvalidSocket || sockets_->udp_send == kInvalidSocket) {
    error = "failed to create udp sockets";
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

  sockaddr_in recv_addr{};
  recv_addr.sin_family = AF_INET;
  recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  recv_addr.sin_port = htons(kDiscoveryPort);
  if (bind(sockets_->udp_recv, reinterpret_cast<sockaddr *>(&recv_addr), sizeof(recv_addr)) != 0) {
    error = "failed to bind discovery port";
    stop_network();
    return false;
  }

  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr(kMulticastAddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
#if defined(_WIN32)
  setsockopt(sockets_->udp_recv, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char *>(&mreq), sizeof(mreq));
#else
  setsockopt(sockets_->udp_recv, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
#endif

  sockets_->accept_thread = std::thread([this]() { accept_loop(); });
  sockets_->discovery_thread = std::thread([this]() { discovery_loop(); });

  return true;
}

void AppCore::stop_network() {
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
  if (sockets_->wsa_ok) {
    WSACleanup();
    sockets_->wsa_ok = false;
  }
#endif

  sockets_.reset();
}

void AppCore::announce_presence() {
  if (sockets_ == nullptr || sockets_->udp_send == kInvalidSocket) {
    return;
  }

  const auto now = now_ms();
  std::string name;
  std::string avatar;
  std::vector<std::string> known_ips;
  {
    std::lock_guard<std::mutex> lock(mu_);
    name = self_name_;
    avatar = self_avatar_;
    known_ips.reserve(peers_.size());
    for (const auto &[id, p] : peers_) {
      if (!p.ip.empty()) {
        known_ips.push_back(p.ip);
      }
    }
  }

  std::ostringstream os;
  os << "LT3|" << self_.id << "|" << sockets_->listen_port << "|" << self_.pub_hex << "|"
     << b64_encode(name) << "|" << b64_encode(avatar) << "|" << now;
  const auto payload = os.str();

  sockaddr_in baddr{};
  baddr.sin_family = AF_INET;
  baddr.sin_port = htons(kDiscoveryPort);
  baddr.sin_addr.s_addr = inet_addr("255.255.255.255");
  sendto(sockets_->udp_send,
         payload.data(),
         static_cast<int>(payload.size()),
         0,
         reinterpret_cast<sockaddr *>(&baddr),
         sizeof(baddr));

  sockaddr_in maddr{};
  maddr.sin_family = AF_INET;
  maddr.sin_port = htons(kDiscoveryPort);
  maddr.sin_addr.s_addr = inet_addr(kMulticastAddr);
  sendto(sockets_->udp_send,
         payload.data(),
         static_cast<int>(payload.size()),
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
           payload.data(),
           static_cast<int>(payload.size()),
           0,
           reinterpret_cast<sockaddr *>(&uaddr),
           sizeof(uaddr));
  }
}

void AppCore::consume_presence_packet(const std::string &packet, const std::string &source_ip) {
  const auto parts = split(packet, '|');
  if (parts.size() < 7 || parts[0] != "LT3") {
    return;
  }

  const std::string &peer_id = parts[1];
  if (peer_id == self_.id || peer_id.size() != 32) {
    return;
  }

  std::uint16_t port = 0;
  if (!parse_u16(parts[2], port)) {
    return;
  }

  std::array<std::uint8_t, 32> pub{};
  if (!hex_decode(parts[3], pub.data(), pub.size())) {
    return;
  }
  std::array<std::uint8_t, 16> digest{};
  crypto_blake2b(digest.data(), digest.size(), pub.data(), pub.size());
  const auto derived_id = hex_encode(digest.data(), digest.size());
  if (derived_id != peer_id) {
    return;
  }

  const auto name = b64_decode(parts[4]);
  const auto avatar = b64_decode(parts[5]);
  const auto seen_ms = now_ms();

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto &p = peers_[peer_id];
    if (p.id.empty()) {
      p.id = peer_id;
      p.first_seen_ms = seen_ms;
      changed = true;
    }
    if (p.public_key_hex != parts[3]) {
      p.public_key_hex = parts[3];
      changed = true;
    }
    if (!name.empty() && p.name != name) {
      p.name = name;
      changed = true;
    }
    if (p.avatar != avatar) {
      p.avatar = avatar;
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
    p.last_seen_ms = seen_ms;
  }

  if (changed) {
    save_peers();
    bump_revision();
  }
}

void AppCore::discovery_loop() {
  auto next_presence = now_ms();
  while (running_.load()) {
    const auto now = now_ms();
    if (now >= next_presence) {
      announce_presence();
      next_presence = now + kPresenceEveryMs;

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

    std::array<char, 2048> buf{};
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
    consume_presence_packet(std::string(buf.data(), static_cast<std::size_t>(n)), ip);
  }
}

void AppCore::accept_loop() {
  while (running_.load()) {
    if (sockets_ == nullptr || sockets_->tcp_listen == kInvalidSocket) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }

    sockaddr_in from{};
    socklen_t len = sizeof(from);
    socket_t client = accept(sockets_->tcp_listen, reinterpret_cast<sockaddr *>(&from), &len);
    if (client == kInvalidSocket) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    char ip[64] = {0};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    std::thread([this, client, sip = std::string(ip)]() {
      handle_connection(static_cast<std::intptr_t>(client), sip);
      close_socket(client);
    }).detach();
  }
}

void AppCore::handle_connection(std::intptr_t sock_value, const std::string &source_ip) {
  const auto sock = static_cast<socket_t>(sock_value);
  while (running_.load()) {
    std::array<std::uint8_t, kFrameAdBytes> ad{};
    if (!recv_all(sock, ad.data(), ad.size())) {
      return;
    }

    if (std::memcmp(ad.data(), "LT3P", 4) != 0 || ad[4] != 1) {
      return;
    }

    std::uint32_t cipher_size =
        static_cast<std::uint32_t>(ad[96]) |
        (static_cast<std::uint32_t>(ad[97]) << 8) |
        (static_cast<std::uint32_t>(ad[98]) << 16) |
        (static_cast<std::uint32_t>(ad[99]) << 24);

    if (cipher_size == 0 || cipher_size > 4 * 1024 * 1024) {
      return;
    }

    std::array<std::uint8_t, kFrameMacBytes> mac{};
    if (!recv_all(sock, mac.data(), mac.size())) {
      return;
    }

    std::vector<std::uint8_t> cipher(cipher_size);
    if (!recv_all(sock, cipher.data(), cipher.size())) {
      return;
    }

    std::array<std::uint8_t, 32> sender_pub{};
    std::memcpy(sender_pub.data(), ad.data() + 40, sender_pub.size());

    std::array<std::uint8_t, 16> digest{};
    crypto_blake2b(digest.data(), digest.size(), sender_pub.data(), sender_pub.size());
    const auto sender_id = hex_encode(digest.data(), digest.size());

    std::array<std::uint8_t, 32> raw_shared{};
    std::array<std::uint8_t, 32> key{};
    crypto_x25519(raw_shared.data(), self_.secret.data(), sender_pub.data());
    crypto_blake2b(key.data(), key.size(), raw_shared.data(), raw_shared.size());

    std::vector<std::uint8_t> plain(cipher_size);
    const auto *nonce = ad.data() + 72;
    if (crypto_aead_unlock(plain.data(),
                           mac.data(),
                           key.data(),
                           nonce,
                           ad.data(),
                           ad.size(),
                           cipher.data(),
                           cipher.size()) != 0) {
      continue;
    }

    process_plain_payload(source_ip, sender_id, sender_pub, plain);
  }
}

void AppCore::process_plain_payload(const std::string &source_ip,
                                    const std::string &sender_id,
                                    const std::array<std::uint8_t, 32> &sender_pub,
                                    const std::vector<std::uint8_t> &plain) {
  if (plain.empty()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto &peer = peers_[sender_id];
    if (peer.id.empty()) {
      peer.id = sender_id;
      peer.first_seen_ms = now_ms();
    }
    peer.public_key_hex = hex_encode(sender_pub.data(), sender_pub.size());
    peer.ip = source_ip;
    peer.last_seen_ms = now_ms();
    peer.online = true;
    if (peer.port == 0) {
      peer.port = kListenPortStart;
    }
  }

  std::size_t cur = 0;
  const auto type = plain[cur++];

  if (type == kPayloadText || type == kPayloadEmoji) {
    std::uint64_t ts = 0;
    std::uint16_t name_len = 0;
    std::uint32_t text_len = 0;
    std::array<std::uint8_t, 16> msg_id{};

    if (!read_u64(plain, cur, ts)) {
      return;
    }
    if (cur + msg_id.size() > plain.size()) {
      return;
    }
    std::memcpy(msg_id.data(), plain.data() + cur, msg_id.size());
    cur += msg_id.size();

    if (!read_u16(plain, cur, name_len) || cur + name_len > plain.size()) {
      return;
    }
    const std::string sender_name(reinterpret_cast<const char *>(plain.data() + cur), name_len);
    cur += name_len;

    if (!read_u32(plain, cur, text_len) || cur + text_len > plain.size()) {
      return;
    }
    const std::string text(reinterpret_cast<const char *>(plain.data() + cur), text_len);

    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = peers_.find(sender_id);
      if (it != peers_.end() && !sender_name.empty()) {
        it->second.name = sender_name;
      }
    }

    on_incoming_text(sender_id, type == kPayloadEmoji, static_cast<std::int64_t>(ts), text);
    return;
  }

  if (type == kPayloadFileStart) {
    std::uint64_t ts = 0;
    std::uint16_t sender_name_len = 0;
    std::uint16_t file_name_len = 0;
    std::array<std::uint8_t, 16> transfer{};
    std::uint64_t file_size = 0;

    if (!read_u64(plain, cur, ts)) {
      return;
    }
    if (cur + transfer.size() > plain.size()) {
      return;
    }
    std::memcpy(transfer.data(), plain.data() + cur, transfer.size());
    cur += transfer.size();

    if (!read_u16(plain, cur, sender_name_len) || cur + sender_name_len > plain.size()) {
      return;
    }
    const std::string sender_name(reinterpret_cast<const char *>(plain.data() + cur), sender_name_len);
    cur += sender_name_len;

    if (cur >= plain.size()) {
      return;
    }
    const bool image = plain[cur++] == 1;

    if (!read_u64(plain, cur, file_size)) {
      return;
    }

    if (!read_u16(plain, cur, file_name_len) || cur + file_name_len > plain.size()) {
      return;
    }
    const std::string file_name(reinterpret_cast<const char *>(plain.data() + cur), file_name_len);

    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = peers_.find(sender_id);
      if (it != peers_.end() && !sender_name.empty()) {
        it->second.name = sender_name;
      }
    }

    on_incoming_file_start(sender_id, transfer, image, file_size, file_name);
    return;
  }

  if (type == kPayloadFileChunk) {
    std::array<std::uint8_t, 16> transfer{};
    std::uint64_t offset = 0;
    std::uint32_t chunk_size = 0;

    if (cur + transfer.size() > plain.size()) {
      return;
    }
    std::memcpy(transfer.data(), plain.data() + cur, transfer.size());
    cur += transfer.size();

    if (!read_u64(plain, cur, offset) || !read_u32(plain, cur, chunk_size) || cur + chunk_size > plain.size()) {
      return;
    }

    std::vector<std::uint8_t> bytes(chunk_size);
    std::memcpy(bytes.data(), plain.data() + cur, chunk_size);
    on_incoming_file_chunk(sender_id, transfer, offset, bytes);
    return;
  }

  if (type == kPayloadFileEnd) {
    std::array<std::uint8_t, 16> transfer{};
    std::uint64_t final_size = 0;

    if (cur + transfer.size() > plain.size()) {
      return;
    }
    std::memcpy(transfer.data(), plain.data() + cur, transfer.size());
    cur += transfer.size();

    if (!read_u64(plain, cur, final_size)) {
      return;
    }

    on_incoming_file_end(sender_id, transfer, final_size);
    return;
  }
}

void AppCore::on_incoming_text(const std::string &peer_id, bool emoji, std::int64_t ts_ms, const std::string &text) {
  Message m;
  m.id = hex_encode(random_id16().data(), 16);
  m.ts_ms = ts_ms;
  m.outgoing = false;
  m.kind = emoji ? "emoji" : "text";
  m.text = text;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto &vec = messages_[peer_id];
    vec.push_back(m);

    auto &peer = peers_[peer_id];
    if (peer.id.empty()) {
      peer.id = peer_id;
      peer.first_seen_ms = now_ms();
    }
    peer.last_chat_ms = ts_ms;
    if (active_peer_ != peer_id) {
      peer.unread += 1;
    }
  }

  append_message_to_disk(peer_id, m);
  save_peers();
  bump_revision();
}

void AppCore::on_incoming_file_start(const std::string &peer_id,
                                     const std::array<std::uint8_t, 16> &transfer_id,
                                     bool image,
                                     std::uint64_t file_size,
                                     const std::string &name) {
  IncomingTransfer transfer;
  transfer.peer_id = peer_id;
  transfer.image = image;
  transfer.file_name = name;
  transfer.expected_size = file_size;
  transfer.local_path = unique_download_path(peer_id, name);

  std::error_code ec;
  std::filesystem::create_directories(transfer.local_path.parent_path(), ec);
  transfer.out.open(transfer.local_path, std::ios::binary | std::ios::trunc);
  if (!transfer.out.is_open()) {
    return;
  }

  const auto key = peer_id + ":" + to_hex16(transfer_id);
  {
    std::lock_guard<std::mutex> lock(mu_);
    incoming_transfers_[key] = std::move(transfer);
  }
}

void AppCore::on_incoming_file_chunk(const std::string &peer_id,
                                     const std::array<std::uint8_t, 16> &transfer_id,
                                     std::uint64_t offset,
                                     const std::vector<std::uint8_t> &bytes) {
  const auto key = peer_id + ":" + to_hex16(transfer_id);
  std::lock_guard<std::mutex> lock(mu_);
  auto it = incoming_transfers_.find(key);
  if (it == incoming_transfers_.end()) {
    return;
  }

  auto &t = it->second;
  if (!t.out.is_open()) {
    return;
  }

  if (offset != t.written_size) {
    t.out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    t.written_size = offset;
  }
  t.out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  t.written_size += bytes.size();
}

void AppCore::on_incoming_file_end(const std::string &peer_id,
                                   const std::array<std::uint8_t, 16> &transfer_id,
                                   std::uint64_t final_size) {
  const auto key = peer_id + ":" + to_hex16(transfer_id);

  Message msg;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = incoming_transfers_.find(key);
    if (it == incoming_transfers_.end()) {
      return;
    }

    auto &t = it->second;
    if (t.out.is_open()) {
      t.out.flush();
      t.out.close();
    }

    msg.id = hex_encode(random_id16().data(), 16);
    msg.ts_ms = now_ms();
    msg.outgoing = false;
    msg.kind = t.image ? "image" : "file";
    msg.file_name = t.file_name;
    msg.file_size = std::max(t.written_size, final_size);
    msg.local_path = utf8_from_path(t.local_path);

    auto &vec = messages_[peer_id];
    vec.push_back(msg);

    auto pit = peers_.find(peer_id);
    if (pit != peers_.end()) {
      pit->second.last_chat_ms = msg.ts_ms;
      if (active_peer_ != peer_id) {
        pit->second.unread += 1;
      }
    }

    incoming_transfers_.erase(it);
    ok = true;
  }

  if (!ok) {
    return;
  }

  append_message_to_disk(peer_id, msg);
  save_peers();
  bump_revision();
}

bool AppCore::send_plain_payload(const Peer &peer, const std::vector<std::uint8_t> &plain, std::string &error) {
  if (peer.ip.empty() || peer.port == 0) {
    error = "peer endpoint unavailable";
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

  const bool ok = send_plain_payload_on_socket(static_cast<std::intptr_t>(sock), peer, plain, error);
  close_socket(sock);
  return ok;
}

bool AppCore::send_plain_payload_on_socket(std::intptr_t sock_value,
                                           const Peer &peer,
                                           const std::vector<std::uint8_t> &plain,
                                           std::string &error) {
  std::array<std::uint8_t, 32> peer_pub{};
  if (!hex_decode(peer.public_key_hex, peer_pub.data(), peer_pub.size())) {
    error = "peer public key missing";
    return false;
  }

  std::array<std::uint8_t, 32> raw_shared{};
  std::array<std::uint8_t, 32> key{};
  crypto_x25519(raw_shared.data(), self_.secret.data(), peer_pub.data());
  crypto_blake2b(key.data(), key.size(), raw_shared.data(), raw_shared.size());

  std::array<std::uint8_t, kFrameAdBytes> ad{};
  std::memcpy(ad.data(), "LT3P", 4);
  ad[4] = 1;
  ad[5] = 1;
  ad[6] = 0;
  ad[7] = 0;

  std::string sid = self_.id;
  if (sid.size() < 32) {
    sid.append(32 - sid.size(), '0');
  }
  std::memcpy(ad.data() + 8, sid.data(), 32);
  std::memcpy(ad.data() + 40, self_.pub.data(), self_.pub.size());

  if (!random_bytes(ad.data() + 72, 24)) {
    error = "rng failed";
    return false;
  }

  const std::uint32_t cipher_size = static_cast<std::uint32_t>(plain.size());
  ad[96] = static_cast<std::uint8_t>(cipher_size & 0xff);
  ad[97] = static_cast<std::uint8_t>((cipher_size >> 8) & 0xff);
  ad[98] = static_cast<std::uint8_t>((cipher_size >> 16) & 0xff);
  ad[99] = static_cast<std::uint8_t>((cipher_size >> 24) & 0xff);

  std::array<std::uint8_t, kFrameMacBytes> mac{};
  std::vector<std::uint8_t> cipher(plain.size());

  crypto_aead_lock(cipher.data(),
                   mac.data(),
                   key.data(),
                   ad.data() + 72,
                   ad.data(),
                   ad.size(),
                   plain.data(),
                   plain.size());

  const auto sock = static_cast<socket_t>(sock_value);
  if (!send_all(sock, ad.data(), ad.size()) ||
      !send_all(sock, mac.data(), mac.size()) ||
      !send_all(sock, cipher.data(), cipher.size())) {
    error = "send failed";
    return false;
  }

  return true;
}

bool AppCore::send_text_like(const std::string &peer_id,
                             const std::string &kind,
                             const std::string &text,
                             std::string &error) {
  if (text.empty()) {
    error = "empty message";
    return false;
  }

  Peer peer;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
      error = "peer not found";
      return false;
    }
    peer = it->second;
  }

  std::vector<std::uint8_t> plain;
  plain.reserve(1 + 8 + 16 + 2 + self_name_.size() + 4 + text.size());
  plain.push_back(kind == "emoji" ? kPayloadEmoji : kPayloadText);
  const auto ts = static_cast<std::uint64_t>(now_ms());
  write_u64(plain, ts);

  const auto msgid = random_id16();
  plain.insert(plain.end(), msgid.begin(), msgid.end());

  const auto name_bytes = self_name_;
  write_u16(plain, static_cast<std::uint16_t>(std::min<std::size_t>(name_bytes.size(), 65535)));
  plain.insert(plain.end(), name_bytes.begin(), name_bytes.end());

  write_u32(plain, static_cast<std::uint32_t>(text.size()));
  plain.insert(plain.end(), text.begin(), text.end());

  if (!send_plain_payload(peer, plain, error)) {
    return false;
  }

  Message m;
  m.id = hex_encode(msgid.data(), msgid.size());
  m.ts_ms = static_cast<std::int64_t>(ts);
  m.outgoing = true;
  m.kind = kind;
  m.text = text;

  {
    std::lock_guard<std::mutex> lock(mu_);
    messages_[peer_id].push_back(m);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
      it->second.last_chat_ms = m.ts_ms;
    }
  }

  append_message_to_disk(peer_id, m);
  save_peers();
  bump_revision();
  return true;
}

bool AppCore::send_file_like(const std::string &peer_id,
                             const std::filesystem::path &path,
                             bool image,
                             std::string &error) {
  Peer peer;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
      error = "peer not found";
      return false;
    }
    peer = it->second;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    error = "file open failed";
    return false;
  }

  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    error = "file size failed";
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

  const auto transfer = random_id16();
  const auto ts = static_cast<std::uint64_t>(now_ms());
  const auto file_name = utf8_from_path(path.filename());

  std::vector<std::uint8_t> start;
  start.push_back(kPayloadFileStart);
  write_u64(start, ts);
  start.insert(start.end(), transfer.begin(), transfer.end());
  write_u16(start, static_cast<std::uint16_t>(std::min<std::size_t>(self_name_.size(), 65535)));
  start.insert(start.end(), self_name_.begin(), self_name_.end());
  start.push_back(image ? 1 : 0);
  write_u64(start, static_cast<std::uint64_t>(size));
  write_u16(start, static_cast<std::uint16_t>(std::min<std::size_t>(file_name.size(), 65535)));
  start.insert(start.end(), file_name.begin(), file_name.end());

  if (!send_plain_payload_on_socket(static_cast<std::intptr_t>(sock), peer, start, error)) {
    close_socket(sock);
    return false;
  }

  std::vector<std::uint8_t> chunk(kChunkBytes);
  std::uint64_t offset = 0;
  while (in.good()) {
    in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    const auto got = in.gcount();
    if (got <= 0) {
      break;
    }

    std::vector<std::uint8_t> pkt;
    pkt.reserve(1 + 16 + 8 + 4 + static_cast<std::size_t>(got));
    pkt.push_back(kPayloadFileChunk);
    pkt.insert(pkt.end(), transfer.begin(), transfer.end());
    write_u64(pkt, offset);
    write_u32(pkt, static_cast<std::uint32_t>(got));
    pkt.insert(pkt.end(), chunk.begin(), chunk.begin() + got);

    if (!send_plain_payload_on_socket(static_cast<std::intptr_t>(sock), peer, pkt, error)) {
      close_socket(sock);
      return false;
    }

    offset += static_cast<std::uint64_t>(got);
  }

  std::vector<std::uint8_t> end;
  end.push_back(kPayloadFileEnd);
  end.insert(end.end(), transfer.begin(), transfer.end());
  write_u64(end, offset);
  if (!send_plain_payload_on_socket(static_cast<std::intptr_t>(sock), peer, end, error)) {
    close_socket(sock);
    return false;
  }

  close_socket(sock);

  Message m;
  m.id = hex_encode(transfer.data(), transfer.size());
  m.ts_ms = static_cast<std::int64_t>(ts);
  m.outgoing = true;
  m.kind = image ? "image" : "file";
  m.file_name = file_name;
  m.file_size = static_cast<std::uint64_t>(size);
  m.local_path = utf8_from_path(path);

  {
    std::lock_guard<std::mutex> lock(mu_);
    messages_[peer_id].push_back(m);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
      it->second.last_chat_ms = m.ts_ms;
    }
  }

  append_message_to_disk(peer_id, m);
  save_peers();
  bump_revision();
  return true;
}

std::string AppCore::rpc_bootstrap() {
  return snapshot_json(active_peer_, 0);
}

std::string AppCore::rpc_snapshot(const std::vector<std::string> &parts) {
  const std::string active = parts.size() > 1 ? parts[1] : active_peer_;
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

std::string AppCore::rpc_open_peer(const std::vector<std::string> &parts) {
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
  bump_revision();
  return R"({"ok":true})";
}

std::string AppCore::rpc_send_text(const std::vector<std::string> &parts, bool emoji) {
  if (parts.size() < 3) {
    return R"({"ok":false,"error":"invalid args"})";
  }

  const auto peer_id = parts[1];
  const auto text = b64_decode(parts[2]);

  std::string error;
  if (!send_text_like(peer_id, emoji ? "emoji" : "text", text, error)) {
    return std::string("{\"ok\":false,\"error\":\"") + json_escape(error) + "\"}";
  }
  return R"({"ok":true})";
}

std::string AppCore::rpc_send_file(const std::vector<std::string> &parts, bool image) {
  if (parts.size() < 3) {
    return R"({"ok":false,"error":"invalid args"})";
  }

  const auto peer_id = parts[1];
  const auto p = path_from_utf8(b64_decode(parts[2]));

  std::string error;
  if (!send_file_like(peer_id, p, image, error)) {
    return std::string("{\"ok\":false,\"error\":\"") + json_escape(error) + "\"}";
  }
  return R"({"ok":true})";
}

std::string AppCore::rpc_set_name(const std::vector<std::string> &parts) {
  if (parts.size() < 2) {
    return R"({"ok":false})";
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

std::string AppCore::rpc_set_avatar(const std::vector<std::string> &parts) {
  if (parts.size() < 2) {
    return R"({"ok":false})";
  }

  const auto avatar = b64_decode(parts[1]);
  {
    std::lock_guard<std::mutex> lock(mu_);
    self_avatar_ = avatar;
  }
  save_profile();
  announce_presence();
  bump_revision();
  return R"({"ok":true})";
}

std::string AppCore::rpc_set_remark(const std::vector<std::string> &parts) {
  if (parts.size() < 3) {
    return R"({"ok":false})";
  }

  const auto peer_id = parts[1];
  const auto remark = b64_decode(parts[2]);
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
      return R"({"ok":false,"error":"peer not found"})";
    }
    it->second.remark = remark;
  }

  save_peers();
  bump_revision();
  return R"({"ok":true})";
}

std::string AppCore::rpc_peer_profile(const std::vector<std::string> &parts) {
  if (parts.size() < 2) {
    return R"({"ok":false})";
  }

  Peer p;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = peers_.find(parts[1]);
    if (it == peers_.end()) {
      return R"({"ok":false,"error":"peer not found"})";
    }
    p = it->second;
  }

  std::ostringstream os;
  os << "{\"ok\":true,\"peer\":{";
  os << "\"id\":\"" << json_escape(p.id) << "\",";
  os << "\"name\":\"" << json_escape(resolve_display_name(p)) << "\",";
  os << "\"rawName\":\"" << json_escape(p.name) << "\",";
  os << "\"remark\":\"" << json_escape(p.remark) << "\",";
  os << "\"ip\":\"" << json_escape(p.ip) << "\",";
  os << "\"online\":" << (p.online ? "true" : "false") << ',';
  os << "\"lastSeen\":" << p.last_seen_ms;
  os << "}}";
  return os.str();
}

std::string AppCore::rpc_pick(bool image) {
  std::function<std::string(bool)> picker;
  {
    std::lock_guard<std::mutex> lock(mu_);
    picker = file_picker_;
  }

  if (!picker) {
    return R"({"ok":false,"path":""})";
  }

  const auto picked = picker(image);
  std::ostringstream os;
  os << "{\"ok\":true,\"path\":\"" << json_escape(b64_encode(picked)) << "\"}";
  return os.str();
}

std::string AppCore::handle_rpc(const std::string &command_line) {
  const auto parts = split(command_line, '\t');
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
    return rpc_open_peer(parts);
  }
  if (cmd == "send_text") {
    return rpc_send_text(parts, false);
  }
  if (cmd == "send_emoji") {
    return rpc_send_text(parts, true);
  }
  if (cmd == "send_file") {
    return rpc_send_file(parts, false);
  }
  if (cmd == "send_image") {
    return rpc_send_file(parts, true);
  }
  if (cmd == "set_name") {
    return rpc_set_name(parts);
  }
  if (cmd == "set_avatar") {
    return rpc_set_avatar(parts);
  }
  if (cmd == "set_remark") {
    return rpc_set_remark(parts);
  }
  if (cmd == "peer_profile") {
    return rpc_peer_profile(parts);
  }
  if (cmd == "pick_file") {
    return rpc_pick(false);
  }
  if (cmd == "pick_image") {
    return rpc_pick(true);
  }

  return R"({"ok":false,"error":"unknown command"})";
}

std::string AppCore::snapshot_json(const std::string &active_peer, std::uint64_t known_revision) {
  std::lock_guard<std::mutex> lock(mu_);

  if (!active_peer.empty()) {
    active_peer_ = active_peer;
    auto it = peers_.find(active_peer_);
    if (it != peers_.end()) {
      it->second.unread = 0;
    }
  }

  const auto rev = revision_.load();

  std::ostringstream os;
  os << "{\"ok\":true,";
  os << "\"revision\":" << rev << ',';
  os << "\"changed\":" << (known_revision != rev ? "true" : "false") << ',';
  os << "\"self\":" << self_json() << ',';
  os << "\"conversations\":" << conversations_json() << ',';
  os << "\"active\":\"" << json_escape(active_peer_) << "\",";
  os << "\"messages\":" << messages_json(active_peer_);
  os << '}';
  return os.str();
}

std::string AppCore::conversations_json() const {
  std::vector<std::reference_wrapper<const Peer>> ordered;
  ordered.reserve(peers_.size());
  for (const auto &[id, p] : peers_) {
    if (id == self_.id) {
      continue;
    }
    ordered.push_back(std::cref(p));
  }

  std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
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
  for (const auto &ref : ordered) {
    const auto &p = ref.get();
    const auto it = messages_.find(p.id);
    Message last_msg;
    bool has_last = false;
    if (it != messages_.end() && !it->second.empty()) {
      last_msg = it->second.back();
      has_last = true;
    }

    if (!first) {
      os << ',';
    }
    first = false;

    os << '{';
    os << "\"id\":\"" << json_escape(p.id) << "\",";
    os << "\"name\":\"" << json_escape(resolve_display_name(p)) << "\",";
    os << "\"rawName\":\"" << json_escape(p.name) << "\",";
    os << "\"avatar\":\"" << json_escape(p.avatar) << "\",";
    os << "\"online\":" << (p.online ? "true" : "false") << ',';
    os << "\"unread\":" << p.unread << ',';
    os << "\"lastTs\":" << (has_last ? last_msg.ts_ms : p.last_seen_ms) << ',';
    os << "\"last\":\"" << json_escape(has_last ? summarize_message(last_msg) : "") << "\"";
    os << '}';
  }
  os << ']';
  return os.str();
}

std::string AppCore::messages_json(const std::string &peer_id) const {
  if (peer_id.empty()) {
    return "[]";
  }

  const auto it = messages_.find(peer_id);
  if (it == messages_.end()) {
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
    os << "\"out\":" << (m.outgoing ? "true" : "false") << ',';
    os << "\"kind\":\"" << json_escape(m.kind) << "\",";
    os << "\"text\":\"" << json_escape(m.text) << "\",";
    os << "\"fileName\":\"" << json_escape(m.file_name) << "\",";
    os << "\"fileSize\":" << m.file_size << ',';
    os << "\"path\":\"" << json_escape(m.local_path) << "\",";
    os << "\"uri\":\"" << json_escape(join_path_uri(path_from_utf8(m.local_path))) << "\"";
    os << '}';
  }
  os << ']';
  return os.str();
}

std::string AppCore::self_json() const {
  std::ostringstream os;
  os << '{';
  os << "\"id\":\"" << json_escape(self_.id) << "\",";
  os << "\"name\":\"" << json_escape(self_name_) << "\",";
  os << "\"avatar\":\"" << json_escape(self_avatar_) << "\",";
  os << "\"dataDir\":\"" << json_escape(utf8_from_path(data_dir_)) << "\"";
  os << '}';
  return os.str();
}

std::string AppCore::resolve_display_name(const Peer &peer) const {
  if (!peer.remark.empty()) {
    return peer.remark;
  }
  if (!peer.name.empty()) {
    return peer.name;
  }
  return peer.id;
}

std::string AppCore::summarize_message(const Message &m) const {
  if (m.kind == "text" || m.kind == "emoji") {
    return m.text;
  }
  if (m.kind == "image") {
    return "[图片] " + m.file_name;
  }
  if (m.kind == "file") {
    return "[文件] " + m.file_name;
  }
  return "";
}

std::string AppCore::now_iso8601() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  std::tm tmv{};
#if defined(_WIN32)
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  std::ostringstream os;
  os << std::put_time(&tmv, "%Y-%m-%dT%H:%M:%SZ");
  return os.str();
}

std::string AppCore::json_escape(const std::string &in) {
  std::ostringstream os;
  for (char c : in) {
    switch (c) {
    case '"':
      os << "\\\"";
      break;
    case '\\':
      os << "\\\\";
      break;
    case '\b':
      os << "\\b";
      break;
    case '\f':
      os << "\\f";
      break;
    case '\n':
      os << "\\n";
      break;
    case '\r':
      os << "\\r";
      break;
    case '\t':
      os << "\\t";
      break;
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

std::string AppCore::b64_encode(const std::string &in) {
  std::vector<std::uint8_t> bytes(in.begin(), in.end());
  return b64_encode_bytes(bytes);
}

std::string AppCore::b64_decode(const std::string &in) {
  const auto bytes = b64_decode_bytes(in);
  return std::string(bytes.begin(), bytes.end());
}

std::string AppCore::b64_encode_bytes(const std::vector<std::uint8_t> &in) {
  static constexpr char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);

  std::size_t i = 0;
  while (i + 2 < in.size()) {
    const std::uint32_t n = (static_cast<std::uint32_t>(in[i]) << 16) |
        (static_cast<std::uint32_t>(in[i + 1]) << 8) |
        static_cast<std::uint32_t>(in[i + 2]);
    out.push_back(kChars[(n >> 18) & 63]);
    out.push_back(kChars[(n >> 12) & 63]);
    out.push_back(kChars[(n >> 6) & 63]);
    out.push_back(kChars[n & 63]);
    i += 3;
  }

  if (i < in.size()) {
    const std::uint32_t a = in[i];
    const std::uint32_t b = (i + 1 < in.size()) ? in[i + 1] : 0;
    const std::uint32_t n = (a << 16) | (b << 8);
    out.push_back(kChars[(n >> 18) & 63]);
    out.push_back(kChars[(n >> 12) & 63]);
    out.push_back(i + 1 < in.size() ? kChars[(n >> 6) & 63] : '=');
    out.push_back('=');
  }

  return out;
}

std::vector<std::uint8_t> AppCore::b64_decode_bytes(const std::string &in) {
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

  std::vector<std::uint8_t> out;
  int val = 0;
  int bits = -8;
  for (const auto c : in) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      continue;
    }
    if (c == '=') {
      break;
    }
    const auto d = table[static_cast<unsigned char>(c)];
    if (d < 0) {
      continue;
    }
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<std::uint8_t>((val >> bits) & 0xff));
      bits -= 8;
    }
  }
  return out;
}

std::vector<std::string> AppCore::split(const std::string &in, char sep) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : in) {
    if (c == sep) {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  return parts;
}

std::string AppCore::join_path_uri(const std::filesystem::path &p) {
  if (p.empty()) {
    return "";
  }

#if defined(_WIN32)
  std::string raw = utf8_from_path(p);
  std::replace(raw.begin(), raw.end(), '\\', '/');
  return "file:///" + raw;
#else
  return "file://" + utf8_from_path(p);
#endif
}

void AppCore::bump_revision() {
  revision_.fetch_add(1);
}

std::int64_t AppCore::now_ms() const {
  return ms_since_epoch();
}

bool AppCore::random_bytes(std::uint8_t *dst, std::size_t n) {
#if defined(_WIN32)
  return BCryptGenRandom(nullptr, dst, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  std::ifstream in("/dev/urandom", std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  in.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(n));
  return in.gcount() == static_cast<std::streamsize>(n);
#endif
}

std::string AppCore::hex_encode(const std::uint8_t *data, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out[2 * i] = kHex[(data[i] >> 4) & 0x0f];
    out[2 * i + 1] = kHex[data[i] & 0x0f];
  }
  return out;
}

bool AppCore::hex_decode(const std::string &hex, std::uint8_t *out, std::size_t out_len) {
  if (hex.size() != out_len * 2) {
    return false;
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };

  for (std::size_t i = 0; i < out_len; ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

std::string AppCore::sanitize_filename(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  for (const unsigned char c : name) {
    if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
        c == '*') {
      out.push_back('_');
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  if (out.empty()) {
    out = "file.bin";
  }
  return out;
}

std::filesystem::path AppCore::unique_download_path(const std::string &peer_id, const std::string &name) const {
  const auto base = data_dir_ / "downloads" / peer_id;
  std::error_code ec;
  std::filesystem::create_directories(base, ec);

  const auto clean = sanitize_filename(name);
  std::filesystem::path p = base / clean;
  if (!std::filesystem::exists(p)) {
    return p;
  }

  const auto stem = p.stem().string();
  const auto ext = p.extension().string();
  for (int i = 1; i < 10000; ++i) {
    auto candidate = base / (stem + "(" + std::to_string(i) + ")" + ext);
    if (!std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return base / (stem + "_" + std::to_string(now_ms()) + ext);
}

bool AppCore::is_image_path(const std::filesystem::path &p) {
  const auto ext = p.extension().string();
  std::string lower;
  lower.resize(ext.size());
  std::transform(ext.begin(), ext.end(), lower.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return lower == ".png" || lower == ".jpg" || lower == ".jpeg" || lower == ".bmp" || lower == ".gif" ||
      lower == ".webp";
}

} // namespace lantalk
