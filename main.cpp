#include "core.h"
#include "ui_embedded.h"

#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include <webview/webview.h>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShObjIdl.h>
#endif

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
      std::string value;
      while (i < req.size()) {
        char c = req[i++];
        if (c == '"') {
          break;
        }
        if (c == '\\' && i < req.size()) {
          const char e = req[i++];
          switch (e) {
          case '"': value.push_back('"'); break;
          case '\\': value.push_back('\\'); break;
          case '/': value.push_back('/'); break;
          case 'b': value.push_back('\b'); break;
          case 'f': value.push_back('\f'); break;
          case 'n': value.push_back('\n'); break;
          case 'r': value.push_back('\r'); break;
          case 't': value.push_back('\t'); break;
          default: value.push_back(e); break;
          }
          continue;
        }
        value.push_back(c);
      }
      out.push_back(std::move(value));
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

#if defined(_WIN32)
std::wstring utf16_from_utf8(const std::string &in) {
  if (in.empty()) {
    return L"";
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, nullptr, 0);
  if (needed <= 0) {
    return L"";
  }
  std::wstring out;
  out.resize(static_cast<std::size_t>(needed - 1));
  MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, out.data(), needed);
  return out;
}

std::string utf8_from_utf16(const std::wstring &in) {
  if (in.empty()) {
    return "";
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, in.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return "";
  }
  std::string out;
  out.resize(static_cast<std::size_t>(needed - 1));
  WideCharToMultiByte(CP_UTF8, 0, in.c_str(), -1, out.data(), needed, nullptr, nullptr);
  return out;
}

std::string pick_path_native(bool image_only) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool did_init = SUCCEEDED(hr);

  IFileOpenDialog *dialog = nullptr;
  hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (FAILED(hr) || dialog == nullptr) {
    if (did_init) {
      CoUninitialize();
    }
    return "";
  }

  DWORD options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
  }

  COMDLG_FILTERSPEC filters[2];
  if (image_only) {
    filters[0] = {L"Image Files", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp"};
    filters[1] = {L"All Files", L"*.*"};
    dialog->SetFileTypes(2, filters);
    dialog->SetFileTypeIndex(1);
  }

  std::string result;
  if (SUCCEEDED(dialog->Show(nullptr))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
      PWSTR raw_path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) && raw_path != nullptr) {
        result = utf8_from_utf16(raw_path);
        CoTaskMemFree(raw_path);
      }
      item->Release();
    }
  }

  dialog->Release();
  if (did_init) {
    CoUninitialize();
  }
  return result;
}
#else
std::string pick_path_native(bool) {
  return "";
}
#endif

int run_app() {
  lantalk::AppCore core;
  std::string error;
  if (!core.boot(error)) {
    if (error == "instance already running") {
      return 0;
    }
    std::cerr << "boot failed: " << error << std::endl;
    return 1;
  }

  core.set_file_picker([](bool image_only) {
    return pick_path_native(image_only);
  });

  webview::webview win(true, nullptr);
  win.set_title("LanTalk");
  win.set_size(1360, 860, WEBVIEW_HINT_NONE);

  win.bind("native", [&core](const std::string &req) -> std::string {
    const auto args = parse_json_args(req);
    if (args.empty()) {
      return R"({"ok":false,"error":"missing arg"})";
    }
    return core.handle_rpc(args[0]);
  });

  win.set_html(lantalk::kUiHtml);
  win.run();

  core.shutdown();
  return 0;
}

} // namespace

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return run_app();
}
#endif

int main() {
  return run_app();
}
