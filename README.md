# LanTalk

LanTalk 是一个 Windows x64 的局域网聊天工具。

- 原生图形界面交互（在线用户列表、消息窗口、输入区、文件发送）
- 自动发现同一局域网内在线用户
- 支持文字消息和文件发送
- 免安装，启动后自动在运行目录保存数据
- 首次运行生成唯一用户识别码（`user_id`）
- 同一目录同一 EXE 禁止双开

## 技术栈

- C++17
- CMake
- WinSock2（UDP 广播发现 + TCP 消息/文件传输）
- GitHub Actions（Windows x64 自动编译并产出可下载 Artifact）
