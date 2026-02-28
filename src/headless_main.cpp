#include "core/app_core.h"

#include <iostream>

int main() {
  lantalk::app_core core;
  core.boot();
  std::cout << "LanTalk core booted. Build with -DLANTALK_ENABLE_DESKTOP=ON for embedded Web UI." << std::endl;
  std::cout << core.self_json() << std::endl;
  return 0;
}
