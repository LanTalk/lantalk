#include "core/app_core.h"

#include <filesystem>
#include <fstream>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

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

std::string trim(const std::string &in) {
  std::size_t s = 0;
  while (s < in.size() && std::isspace(static_cast<unsigned char>(in[s]))) {
    ++s;
  }
  std::size_t e = in.size();
  while (e > s && std::isspace(static_cast<unsigned char>(in[e - 1]))) {
    --e;
  }
  return in.substr(s, e - s);
}

std::vector<std::string> parse_json_args(const std::string &req) {
  std::vector<std::string> out;
  std::size_t i = 0;
  const auto skip_ws = [&]() {
    while (i < req.size() && std::isspace(static_cast<unsigned char>(req[i]))) {
      ++i;
    }
  };

  skip_ws();
  if (i >= req.size() || req[i] != '[') {
    return out;
  }
  ++i;

  while (i < req.size()) {
    skip_ws();
    if (i < req.size() && req[i] == ']') {
      ++i;
      break;
    }

    if (i < req.size() && req[i] == '"') {
      ++i;
      std::string s;
      while (i < req.size()) {
        const char c = req[i++];
        if (c == '"') {
          break;
        }
        if (c == '\\' && i < req.size()) {
          const char e = req[i++];
          switch (e) {
          case '"': s.push_back('"'); break;
          case '\\': s.push_back('\\'); break;
          case '/': s.push_back('/'); break;
          case 'b': s.push_back('\b'); break;
          case 'f': s.push_back('\f'); break;
          case 'n': s.push_back('\n'); break;
          case 'r': s.push_back('\r'); break;
          case 't': s.push_back('\t'); break;
          default: s.push_back(e); break;
          }
          continue;
        }
        s.push_back(c);
      }
      out.push_back(std::move(s));
    } else {
      const auto start = i;
      while (i < req.size() && req[i] != ',' && req[i] != ']') {
        ++i;
      }
      out.push_back(trim(req.substr(start, i - start)));
    }

    skip_ws();
    if (i < req.size() && req[i] == ',') {
      ++i;
      continue;
    }
    if (i < req.size() && req[i] == ']') {
      ++i;
      break;
    }
  }
  return out;
}

std::string arg_at(const std::string &req, std::size_t idx) {
  auto args = parse_json_args(req);
  if (idx >= args.size()) {
    return "";
  }
  return args[idx];
}

} // namespace

int run_app() {
  lantalk::app_core core;
  core.boot();

  webview::webview win(true, nullptr);
  win.set_title("LanTalk");
  win.set_size(1320, 820, WEBVIEW_HINT_NONE);

  win.bind("nativeSelf", [&core](std::string) { return core.self_json(); });
  win.bind("nativeConversations", [&core](std::string) { return core.conversations_json(); });
  win.bind("nativeMessages", [&core](std::string req) { return core.messages_json(arg_at(req, 0)); });
  win.bind("nativeRenameSelf", [&core](std::string req) {
    return core.rename_self(arg_at(req, 0)) ? "true" : "false";
  });
  win.bind("nativeSendText", [&core](std::string req) {
    return core.send_text(arg_at(req, 0), arg_at(req, 1)) ? "true" : "false";
  });

  win.set_html(load_ui_html());
  win.run();
  return 0;
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return run_app();
}
#endif

int main() {
  return run_app();
}
