#pragma once

#include <cstdint>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lantalk {

struct message {
  std::string id;
  std::string peer_id;
  bool outgoing{false};
  std::int64_t sent_at{0};
  std::string text;
};

struct peer {
  std::string id;
  std::string name;
  bool online{false};
  std::int64_t last_seen_at{0};
  std::string last_text;
  std::int64_t last_sent_at{0};
};

class app_core {
public:
  app_core();
  ~app_core();
  void boot();

  std::string self_json();
  std::string conversations_json();
  std::string messages_json(const std::string &peer_id);

  bool rename_self(const std::string &name);
  bool send_text(const std::string &peer_id, const std::string &text);

private:
  std::string base_dir_;
  std::string data_dir_;
  std::string chats_dir_;
  std::string profile_file_;
  std::string peers_file_;
  std::string lock_file_;
  std::string instance_id_;

  std::string self_id_;
  std::string self_name_;

  std::unordered_map<std::string, peer> peers_;
  std::unordered_map<std::string, std::vector<message>> chats_;
  std::mutex mu_;
  std::atomic<bool> running_{false};
  std::thread discover_thread_;
  std::uintptr_t lock_handle_{0};

  static std::string trim(const std::string &in);
  static std::string to_upper(std::string in);
  static std::string json_escape(const std::string &in);
  static std::int64_t now_unix();
  static std::string random_hex(std::size_t bytes);
  static std::string sanitize_id(const std::string &raw);
  static std::string b64_encode(const std::string &in);
  static std::string b64_decode(const std::string &in);
  static std::string data_slot_name(int idx);
  static std::string local_ipv4_broadcast();

  static std::string exe_dir();
  static std::vector<std::string> split_tab(const std::string &line);

  void select_data_dir();
  bool try_lock_data_dir(const std::string &dir);
  void release_data_dir_lock();
  void ensure_dirs();
  void load_profile();
  void save_profile();
  void load_peers();
  void save_peers();
  void load_chats();
  void save_chat(const std::string &peer_id);
  std::string make_msg_id() const;
  void refresh_online_locked(std::int64_t now);
  void upsert_peer_seen(const std::string &id, const std::string &name, std::int64_t seen_at);
  void start_discovery();
  void stop_discovery();
  void discovery_loop();
};

} // namespace lantalk
