#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <commdlg.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

constexpr uint32_t kPacketMagic = 0x4C414E54;  // "LANT"
constexpr uint16_t kDiscoveryPort = 37021;
constexpr int kHeartbeatSeconds = 3;
constexpr int kPeerTimeoutSeconds = 12;
constexpr uint64_t kMaxTextBytes = 16 * 1024;
constexpr uint64_t kMaxFileBytes = 1024ULL * 1024ULL * 1024ULL;

enum class PacketType : uint8_t {
    Text = 1,
    File = 2,
};

struct Config {
    std::string userName;
    std::string userId;
    uint16_t listenPort = 39001;
};

struct Peer {
    std::string userId;
    std::string name;
    std::string ip;
    uint16_t port = 0;
    std::chrono::steady_clock::time_point lastSeen;
};

class NetworkRuntime {
public:
#ifdef _WIN32
    NetworkRuntime() {
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~NetworkRuntime() {
        WSACleanup();
    }
#else
    NetworkRuntime() = default;
    ~NetworkRuntime() = default;
#endif
};

int getSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void closeSocket(socket_t sock) {
#ifdef _WIN32
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
#else
    if (sock >= 0) {
        close(sock);
    }
#endif
}

bool isConnectInProgress(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
    return err == EINPROGRESS || err == EALREADY || err == EWOULDBLOCK;
#endif
}

bool setNonBlocking(socket_t sock, bool nonBlocking) {
#ifdef _WIN32
    u_long mode = nonBlocking ? 1UL : 0UL;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if (nonBlocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(sock, F_SETFL, flags) == 0;
#endif
}

uint64_t hostToNet64(uint64_t value) {
    const uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
    const uint32_t low = htonl(static_cast<uint32_t>(value & 0xFFFFFFFFULL));
    return (static_cast<uint64_t>(low) << 32) | high;
}

uint64_t netToHost64(uint64_t value) {
    const uint32_t low = ntohl(static_cast<uint32_t>(value >> 32));
    const uint32_t high = ntohl(static_cast<uint32_t>(value & 0xFFFFFFFFULL));
    return (static_cast<uint64_t>(high) << 32) | low;
}

bool sendAll(socket_t sock, const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        const int chunk = static_cast<int>(std::min<size_t>(remaining, 64 * 1024));
        const int sent = send(sock, ptr, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

bool recvAll(socket_t sock, void* data, size_t len) {
    char* ptr = static_cast<char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        const int chunk = static_cast<int>(std::min<size_t>(remaining, 64 * 1024));
        const int received = recv(sock, ptr, chunk, 0);
        if (received <= 0) {
            return false;
        }
        ptr += received;
        remaining -= static_cast<size_t>(received);
    }
    return true;
}

bool sendU8(socket_t sock, uint8_t value) {
    return sendAll(sock, &value, sizeof(value));
}

bool sendU16(socket_t sock, uint16_t value) {
    const uint16_t v = htons(value);
    return sendAll(sock, &v, sizeof(v));
}

bool sendU32(socket_t sock, uint32_t value) {
    const uint32_t v = htonl(value);
    return sendAll(sock, &v, sizeof(v));
}

bool sendU64(socket_t sock, uint64_t value) {
    const uint64_t v = hostToNet64(value);
    return sendAll(sock, &v, sizeof(v));
}

bool recvU8(socket_t sock, uint8_t& value) {
    return recvAll(sock, &value, sizeof(value));
}

bool recvU16(socket_t sock, uint16_t& value) {
    uint16_t v = 0;
    if (!recvAll(sock, &v, sizeof(v))) {
        return false;
    }
    value = ntohs(v);
    return true;
}

bool recvU32(socket_t sock, uint32_t& value) {
    uint32_t v = 0;
    if (!recvAll(sock, &v, sizeof(v))) {
        return false;
    }
    value = ntohl(v);
    return true;
}

bool recvU64(socket_t sock, uint64_t& value) {
    uint64_t v = 0;
    if (!recvAll(sock, &v, sizeof(v))) {
        return false;
    }
    value = netToHost64(v);
    return true;
}

std::string trim(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(start, end - start);
}

std::vector<std::string> split(const std::string& input, char delimiter) {
    std::vector<std::string> out;
    std::string current;
    std::istringstream iss(input);
    while (std::getline(iss, current, delimiter)) {
        out.push_back(current);
    }
    return out;
}

std::string stripQuotes(const std::string& input) {
    if (input.size() >= 2) {
        const char first = input.front();
        const char last = input.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return input.substr(1, input.size() - 2);
        }
    }
    return input;
}

std::string sanitizeHelloField(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\t' || ch == '\r' || ch == '\n') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return trim(out);
}

std::string sanitizeFileName(std::string fileName) {
    static const std::string forbidden = "\\/:*?\"<>|";
    for (char& ch : fileName) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 32 || forbidden.find(ch) != std::string::npos) {
            ch = '_';
        }
    }
    fileName = trim(fileName);
    if (fileName.empty() || fileName == "." || fileName == "..") {
        return "file.bin";
    }
    return fileName;
}

std::tm toLocalTime(std::time_t t) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &t);
#else
    localtime_r(&t, &result);
#endif
    return result;
}

std::string formatTime(std::time_t t) {
    const std::tm tm = toLocalTime(t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string nowTimeString() {
    return formatTime(std::time(nullptr));
}

std::string randomHex(std::mt19937_64& rng, size_t bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        const uint8_t value = static_cast<uint8_t>(rng() & 0xFFU);
        out.push_back(hex[(value >> 4) & 0x0F]);
        out.push_back(hex[value & 0x0F]);
    }
    return out;
}

std::string readEnvVar(const char* name) {
#ifdef _WIN32
    char* raw = nullptr;
    size_t len = 0;
    if (_dupenv_s(&raw, &len, name) != 0 || raw == nullptr) {
        return "";
    }
    std::string value(raw);
    std::free(raw);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : "";
#endif
}

uint64_t fnv1a64(const std::string& input) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : input) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string shortHashHex(const std::string& input) {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << (fnv1a64(input) & 0xFFFFFFFFULL);
    return oss.str();
}

fs::path getExecutablePath() {
#ifdef _WIN32
    std::vector<char> buffer(1024, '\0');
    while (true) {
        DWORD copied = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return fs::path();
        }
        if (copied < buffer.size() - 1) {
            return fs::path(std::string(buffer.data(), copied));
        }
        buffer.resize(buffer.size() * 2, '\0');
    }
