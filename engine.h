#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lantalk {

class LanTalkEngine {
public:
  LanTalkEngine();
  ~LanTalkEngine();

  bool boot(std::string &error);
  void shutdown();

  // command format: "cmd\targ1\targ2"
  std::string handle_rpc(const std::string &line);

private:
  struct Message {
    std::string id;
    std::int64_t ts_ms = 0;
    bool outgoing = false;
    std::string text;
  };

  struct Peer {
    std::string id;
    std::string name;
    std::string ip;
    std::uint16_t port = 0;
    std::int64_t first_seen_ms = 0;
    std::int64_t last_seen_ms = 0;
    std::int64_t last_chat_ms = 0;
    bool online = false;
    std::uint32_t unread = 0;
  };

  struct DataSlotLock;
  struct SocketState;

  bool init_data_slot(std::string &error);
  bool init_profile(std::string &error);
  void save_profile();
  void load_peers();
  void save_peers();
  void load_messages();
  void append_message_to_disk(const std::string &peer_id, const Message &m);

  bool start_network(std::string &error);
  void stop_network();
  void discovery_loop();
  void announce_presence(bool reply_only, const std::string &target_ip = "");
  void consume_presence(const std::string &packet, const std::string &source_ip);
  void accept_loop();
  void handle_connection(std::intptr_t sock, const std::string &source_ip);

  bool send_text(const std::string &peer_id, const std::string &text, std::string &error);

  std::string rpc_bootstrap();
  std::string rpc_snapshot(const std::vector<std::string> &parts);
  std::string rpc_open(const std::vector<std::string> &parts);
  std::string rpc_send(const std::vector<std::string> &parts);
  std::string rpc_set_name(const std::vector<std::string> &parts);

  std::string snapshot_json(const std::string &active_override);
  std::string self_json() const;
  std::string peers_json() const;
  std::string messages_json(const std::string &peer_id) const;

  static std::vector<std::string> split(const std::string &in, char sep);
  static std::string trim(const std::string &in);
  static std::string json_escape(const std::string &in);
  static std::string b64_encode(const std::string &in);
  static std::string b64_decode(const std::string &in);
  static bool random_bytes(std::uint8_t *dst, std::size_t n);
  static std::string hex_encode(const std::uint8_t *data, std::size_t n);
  static std::int64_t now_ms();
  static std::string format_hhmm(std::int64_t ts_ms);

private:
  mutable std::mutex mu_;
  std::atomic<bool> running_{false};

  std::filesystem::path work_dir_;
  std::filesystem::path data_dir_;

  std::string self_id_;
  std::string self_name_;

  std::string active_peer_;
  std::unordered_map<std::string, Peer> peers_;
  std::unordered_map<std::string, std::vector<Message>> messages_;

  std::unique_ptr<DataSlotLock> slot_lock_;
  std::unique_ptr<SocketState> sockets_;
};

} // namespace lantalk
