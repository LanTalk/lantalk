#include "core.h"

#include <iostream>
#include <string>

int main() {
  lantalk::AppCore core;
  std::string error;
  if (!core.boot(error)) {
    std::cerr << "boot failed: " << error << std::endl;
    return 1;
  }

  std::cout << "LanTalk headless mode. Commands: bootstrap, snapshot\t<peer>\t<rev>, open\t<peer>, send_text\t<peer>\t<b64>\n";
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "quit" || line == "exit") {
      break;
    }
    std::cout << core.handle_rpc(line) << std::endl;
  }

  core.shutdown();
  return 0;
}