#else
    std::vector<char> buffer(1024, '\0');
    while (true) {
        const ssize_t copied = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (copied < 0) {
            return fs::path();
        }
        if (static_cast<size_t>(copied) < buffer.size() - 1) {
            return fs::path(std::string(buffer.data(), static_cast<size_t>(copied)));
        }
        buffer.resize(buffer.size() * 2, '\0');
    }
#endif
}

class LanTalkApp {
public:
    LanTalkApp()
        : exePath_(getExecutablePath()),
          baseDir_(fs::current_path()),
          exeTag_(buildExeTag()),
          dataDir_(baseDir_ / ("data_" + exeTag_)),
          recvDir_(dataDir_ / "received"),
          configPath_(dataDir_ / "config.ini"),
          logPath_(dataDir_ / "chat.log"),
          rng_(std::random_device{}()) {}

    ~LanTalkApp() {
        stop();
    }

    bool init() {
        setLastError("");
        if (!loadOrCreateConfig()) {
            return false;
        }
        if (!acquireSingleInstanceLock()) {
            return false;
        }
        if (!initDiscoverySocket()) {
            return false;
        }
        if (!initListenSocket()) {
            return false;
        }
        return true;
    }

    bool startAsync() {
        if (running_.exchange(true)) {
            return true;
        }
        discoveryRecvThread_ = std::thread(&LanTalkApp::discoveryRecvLoop, this);
        discoverySendThread_ = std::thread(&LanTalkApp::discoverySendLoop, this);
        serverThread_ = std::thread(&LanTalkApp::serverLoop, this);

        printLine("LanTalk started.");
        printLine("Data directory: " + dataDir_.string());
        printLine("Local user: " + config_.userName + "  UserID: " + config_.userId +
                  "  Listen: " + std::to_string(config_.listenPort));
        return true;
    }

    void run() {
        startAsync();
        printHelp();

        inputLoop();
        stop();
    }

    void shutdown() {
        stop();
    }

    std::vector<Peer> snapshotPeers() {
        return getPeerSnapshot();
    }

    bool sendTextToUserId(const std::string& userId, const std::string& text, std::string* errorOut = nullptr) {
        Peer peer;
        if (!getPeerByUserId(userId, peer)) {
            if (errorOut != nullptr) {
                *errorOut = "Peer is offline.";
            }
            return false;
        }
        if (!sendTextToPeer(peer, text)) {
            if (errorOut != nullptr) {
                *errorOut = "Failed to send message.";
            }
            return false;
        }
        appendLog("OUT MSG to=" + peer.name + "(" + peer.ip + ") text=" + text);
        return true;
    }

    bool sendFileToUserId(const std::string& userId, const fs::path& filePath, std::string* errorOut = nullptr) {
        Peer peer;
        if (!getPeerByUserId(userId, peer)) {
            if (errorOut != nullptr) {
                *errorOut = "Peer is offline.";
            }
            return false;
        }
        if (!sendFileToPeer(peer, filePath)) {
            if (errorOut != nullptr) {
                *errorOut = "Failed to send file.";
            }
            return false;
        }
        appendLog("OUT FILE to=" + peer.name + "(" + peer.ip + ") path=" + filePath.string());
        return true;
    }

    bool updateLocalUserName(const std::string& newName, std::string* errorOut = nullptr) {
        std::string safeName = sanitizeHelloField(newName);
        if (safeName.empty()) {
            if (errorOut != nullptr) {
                *errorOut = "Name cannot be empty.";
            }
            return false;
        }
        config_.userName = safeName;
        if (!saveConfig()) {
            if (errorOut != nullptr) {
                *errorOut = "Failed to save name.";
            }
            return false;
        }
        broadcastHello();
        printLine("Name updated to: " + config_.userName);
        return true;
    }

    Config configCopy() const {
        return config_;
    }

    std::string dataDirString() const {
        return dataDir_.string();
    }

    std::string lastError() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }

    void setEventCallback(std::function<void(const std::string&)> cb) {
        std::lock_guard<std::mutex> lock(eventMutex_);
        eventCallback_ = std::move(cb);
    }

