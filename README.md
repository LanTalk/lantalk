# LanTalk

一个面向 Win10/Win11 的局域网聊天工具（纯 Go），免安装运行，聊天消息端到端加密。

## 设计目标

- 纯 Go 代码（后续可扩展 Linux/macOS）
- Windows 便携版（解压即用）
- 所有数据保存在软件所在目录：`data/`
- 局域网自动发现在线用户
- 聊天消息端到端加密（X25519 + HKDF-SHA256 + AES-256-GCM）
- UI 风格参考老版 QQ：左侧联系人，右侧会话面板

## 本地运行

```bash
go run .
```

说明：
- 在 Linux/macOS 上当前只保留占位入口（后续扩展），完整 GUI 在 Windows 下运行。
- 首次启动会在程序目录创建：
  - `data/config.json`（身份与密钥）
  - `data/chats/*.json`（聊天记录）

## 本地构建 Windows 可执行文件

```bash
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-s -w -H=windowsgui" -o LanTalk.exe .
```

## GitHub Actions 打包

仓库已包含工作流：`.github/workflows/build-win.yml`

触发后会：
- 编译 `LanTalk.exe`
- 生成便携目录 `dist/LanTalk/`
- 打包 `dist/LanTalk-win-x64.zip`
- 上传为构建产物（artifact）
