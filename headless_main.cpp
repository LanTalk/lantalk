#include "engine.h"

#include <iostream>
#include <string>

int main() {
  lantalk::LanTalkEngine engine;
  std::string error;
  if (!engine.boot(error)) {
    std::cerr << "boot failed: " << error << std::endl;
    return 1;
  }

  std::cout << "LanTalk headless\n";
  std::cout << "commands: bootstrap | snapshot\\t<peer> | open\\t<peer> | send\\t<peer>\\t<b64> | set_name\\t<b64>\n";

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "exit" || line == "quit") {
      break;
    }
    std::cout << engine.handle_rpc(line) << std::endl;
  }

  engine.shutdown();
  return 0;
}