private:
    bool loadOrCreateConfig() {
        std::error_code ec;
        fs::create_directories(recvDir_, ec);
        if (ec) {
            setLastError("Failed to create data directory: " + ec.message());
            return false;
        }

        Config loaded;
        bool hasUser = false;
        bool hasUserId = false;
        bool hasPort = false;

        if (fs::exists(configPath_)) {
            std::ifstream in(configPath_);
            std::string line;
            while (std::getline(in, line)) {
                line = trim(line);
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                const size_t pos = line.find('=');
                if (pos == std::string::npos) {
                    continue;
                }
                const std::string key = trim(line.substr(0, pos));
                const std::string value = trim(line.substr(pos + 1));
                if (key == "username") {
                    loaded.userName = value;
                    hasUser = !value.empty();
                } else if (key == "user_id") {
                    loaded.userId = value;
                    hasUserId = !value.empty();
                } else if (key == "instance_id") {
                    loaded.userId = value;
                    hasUserId = !value.empty();
                } else if (key == "listen_port") {
                    try {
                        const int port = std::stoi(value);
                        if (port > 1024 && port <= 65535) {
                            loaded.listenPort = static_cast<uint16_t>(port);
                            hasPort = true;
                        }
                    } catch (...) {
                    }
                }
            }
        }

        if (!hasUser) {
            std::string envUser = readEnvVar("USERNAME");
            if (envUser.empty()) {
                envUser = readEnvVar("USER");
            }
            loaded.userName = !envUser.empty() ? envUser : "LanTalkUser";
            hasUser = true;
        }

        if (!hasUserId) {
            loaded.userId = randomHex(rng_, 8);
        }

        if (!hasPort) {
            loaded.listenPort = static_cast<uint16_t>(39001 + (rng_() % 2000));
        }

        loaded.userName = sanitizeHelloField(loaded.userName);
        if (loaded.userName.empty()) {
            loaded.userName = "LanTalkUser";
        }
        loaded.userId = trim(loaded.userId);
        if (loaded.userId.empty()) {
            loaded.userId = randomHex(rng_, 8);
        }

        config_ = loaded;
        return saveConfig();
    }

    bool saveConfig() {
        std::ofstream out(configPath_, std::ios::trunc);
        if (!out) {
            setLastError("Failed to write config: " + configPath_.string());
            return false;
        }
        out << "username=" << config_.userName << '\n';
        out << "user_id=" << config_.userId << '\n';
        out << "listen_port=" << config_.listenPort << '\n';
        return true;
    }

    std::string buildExeTag() const {
        fs::path exeNamePath = exePath_.filename();
        if (exeNamePath.empty()) {
            exeNamePath = "lantalk";
        }
        std::string stem = sanitizeFileName(exeNamePath.stem().string());
        if (stem.empty()) {
            stem = "lantalk";
        }
        std::string hashSource = exePath_.string();
        if (hashSource.empty()) {
            hashSource = stem;
        }
        return stem + "_" + shortHashHex(hashSource);
    }

    bool acquireSingleInstanceLock() {
        if (singleInstanceLocked_) {
            return true;
        }

#ifdef _WIN32
        std::string lockIdentity = exePath_.string();
        if (lockIdentity.empty()) {
            lockIdentity = (baseDir_ / "lantalk").string();
        }
        std::string mutexName = "LanTalkMutex_" + shortHashHex(lockIdentity);
        instanceMutex_ = CreateMutexA(nullptr, TRUE, mutexName.c_str());
        if (instanceMutex_ == nullptr) {
            setLastError("Failed to create single-instance mutex.");
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            setLastError("This executable is already running in current directory.");
            CloseHandle(instanceMutex_);
            instanceMutex_ = nullptr;
            return false;
        }
#else
        lockFilePath_ = dataDir_ / ".instance.lock";
        lockFd_ = open(lockFilePath_.string().c_str(), O_RDWR | O_CREAT, 0644);
        if (lockFd_ < 0) {
            setLastError("Failed to create lock file: " + lockFilePath_.string());
            return false;
        }
        if (flock(lockFd_, LOCK_EX | LOCK_NB) != 0) {
            setLastError("This executable is already running in current directory.");
            close(lockFd_);
            lockFd_ = -1;
            return false;
        }
#endif

        singleInstanceLocked_ = true;
        return true;
    }

    void releaseSingleInstanceLock() {
        if (!singleInstanceLocked_) {
            return;
        }
#ifdef _WIN32
        if (instanceMutex_ != nullptr) {
            ReleaseMutex(instanceMutex_);
            CloseHandle(instanceMutex_);
            instanceMutex_ = nullptr;
        }
#else
        if (lockFd_ >= 0) {
            flock(lockFd_, LOCK_UN);
            close(lockFd_);
            lockFd_ = -1;
        }
#endif
        singleInstanceLocked_ = false;
    }

    bool initDiscoverySocket() {
        udpSock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udpSock_ == kInvalidSocket) {
            setLastError("Failed to create discovery socket.");
            return false;
        }

        int opt = 1;
        setsockopt(udpSock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        setsockopt(udpSock_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kDiscoveryPort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(udpSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            setLastError("Failed to bind discovery socket.");
            closeSocket(udpSock_);
            udpSock_ = kInvalidSocket;
            return false;
        }

        return true;
    }

    bool initListenSocket() {
        uint16_t chosenPort = 0;
        socket_t chosenSocket = kInvalidSocket;

        for (int i = 0; i < 100; ++i) {
            const uint16_t tryPort = static_cast<uint16_t>(config_.listenPort + i);
            socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == kInvalidSocket) {
                continue;
            }

            int opt = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(tryPort);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);

            if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(sock, 16) == 0) {
                chosenSocket = sock;
                chosenPort = tryPort;
                break;
            }
            closeSocket(sock);
        }

        if (chosenSocket == kInvalidSocket) {
            setLastError("Failed to open TCP listen socket.");
            return false;
        }

        listenSock_ = chosenSocket;
        if (config_.listenPort != chosenPort) {
            config_.listenPort = chosenPort;
            saveConfig();
        }
        return true;
    }

    void stop() {
        const bool wasRunning = running_.exchange(false);
        if (!wasRunning && udpSock_ == kInvalidSocket && listenSock_ == kInvalidSocket && !singleInstanceLocked_) {
            return;
        }

        if (udpSock_ != kInvalidSocket) {
            closeSocket(udpSock_);
            udpSock_ = kInvalidSocket;
        }
        if (listenSock_ != kInvalidSocket) {
            closeSocket(listenSock_);
            listenSock_ = kInvalidSocket;
        }

        if (discoveryRecvThread_.joinable()) {
            discoveryRecvThread_.join();
        }
        if (discoverySendThread_.joinable()) {
            discoverySendThread_.join();
        }
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
        releaseSingleInstanceLock();
    }

    void printHelp() {
        printLine("Commands:");
        printLine("  /help                     Show this help");
        printLine("  /peers                    List online peers");
        printLine("  /name <new_name>          Change local display name");
        printLine("  /msg <index> <text>       Send text to one peer");
        printLine("  /all <text>               Broadcast text to all peers");
        printLine("  /file <index> <path>      Send file to one peer");
        printLine("  /quit                     Exit");
        printLine("Tip: typing plain text is equal to /all <text>");
    }

    void inputLoop() {
        std::string line;
        while (running_.load()) {
            {
                std::lock_guard<std::mutex> lock(ioMutex_);
                std::cout << "> " << std::flush;
            }
            if (!std::getline(std::cin, line)) {
                running_.store(false);
                break;
            }

            line = trim(line);
            if (line.empty()) {
                continue;
            }

            if (line == "/quit") {
                running_.store(false);
                break;
            }

            if (line == "/help") {
                printHelp();
                continue;
            }

            if (line == "/peers") {
                const auto peers = getPeerSnapshot();
                if (peers.empty()) {
                    printLine("No peers discovered yet.");
                } else {
                    printLine("Peers:");
                    const auto now = std::chrono::steady_clock::now();
                    for (size_t i = 0; i < peers.size(); ++i) {
                        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - peers[i].lastSeen).count();
                        std::ostringstream oss;
                        oss << "  [" << (i + 1) << "] " << peers[i].name << "  id=" << peers[i].userId << "  "
                            << peers[i].ip << ':' << peers[i].port
                            << "  seen " << age << "s ago";
                        printLine(oss.str());
                    }
                }
                continue;
            }

            if (line.rfind("/name ", 0) == 0) {
                std::string newName = sanitizeHelloField(trim(line.substr(6)));
                if (newName.empty()) {
                    printLine("Name cannot be empty.");
                    continue;
                }
                config_.userName = newName;
                if (saveConfig()) {
                    printLine("Name updated to: " + config_.userName);
                    broadcastHello();
                }
                continue;
            }

            if (line.rfind("/msg ", 0) == 0) {
                std::istringstream iss(line);
                std::string cmd;
                int index = 0;
                iss >> cmd >> index;
                std::string text;
                std::getline(iss, text);
                text = trim(text);
                if (index <= 0 || text.empty()) {
                    printLine("Usage: /msg <index> <text>");
                    continue;
                }
                Peer peer;
                if (!resolvePeerByIndex(index, peer)) {
                    printLine("Invalid peer index.");
                    continue;
                }
                if (sendTextToPeer(peer, text)) {
                    printLine("[Sent] to " + peer.name + ": " + text);
                    appendLog("OUT MSG to=" + peer.name + "(" + peer.ip + ") text=" + text);
                } else {
                    printLine("Failed to send message.");
                }
                continue;
            }

            if (line.rfind("/file ", 0) == 0) {
                std::istringstream iss(line);
                std::string cmd;
                int index = 0;
                iss >> cmd >> index;
                std::string pathText;
                std::getline(iss, pathText);
                pathText = stripQuotes(trim(pathText));
                if (index <= 0 || pathText.empty()) {
                    printLine("Usage: /file <index> <path>");
                    continue;
                }
                Peer peer;
                if (!resolvePeerByIndex(index, peer)) {
                    printLine("Invalid peer index.");
                    continue;
                }
                fs::path filePath = fs::path(pathText);
                std::error_code ec;
                if (!fs::exists(filePath, ec) || !fs::is_regular_file(filePath, ec)) {
                    printLine("File does not exist: " + pathText);
                    continue;
                }
                if (sendFileToPeer(peer, filePath)) {
                    const std::string name = filePath.filename().string();
                    printLine("[Sent file] to " + peer.name + ": " + name);
                    appendLog("OUT FILE to=" + peer.name + "(" + peer.ip + ") name=" + name + " path=" + filePath.string());
                } else {
                    printLine("Failed to send file.");
                }
                continue;
            }

            if (line.rfind("/all ", 0) == 0) {
                line = trim(line.substr(5));
                if (line.empty()) {
                    printLine("Usage: /all <text>");
                    continue;
                }
            }

            const std::string textToSend = (line.rfind('/', 0) == 0) ? "" : line;
            if (textToSend.empty()) {
                printLine("Unknown command. Use /help");
                continue;
            }

            const auto peers = getPeerSnapshot();
            if (peers.empty()) {
                printLine("No peers available.");
                continue;
            }
            int sentCount = 0;
            for (const Peer& peer : peers) {
                if (sendTextToPeer(peer, textToSend)) {
                    ++sentCount;
                }
            }
            std::ostringstream oss;
            oss << "Broadcast sent to " << sentCount << '/' << peers.size() << " peers.";
            printLine(oss.str());
            appendLog("OUT BCAST peers=" + std::to_string(peers.size()) + " text=" + textToSend);
        }
    }

    void discoveryRecvLoop() {
        while (running_.load()) {
            if (udpSock_ == kInvalidSocket) {
                break;
            }

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(udpSock_, &readSet);
            timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;

#ifdef _WIN32
            const int rc = select(0, &readSet, nullptr, nullptr, &tv);
#else
            const int rc = select(udpSock_ + 1, &readSet, nullptr, nullptr, &tv);
#endif
            if (!running_.load()) {
                break;
            }
            if (rc <= 0) {
                continue;
            }

            sockaddr_in from{};
#ifdef _WIN32
            int fromLen = sizeof(from);
#else
            socklen_t fromLen = sizeof(from);
#endif
            char buffer[512] = {0};
            const int n = recvfrom(udpSock_, buffer, static_cast<int>(sizeof(buffer) - 1), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) {
                continue;
            }

            const std::string msg(buffer, buffer + n);
            const auto parts = split(trim(msg), '\t');
            if (parts.size() != 4 || parts[0] != "HELLO") {
                continue;
            }

            const std::string peerUserId = parts[1];
            if (peerUserId.empty()) {
                continue;
            }

            std::string peerName = sanitizeHelloField(parts[2]);
            if (peerName.empty()) {
                peerName = "LanTalkUser";
            }

            int peerPort = 0;
            try {
                peerPort = std::stoi(parts[3]);
            } catch (...) {
                continue;
            }
            if (peerPort <= 0 || peerPort > 65535) {
                continue;
            }
            if (peerUserId == config_.userId) {
                continue;
            }

            char ipBuf[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &from.sin_addr, ipBuf, sizeof(ipBuf)) == nullptr) {
                continue;
            }
            bool isNew = false;
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                auto it = peers_.find(peerUserId);
                if (it == peers_.end()) {
                    Peer peer;
                    peer.userId = peerUserId;
                    peer.name = peerName;
                    peer.ip = ipBuf;
                    peer.port = static_cast<uint16_t>(peerPort);
                    peer.lastSeen = std::chrono::steady_clock::now();
                    peers_[peerUserId] = peer;
                    isNew = true;
                } else {
                    it->second.userId = peerUserId;
                    it->second.name = peerName;
                    it->second.ip = ipBuf;
                    it->second.port = static_cast<uint16_t>(peerPort);
                    it->second.lastSeen = std::chrono::steady_clock::now();
                }
            }
            if (isNew) {
                printLine("[Peer online] " + peerName + " " + std::string(ipBuf) + ":" + std::to_string(peerPort));
            }
        }
    }

    void discoverySendLoop() {
        while (running_.load()) {
            broadcastHello();
            prunePeers();
            for (int i = 0; i < kHeartbeatSeconds; ++i) {
                if (!running_.load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void broadcastHello() {
        if (udpSock_ == kInvalidSocket) {
            return;
        }
        const std::string payload = "HELLO\t" + config_.userId + "\t" + sanitizeHelloField(config_.userName) + "\t" +
                                    std::to_string(config_.listenPort);

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(kDiscoveryPort);
        target.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        sendto(udpSock_, payload.data(), static_cast<int>(payload.size()), 0,
               reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }

    void prunePeers() {
        std::vector<Peer> removed;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            for (auto it = peers_.begin(); it != peers_.end();) {
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastSeen).count();
                if (age > kPeerTimeoutSeconds) {
                    removed.push_back(it->second);
                    it = peers_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (const Peer& peer : removed) {
            printLine("[Peer offline] " + peer.name + " " + peer.ip);
        }
    }

    void serverLoop() {
        while (running_.load()) {
            if (listenSock_ == kInvalidSocket) {
                break;
            }

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSock_, &readSet);
            timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;

#ifdef _WIN32
            const int rc = select(0, &readSet, nullptr, nullptr, &tv);
#else
            const int rc = select(listenSock_ + 1, &readSet, nullptr, nullptr, &tv);
#endif
            if (!running_.load()) {
                break;
            }
            if (rc <= 0) {
                continue;
            }

            sockaddr_in remote{};
#ifdef _WIN32
            int remoteLen = sizeof(remote);
#else
            socklen_t remoteLen = sizeof(remote);
#endif
            socket_t client = accept(listenSock_, reinterpret_cast<sockaddr*>(&remote), &remoteLen);
            if (client == kInvalidSocket) {
                continue;
            }

            std::thread(&LanTalkApp::handleClient, this, client, remote).detach();
        }
    }

    void handleClient(socket_t client, sockaddr_in remote) {
        struct SocketGuard {
            socket_t sock;
            ~SocketGuard() { closeSocket(sock); }
        } guard{client};

        uint32_t magic = 0;
        uint8_t typeRaw = 0;
        uint16_t fromLen = 0;
        uint16_t nameLen = 0;
        uint64_t ts = 0;
        uint64_t payloadLen = 0;

        if (!recvU32(client, magic) || !recvU8(client, typeRaw) || !recvU16(client, fromLen) || !recvU16(client, nameLen) ||
            !recvU64(client, ts) || !recvU64(client, payloadLen)) {
            return;
        }

        if (magic != kPacketMagic) {
            return;
        }

        if (fromLen == 0 || fromLen > 128 || nameLen > 260) {
            return;
        }

        const PacketType type = static_cast<PacketType>(typeRaw);
        if (type == PacketType::Text) {
            if (payloadLen > kMaxTextBytes || nameLen != 0) {
                return;
            }
        } else if (type == PacketType::File) {
            if (payloadLen > kMaxFileBytes) {
                return;
            }
        } else {
            return;
        }

        std::string fromName(fromLen, '\0');
        if (!recvAll(client, fromName.data(), fromName.size())) {
            return;
        }

        std::string fileName;
        if (nameLen > 0) {
            fileName.resize(nameLen);
            if (!recvAll(client, fileName.data(), fileName.size())) {
                return;
            }
            fileName = sanitizeFileName(fileName);
        }

        char ipBuf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &remote.sin_addr, ipBuf, sizeof(ipBuf)) == nullptr) {
            constexpr char kUnknownIp[] = "unknown";
            std::memcpy(ipBuf, kUnknownIp, sizeof(kUnknownIp));
        }
        const std::string remoteIp(ipBuf);

        if (type == PacketType::Text) {
            std::string text(static_cast<size_t>(payloadLen), '\0');
            if (!recvAll(client, text.data(), text.size())) {
                return;
            }
            onIncomingText(fromName, remoteIp, text, static_cast<std::time_t>(ts));
            return;
        }

        fs::path savePath = uniqueFilePath(fromName, fileName);
        std::ofstream out(savePath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }

        uint64_t remaining = payloadLen;
        std::vector<char> chunk(64 * 1024);
        while (remaining > 0) {
            const size_t chunkSize = static_cast<size_t>(std::min<uint64_t>(remaining, chunk.size()));
            if (!recvAll(client, chunk.data(), chunkSize)) {
                out.close();
                std::error_code ec;
                fs::remove(savePath, ec);
                return;
            }
            out.write(chunk.data(), static_cast<std::streamsize>(chunkSize));
            if (!out) {
                out.close();
                std::error_code ec;
                fs::remove(savePath, ec);
                return;
            }
            remaining -= chunkSize;
        }

        out.close();
        onIncomingFile(fromName, remoteIp, fileName, savePath, payloadLen, static_cast<std::time_t>(ts));
    }

    bool sendTextToPeer(const Peer& peer, const std::string& text) {
        if (text.empty() || text.size() > kMaxTextBytes) {
            return false;
        }

        socket_t sock = connectToPeer(peer, 3000);
        if (sock == kInvalidSocket) {
            return false;
        }
        struct SocketGuard {
            socket_t sock;
            ~SocketGuard() { closeSocket(sock); }
        } guard{sock};

        const uint64_t ts = static_cast<uint64_t>(std::time(nullptr));
        if (!sendHeader(sock, PacketType::Text, config_.userName, "", static_cast<uint64_t>(text.size()), ts)) {
            return false;
        }
        return sendAll(sock, text.data(), text.size());
    }

    bool sendFileToPeer(const Peer& peer, const fs::path& filePath) {
        std::error_code ec;
        const uint64_t fileSize = fs::file_size(filePath, ec);
        if (ec || fileSize == 0 || fileSize > kMaxFileBytes) {
            return false;
        }

        std::ifstream in(filePath, std::ios::binary);
        if (!in) {
            return false;
        }

        std::string fileName = filePath.filename().string();
        fileName = sanitizeFileName(fileName);

        socket_t sock = connectToPeer(peer, 5000);
        if (sock == kInvalidSocket) {
            return false;
        }
        struct SocketGuard {
            socket_t sock;
            ~SocketGuard() { closeSocket(sock); }
        } guard{sock};

        const uint64_t ts = static_cast<uint64_t>(std::time(nullptr));
        if (!sendHeader(sock, PacketType::File, config_.userName, fileName, fileSize, ts)) {
            return false;
        }

        std::vector<char> chunk(64 * 1024);
        uint64_t remaining = fileSize;
        while (remaining > 0) {
            const size_t chunkSize = static_cast<size_t>(std::min<uint64_t>(remaining, chunk.size()));
            in.read(chunk.data(), static_cast<std::streamsize>(chunkSize));
            const std::streamsize got = in.gcount();
            if (got <= 0) {
                return false;
            }
            if (!sendAll(sock, chunk.data(), static_cast<size_t>(got))) {
                return false;
            }
            remaining -= static_cast<uint64_t>(got);
        }

        return true;
    }

    socket_t connectToPeer(const Peer& peer, int timeoutMs) {
        socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == kInvalidSocket) {
            return kInvalidSocket;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(peer.port);
        if (inet_pton(AF_INET, peer.ip.c_str(), &addr.sin_addr) <= 0) {
            closeSocket(sock);
            return kInvalidSocket;
        }

        if (!setNonBlocking(sock, true)) {
            closeSocket(sock);
            return kInvalidSocket;
        }

        int rc = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc != 0) {
            const int err = getSocketError();
            if (!isConnectInProgress(err)) {
                closeSocket(sock);
                return kInvalidSocket;
            }

            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
            rc = select(0, nullptr, &writeSet, nullptr, &tv);
#else
            rc = select(sock + 1, nullptr, &writeSet, nullptr, &tv);
#endif
            if (rc <= 0) {
                closeSocket(sock);
                return kInvalidSocket;
            }

            int soError = 0;
#ifdef _WIN32
            int soLen = sizeof(soError);
#else
            socklen_t soLen = sizeof(soError);
#endif
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen) != 0 || soError != 0) {
                closeSocket(sock);
                return kInvalidSocket;
            }
        }

        if (!setNonBlocking(sock, false)) {
            closeSocket(sock);
            return kInvalidSocket;
        }
        return sock;
    }

    bool sendHeader(socket_t sock,
                    PacketType type,
                    const std::string& from,
                    const std::string& fileName,
                    uint64_t payloadLen,
                    uint64_t ts) {
        if (from.empty() || from.size() > 128 || fileName.size() > 260) {
            return false;
        }

        if (!sendU32(sock, kPacketMagic) || !sendU8(sock, static_cast<uint8_t>(type)) ||
            !sendU16(sock, static_cast<uint16_t>(from.size())) || !sendU16(sock, static_cast<uint16_t>(fileName.size())) ||
            !sendU64(sock, ts) || !sendU64(sock, payloadLen)) {
            return false;
        }

        if (!sendAll(sock, from.data(), from.size())) {
            return false;
        }

        if (!fileName.empty() && !sendAll(sock, fileName.data(), fileName.size())) {
            return false;
        }

        return true;
    }

    void onIncomingText(const std::string& fromName,
                        const std::string& remoteIp,
                        const std::string& text,
                        std::time_t sentTime) {
        const std::string stamp = formatTime(sentTime);
        printLine("[" + stamp + "] [MSG] " + fromName + "(" + remoteIp + "): " + text);
        appendLog("IN MSG from=" + fromName + "(" + remoteIp + ") text=" + text);
    }

    void onIncomingFile(const std::string& fromName,
                        const std::string& remoteIp,
                        const std::string& fileName,
                        const fs::path& savePath,
                        uint64_t size,
                        std::time_t sentTime) {
        const std::string stamp = formatTime(sentTime);
        printLine("[" + stamp + "] [FILE] " + fromName + "(" + remoteIp + ") -> " + savePath.string() +
                  " (" + std::to_string(size) + " bytes)");
        appendLog("IN FILE from=" + fromName + "(" + remoteIp + ") name=" + fileName + " save=" + savePath.string() +
                  " bytes=" + std::to_string(size));
    }

    fs::path uniqueFilePath(const std::string& sender, const std::string& originalName) {
        const std::string safeSender = sanitizeFileName(sender);
        const std::string safeName = sanitizeFileName(originalName);
        const fs::path senderDir = recvDir_ / safeSender;
        std::error_code ec;
        fs::create_directories(senderDir, ec);

        fs::path candidate = senderDir / safeName;
        if (!fs::exists(candidate, ec)) {
            return candidate;
        }

        const fs::path stem = candidate.stem();
        const fs::path ext = candidate.extension();
        for (int i = 1; i <= 9999; ++i) {
            std::ostringstream oss;
            oss << stem.string() << '_' << i << ext.string();
            candidate = senderDir / oss.str();
            if (!fs::exists(candidate, ec)) {
                return candidate;
            }
        }

        const auto ts = static_cast<long long>(std::time(nullptr));
        return senderDir / (safeName + "_" + std::to_string(ts));
    }

    std::vector<Peer> getPeerSnapshot() {
        std::vector<Peer> out;
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            out.reserve(peers_.size());
            for (const auto& kv : peers_) {
                out.push_back(kv.second);
            }
        }
        std::sort(out.begin(), out.end(), [](const Peer& a, const Peer& b) {
            if (a.name != b.name) {
                return a.name < b.name;
            }
            if (a.ip != b.ip) {
                return a.ip < b.ip;
            }
            return a.port < b.port;
        });
        return out;
    }

    bool resolvePeerByIndex(int index, Peer& outPeer) {
        const auto peers = getPeerSnapshot();
        if (index <= 0 || static_cast<size_t>(index) > peers.size()) {
            return false;
        }
        outPeer = peers[static_cast<size_t>(index - 1)];
        return true;
    }

    bool getPeerByUserId(const std::string& userId, Peer& outPeer) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(userId);
        if (it == peers_.end()) {
            return false;
        }
        outPeer = it->second;
        return true;
    }

    void setLastError(const std::string& errorText) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = errorText;
    }

    void appendLog(const std::string& line) {
        std::lock_guard<std::mutex> lock(logMutex_);
        std::ofstream out(logPath_, std::ios::app);
        if (!out) {
            return;
        }
        out << '[' << nowTimeString() << "] " << line << '\n';
    }

    void printLine(const std::string& line) {
        std::function<void(const std::string&)> callback;
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback(line);
            return;
        }
        std::lock_guard<std::mutex> lock(ioMutex_);
        std::cout << line << '\n';
    }

