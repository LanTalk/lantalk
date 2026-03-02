#include "app.h"

#include <iostream>
#include <string>

int main() {
  lantalk::App app;
  std::string error;
  if (!app.boot(error)) {
    std::cerr << "boot failed: " << error << std::endl;
    return 1;
  }

  std::cout << "LanTalk headless mode\n";
  std::cout << "commands: bootstrap | snapshot\\t<peer>\\t<rev> | open\\t<peer> | send\\t<peer>\\t<b64> | set_name\\t<b64>\n";

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "exit" || line == "quit") {
      break;
    }
    std::cout << app.rpc(line) << std::endl;
  }

  app.shutdown();
  return 0;
}
