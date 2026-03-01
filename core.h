#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lantalk {

class AppCore {
public:
  AppCore();
  ~AppCore();

  bool boot(std::string &error);
  void shutdown();

  void set_file_picker(std::function<std::string(bool image_only)> picker);

  // Bridge command format: command\targ1\targ2...
  std::string handle_rpc(const std::string &command_line);

private:
  struct Message {
    std::string id;
    std::int64_t ts_ms = 0;
    bool outgoing = false;
    std::string kind; // text, emoji, image, file
    std::string text;
    std::string file_name;
    std::uint64_t file_size = 0;
    std::string local_path;
  };

  struct Peer {
    std::string id;
    std::string name;
    std::string remark;
    std::string avatar;
    std::string ip;
    std::uint16_t port = 0;
    std::string public_key_hex;
    std::int64_t first_seen_ms = 0;
    std::int64_t last_seen_ms = 0;
    std::int64_t last_chat_ms = 0;
    bool online = false;
    std::uint32_t unread = 0;
  };

  struct Identity {
    std::array<std::uint8_t, 32> secret{};
    std::array<std::uint8_t, 32> pub{};
    std::string id;
    std::string pub_hex;
  };

  struct IncomingTransfer;

  struct InstanceLock;
  struct DataSlotLock;
  struct SocketState;

  bool init_identity(std::string &error);
  bool init_data_slot(std::string &error);
  bool init_profile();
  void save_profile();

  void load_peers();
  void save_peers();

  void load_messages();
  void append_message_to_disk(const std::string &peer_id, const Message &m);

  bool start_network(std::string &error);
  void stop_network();

  void discovery_loop();
  void announce_presence();
  void consume_presence_packet(const std::string &packet, const std::string &source_ip);

  void accept_loop();
  void handle_connection(std::intptr_t sock, const std::string &source_ip);

  bool send_text_like(const std::string &peer_id, const std::string &kind, const std::string &text, std::string &error);
  bool send_file_like(const std::string &peer_id, const std::filesystem::path &path, bool image, std::string &error);

  bool send_plain_payload(const Peer &peer, const std::vector<std::uint8_t> &plain, std::string &error);
  bool send_plain_payload_on_socket(std::intptr_t sock,
                                    const Peer &peer,
                                    const std::vector<std::uint8_t> &plain,
                                    std::string &error);

  void process_plain_payload(const std::string &source_ip,
                             const std::string &sender_id,
                             const std::array<std::uint8_t, 32> &sender_pub,
                             const std::vector<std::uint8_t> &plain);

  void on_incoming_text(const std::string &peer_id, bool emoji, std::int64_t ts_ms, const std::string &text);
  void on_incoming_file_start(const std::string &peer_id,
                              const std::array<std::uint8_t, 16> &transfer_id,
                              bool image,
                              std::uint64_t file_size,
                              const std::string &name);
  void on_incoming_file_chunk(const std::string &peer_id,
                              const std::array<std::uint8_t, 16> &transfer_id,
                              std::uint64_t offset,
                              const std::vector<std::uint8_t> &bytes);
  void on_incoming_file_end(const std::string &peer_id,
                            const std::array<std::uint8_t, 16> &transfer_id,
                            std::uint64_t final_size);

  std::string rpc_bootstrap();
  std::string rpc_snapshot(const std::vector<std::string> &parts);
  std::string rpc_open_peer(const std::vector<std::string> &parts);
  std::string rpc_send_text(const std::vector<std::string> &parts, bool emoji);
  std::string rpc_send_file(const std::vector<std::string> &parts, bool image);
  std::string rpc_set_name(const std::vector<std::string> &parts);
  std::string rpc_set_avatar(const std::vector<std::string> &parts);
  std::string rpc_set_remark(const std::vector<std::string> &parts);
  std::string rpc_peer_profile(const std::vector<std::string> &parts);
  std::string rpc_pick(bool image);

  std::string snapshot_json(const std::string &active_peer, std::uint64_t known_revision);
  std::string conversations_json() const;
  std::string messages_json(const std::string &peer_id) const;
  std::string self_json() const;

  std::string resolve_display_name(const Peer &peer) const;
  std::string summarize_message(const Message &m) const;

  static std::string now_iso8601();

  static std::string json_escape(const std::string &in);
  static std::string b64_encode(const std::string &in);
  static std::string b64_decode(const std::string &in);
  static std::string b64_encode_bytes(const std::vector<std::uint8_t> &in);
  static std::vector<std::uint8_t> b64_decode_bytes(const std::string &in);

  static std::vector<std::string> split(const std::string &in, char sep);
  static std::string join_path_uri(const std::filesystem::path &p);

  void bump_revision();
  std::int64_t now_ms() const;

  static bool random_bytes(std::uint8_t *dst, std::size_t n);
  static std::string hex_encode(const std::uint8_t *data, std::size_t n);
  static bool hex_decode(const std::string &hex, std::uint8_t *out, std::size_t out_len);

  static std::string sanitize_filename(const std::string &name);
  std::filesystem::path unique_download_path(const std::string &peer_id, const std::string &name) const;

  static bool is_image_path(const std::filesystem::path &p);

private:
  mutable std::mutex mu_;
  std::atomic<bool> running_{false};

  std::unique_ptr<InstanceLock> instance_lock_;
  std::unique_ptr<DataSlotLock> slot_lock_;
  std::unique_ptr<SocketState> sockets_;

  std::filesystem::path exe_dir_;
  std::filesystem::path exe_path_;
  std::filesystem::path data_dir_;

  Identity self_{};
  std::string self_name_;
  std::string self_avatar_;
  std::string active_peer_;

  std::unordered_map<std::string, Peer> peers_;
  std::unordered_map<std::string, std::vector<Message>> messages_;
  std::unordered_map<std::string, IncomingTransfer> incoming_transfers_;

  std::function<std::string(bool image_only)> file_picker_;

  std::atomic<std::uint64_t> revision_{1};
};

} // namespace lantalk