private:
    fs::path exePath_;
    fs::path baseDir_;
    std::string exeTag_;
    fs::path dataDir_;
    fs::path recvDir_;
    fs::path configPath_;
    fs::path logPath_;
    fs::path lockFilePath_;

    Config config_;

    std::atomic<bool> running_{false};
    socket_t udpSock_ = kInvalidSocket;
    socket_t listenSock_ = kInvalidSocket;

    std::thread discoveryRecvThread_;
    std::thread discoverySendThread_;
    std::thread serverThread_;

    std::mutex peersMutex_;
    std::mutex ioMutex_;
    std::mutex logMutex_;
    std::mutex eventMutex_;
    mutable std::mutex errorMutex_;

    std::map<std::string, Peer> peers_;
    std::mt19937_64 rng_;
    std::function<void(const std::string&)> eventCallback_;
    std::string lastError_;
    bool singleInstanceLocked_ = false;

#ifdef _WIN32
    HANDLE instanceMutex_ = nullptr;
#else
    int lockFd_ = -1;
#endif
};

#ifdef _WIN32
constexpr UINT kMsgLogLine = WM_APP + 1;
constexpr UINT_PTR kTimerPeerRefresh = 1;

enum ControlId : int {
    kCtlPeerList = 1001,
    kCtlLogEdit = 1002,
    kCtlInputEdit = 1003,
    kCtlSendBtn = 1004,
    kCtlFileBtn = 1005,
    kCtlNameEdit = 1006,
    kCtlRenameBtn = 1007,
    kCtlStatus = 1008,
};

