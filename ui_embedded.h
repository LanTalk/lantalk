#pragma once

#include <cstddef>
#include <string>

namespace lantalk {

#include "ui_embedded_data.h"

inline std::string ui_html() {
  return std::string(reinterpret_cast<const char *>(lantalk_ui_html),
                     static_cast<std::size_t>(lantalk_ui_html_len));
}

} // namespace lantalk
