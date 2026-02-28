#include "core/app_core.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

#include <webview/webview.h>

namespace {

std::string exe_dir() {
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

std::string load_ui_html() {
  const auto root = std::filesystem::path(exe_dir());
  const auto ui_path = root / "ui" / "index.html";
  std::ifstream in(ui_path, std::ios::binary);
  if (!in.good()) {
    return R"(<html><body>ui/index.html not found next to executable.</body></html>)";
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

} // namespace

int main() {
  lantalk::app_core core;
  core.boot();

  webview::webview win(true, nullptr);
  win.set_title("LanTalk");
  win.set_size(1320, 820, WEBVIEW_HINT_NONE);

  win.bind("nativeSelf", [&core]() { return core.self_json(); });
  win.bind("nativeConversations", [&core]() { return core.conversations_json(); });
  win.bind("nativeMessages", [&core](const std::string &peer_id) { return core.messages_json(peer_id); });
  win.bind("nativeRenameSelf", [&core](const std::string &name) { return core.rename_self(name); });
  win.bind("nativeSendText", [&core](const std::string &peer_id, const std::string &text) {
    return core.send_text(peer_id, text);
  });

  win.set_html(load_ui_html());
  win.run();
  return 0;
}