class WinGuiApp {
public:
    int run(HINSTANCE instance, int showCmd) {
        instance_ = instance;

        NetworkRuntime runtime;
        (void)runtime;

        app_.setEventCallback([this](const std::string& line) {
            if (hwnd_ == nullptr) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(pendingLogMutex_);
                pendingLogs_.push_back(line);
            }
            PostMessageA(hwnd_, kMsgLogLine, 0, 0);
        });

        if (!app_.init()) {
            const std::string errorText = app_.lastError().empty() ? "Initialization failed." : app_.lastError();
            MessageBoxA(nullptr, errorText.c_str(), "LanTalk", MB_ICONERROR | MB_OK);
            return 1;
        }

        if (!registerWindowClass()) {
            MessageBoxA(nullptr, "Failed to register window class.", "LanTalk", MB_ICONERROR | MB_OK);
            app_.shutdown();
            return 1;
        }

        hwnd_ = CreateWindowExA(0,
                                windowClassName(),
                                "LanTalk",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                1100,
                                720,
                                nullptr,
                                nullptr,
                                instance_,
                                this);
        if (hwnd_ == nullptr) {
            MessageBoxA(nullptr, "Failed to create main window.", "LanTalk", MB_ICONERROR | MB_OK);
            app_.shutdown();
            return 1;
        }

