# LanTalk

LanTalk 已重构为 **C++ Core + Embedded Web UI** 架构（参考 QQ NT 的桌面壳模型）：

- 核心层（C++）：数据模型、会话状态、持久化存储、后续网络/加密能力。
- UI 层（Web）：HTML/CSS/JavaScript，运行在桌面内嵌 WebView 中，不是外部浏览器页面。
- 桥接层：Web UI 通过 Native API 调用 C++ Core。

## 目录结构

- `src/app_core.h` / `src/app_core.cpp`：核心逻辑
- `src/main.cpp`：桌面壳入口（WebView）
- `src/headless_main.cpp`：无桌面依赖的核心验证入口
- `src/ui_embedded.h`：内嵌 Web UI（编译进 exe）
- `.github/workflows/build-win.yml`：Windows 便携打包

## 数据目录

运行后在可执行文件目录生成：

- `data/profile.txt`：本机身份（ID 不可手动改）
- `data/peers.txt`：会话列表与最近状态
- `data/chats/*.log`：聊天记录

## 本地构建

### Linux（核心验证）

```bash
cmake -S . -B build -DLANTALK_ENABLE_DESKTOP=OFF
cmake --build build -j
./build/lantalk
```

### Windows（桌面版）

```powershell
cmake -S . -B build -DLANTALK_ENABLE_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## 当前状态

本次是一次彻底架构迁移，已完成：

- C++ 核心工程化
- 局域网自动发现（UDP 广播心跳）
- Web UI 三栏主界面（QQ NT 风格布局）
- Native Bridge（读取身份、会话列表、消息、发送文本、改昵称）
- 本地数据持久化

文件/图片传输与端到端加密链路会在这个新架构上继续补齐。
