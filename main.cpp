#include "engine.h"
#include "ui_embedded.h"

#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include <webview/webview.h>

namespace {

std::string trim(const std::string &in) {
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

std::vector<std::string> parse_json_args(const std::string &req) {
  std::vector<std::string> out;
  std::size_t i = 0;

  const auto skip_ws = [&]() {
    while (i < req.size() && std::isspace(static_cast<unsigned char>(req[i])) != 0) {
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
    }
  }

  return out;
}

int run_app() {
  lantalk::LanTalkEngine engine;
  std::string error;
  if (!engine.boot(error)) {
    std::cerr << "boot failed: " << error << std::endl;
    return 1;
  }

  webview::webview win(true, nullptr);
  win.set_title("LanTalk");
  win.set_size(1260, 840, WEBVIEW_HINT_NONE);

  win.bind("native", [&engine](const std::string &req) {
    const auto args = parse_json_args(req);
    if (args.empty()) {
      return std::string(R"({"ok":false,"error":"missing args"})");
    }
    return engine.handle_rpc(args[0]);
  });

  win.set_html(lantalk::ui_html());
  win.run();

  engine.shutdown();
  return 0;
}

} // namespace

#if defined(_WIN32)
#include <Windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return run_app();
}
#endif

int main() {
  return run_app();
}