        ShowWindow(hwnd_, showCmd);
        UpdateWindow(hwnd_);

        app_.startAsync();
        started_ = true;
        refreshStatusBar();
        refreshPeerList();
        SetTimer(hwnd_, kTimerPeerRefresh, 1000, nullptr);

        MSG msg{};
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    static const char* windowClassName() {
        return "LanTalkMainWindow";
    }

    static LRESULT CALLBACK setupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTA*>(lParam);
            auto* self = static_cast<WinGuiApp*>(cs->lpCreateParams);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return self->wndProc(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK forwardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<WinGuiApp*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcA(hwnd, msg, wParam, lParam);
        }
        return self->wndProc(hwnd, msg, wParam, lParam);
    }

    bool registerWindowClass() const {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &WinGuiApp::setupWndProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = windowClassName();
        if (RegisterClassExA(&wc) != 0) {
            return true;
        }
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void initControls(HWND hwnd) {
        font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        peerList_ = CreateWindowExA(WS_EX_CLIENTEDGE,
                                    "LISTBOX",
                                    "",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                                    0,
                                    0,
                                    100,
                                    100,
                                    hwnd,
                                    reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtlPeerList)),
                                    instance_,
                                    nullptr);

        logEdit_ = CreateWindowExA(WS_EX_CLIENTEDGE,
                                   "EDIT",
                                   "",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                   0,
                                   0,
                                   100,
                                   100,
                                   hwnd,
                                   reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtlLogEdit)),
                                   instance_,
                                   nullptr);

        inputEdit_ = CreateWindowExA(WS_EX_CLIENTEDGE,
                                     "EDIT",
                                     "",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     0,
                                     0,
                                     100,
                                     24,
                                     hwnd,
                                     reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtlInputEdit)),
                                     instance_,
                                     nullptr);

        sendBtn_ = CreateWindowExA(0,
                                   "BUTTON",
                                   "Send",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0,
                                   0,
                                   80,
                                   24,
                                   hwnd,
                                   reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtlSendBtn)),
                                   instance_,
                                   nullptr);

        fileBtn_ = CreateWindowExA(0,
                                   "BUTTON",
                                   "Send File",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0,
                                   0,
                                   90,
                                   24,
                                   hwnd,
                                   reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtlFileBtn)),
                                   instance_,
                                   nullptr);

        nameEdit_ = CreateWindowExA(WS_EX_CLIENTEDGE,
                                    "EDIT",
                                    "",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    0,
                                    0,
                                    100,
                                    24,
                                    hwnd,
                                    reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtlNameEdit)),
                                    instance_,
                                    nullptr);

        renameBtn_ = CreateWindowExA(0,
                                     "BUTTON",
                                     "Set Name",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     0,
                                     0,
                                     90,
                                     24,
                                     hwnd,
                                     reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtlRenameBtn)),
                                     instance_,
                                     nullptr);

        statusStatic_ = CreateWindowExA(0,
                                        "STATIC",
                                        "",
                                        WS_CHILD | WS_VISIBLE,
                                        0,
                                        0,
                                        100,
                                        24,
                                        hwnd,
                                        reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtlStatus)),
                                        instance_,
                                        nullptr);

        const HWND controls[] = {peerList_, logEdit_, inputEdit_, sendBtn_, fileBtn_, nameEdit_, renameBtn_, statusStatic_};
        for (HWND control : controls) {
            SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }

        const Config cfg = app_.configCopy();
        SetWindowTextA(nameEdit_, cfg.userName.c_str());
    }

    static std::string getWindowTextString(HWND control) {
        const int length = GetWindowTextLengthA(control);
        if (length <= 0) {
            return "";
        }
        std::string text(static_cast<size_t>(length) + 1, '\0');
        if (length > 0) {
            GetWindowTextA(control, text.data(), length + 1);
        }
        text.resize(static_cast<size_t>(length));
        return text;
    }

    void layoutControls(int width, int height) {
        const int margin = 8;
        const int leftWidth = std::max(220, std::min(320, width / 4));
        const int rightX = leftWidth + margin * 2;
        const int rightWidth = width - rightX - margin;
        const int topRowHeight = 26;
        const int secondRowHeight = 28;
        const int bottomRowHeight = 30;

        MoveWindow(peerList_, margin, margin, leftWidth, height - margin * 2, TRUE);
        MoveWindow(statusStatic_, rightX, margin, rightWidth, topRowHeight, TRUE);

        const int renameBtnWidth = 90;
        MoveWindow(nameEdit_, rightX, margin + topRowHeight + margin, rightWidth - renameBtnWidth - margin, secondRowHeight, TRUE);
        MoveWindow(renameBtn_,
                   rightX + rightWidth - renameBtnWidth,
                   margin + topRowHeight + margin,
                   renameBtnWidth,
                   secondRowHeight,
                   TRUE);

        const int bottomY = height - margin - bottomRowHeight;
        const int sendWidth = 80;
        const int fileWidth = 90;
        MoveWindow(inputEdit_,
                   rightX,
                   bottomY,
                   rightWidth - sendWidth - fileWidth - margin * 2,
                   bottomRowHeight,
                   TRUE);
        MoveWindow(sendBtn_, rightX + rightWidth - sendWidth - fileWidth - margin, bottomY, sendWidth, bottomRowHeight, TRUE);
        MoveWindow(fileBtn_, rightX + rightWidth - fileWidth, bottomY, fileWidth, bottomRowHeight, TRUE);

        const int logTop = margin + topRowHeight + margin + secondRowHeight + margin;
        const int logHeight = bottomY - logTop - margin;
        MoveWindow(logEdit_, rightX, logTop, rightWidth, std::max(100, logHeight), TRUE);
    }

    void appendLogLine(const std::string& line) {
        const int len = GetWindowTextLengthA(logEdit_);
        SendMessageA(logEdit_, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
        const std::string withNewLine = line + "\r\n";
        SendMessageA(logEdit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(withNewLine.c_str()));
        SendMessageA(logEdit_, EM_SCROLLCARET, 0, 0);
    }

    void refreshStatusBar() {
        const Config cfg = app_.configCopy();
        std::string text = "Local: " + cfg.userName + " | UserID: " + cfg.userId + " | Data: " + app_.dataDirString();
        SetWindowTextA(statusStatic_, text.c_str());
    }

    void refreshPeerList() {
        std::string selectedUserId;
        const int selectedIndex = static_cast<int>(SendMessageA(peerList_, LB_GETCURSEL, 0, 0));
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(peerUserIds_.size())) {
            selectedUserId = peerUserIds_[static_cast<size_t>(selectedIndex)];
        }

        const auto peers = app_.snapshotPeers();
        peerUserIds_.clear();
        SendMessageA(peerList_, LB_RESETCONTENT, 0, 0);
        for (const Peer& peer : peers) {
            std::ostringstream oss;
            oss << peer.name << " [" << peer.userId << "] " << peer.ip << ':' << peer.port;
            const std::string line = oss.str();
            SendMessageA(peerList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
            peerUserIds_.push_back(peer.userId);
        }

        if (!selectedUserId.empty()) {
            for (size_t i = 0; i < peerUserIds_.size(); ++i) {
                if (peerUserIds_[i] == selectedUserId) {
                    SendMessageA(peerList_, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
                    return;
                }
            }
        }
        if (!peerUserIds_.empty()) {
            SendMessageA(peerList_, LB_SETCURSEL, 0, 0);
        }
    }

    bool selectedPeerUserId(std::string& outUserId) const {
        const int index = static_cast<int>(SendMessageA(peerList_, LB_GETCURSEL, 0, 0));
        if (index < 0 || index >= static_cast<int>(peerUserIds_.size())) {
            return false;
        }
        outUserId = peerUserIds_[static_cast<size_t>(index)];
        return true;
    }

    std::string peerNameByUserId(const std::string& userId) {
        const auto peers = app_.snapshotPeers();
        for (const auto& peer : peers) {
            if (peer.userId == userId) {
                return peer.name;
            }
        }
        return userId;
    }

    void onSendMessage() {
        std::string userId;
        if (!selectedPeerUserId(userId)) {
            MessageBoxA(hwnd_, "Please select a peer first.", "LanTalk", MB_ICONINFORMATION | MB_OK);
            return;
        }
        std::string text = trim(getWindowTextString(inputEdit_));
        if (text.empty()) {
            return;
        }
        std::string errorText;
        if (!app_.sendTextToUserId(userId, text, &errorText)) {
            MessageBoxA(hwnd_, errorText.c_str(), "LanTalk", MB_ICONERROR | MB_OK);
            refreshPeerList();
            return;
        }
        appendLogLine("[Sent] to " + peerNameByUserId(userId) + ": " + text);
        SetWindowTextA(inputEdit_, "");
    }

    void onSendFile() {
        std::string userId;
        if (!selectedPeerUserId(userId)) {
            MessageBoxA(hwnd_, "Please select a peer first.", "LanTalk", MB_ICONINFORMATION | MB_OK);
            return;
        }

        char pathBuffer[MAX_PATH] = {0};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFile = pathBuffer;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = "All Files\0*.*\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameA(&ofn)) {
            return;
        }

        std::string errorText;
        if (!app_.sendFileToUserId(userId, fs::path(pathBuffer), &errorText)) {
            MessageBoxA(hwnd_, errorText.c_str(), "LanTalk", MB_ICONERROR | MB_OK);
            refreshPeerList();
            return;
        }
        appendLogLine("[Sent file] to " + peerNameByUserId(userId) + ": " + fs::path(pathBuffer).filename().string());
    }

    void onRename() {
        std::string name = trim(getWindowTextString(nameEdit_));
        std::string errorText;
        if (!app_.updateLocalUserName(name, &errorText)) {
            MessageBoxA(hwnd_, errorText.c_str(), "LanTalk", MB_ICONERROR | MB_OK);
            return;
        }
        refreshStatusBar();
    }

    void shutdownApp() {
        if (!started_) {
            return;
        }
        app_.setEventCallback(nullptr);
        app_.shutdown();
        started_ = false;
    }

    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_NCCREATE:
                SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WinGuiApp::forwardWndProc));
                return TRUE;
            case WM_CREATE:
                initControls(hwnd);
                return 0;
            case WM_SIZE:
                layoutControls(LOWORD(lParam), HIWORD(lParam));
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wParam)) {
                    case kCtlSendBtn:
                        onSendMessage();
                        return 0;
                    case kCtlFileBtn:
                        onSendFile();
                        return 0;
                    case kCtlRenameBtn:
                        onRename();
                        return 0;
                    default:
                        break;
                }
                break;
            case WM_TIMER:
                if (wParam == kTimerPeerRefresh) {
                    refreshPeerList();
                    refreshStatusBar();
                }
                return 0;
            case kMsgLogLine: {
                std::vector<std::string> batch;
                {
                    std::lock_guard<std::mutex> lock(pendingLogMutex_);
                    batch.swap(pendingLogs_);
                }
                for (const auto& line : batch) {
                    appendLogLine(line);
                }
                return 0;
            }
            case WM_CLOSE:
                shutdownApp();
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd, kTimerPeerRefresh);
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

private:
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND peerList_ = nullptr;
    HWND logEdit_ = nullptr;
    HWND inputEdit_ = nullptr;
    HWND sendBtn_ = nullptr;
    HWND fileBtn_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND renameBtn_ = nullptr;
    HWND statusStatic_ = nullptr;
    HFONT font_ = nullptr;
    std::mutex pendingLogMutex_;
    std::vector<std::string> pendingLogs_;

    LanTalkApp app_;
    bool started_ = false;
    std::vector<std::string> peerUserIds_;
};
#endif

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCmd) {
    try {
        WinGuiApp gui;
        return gui.run(instance, showCmd);
    } catch (const std::exception& ex) {
        MessageBoxA(nullptr, ex.what(), "LanTalk", MB_ICONERROR | MB_OK);
        return 1;
    }
}
#else
int main() {
    try {
        NetworkRuntime runtime;
        (void)runtime;
        LanTalkApp app;
        if (!app.init()) {
            std::cerr << app.lastError() << '\n';
            return 1;
        }
        app.run();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
        return 1;
    }
}
#endif
