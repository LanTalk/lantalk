# LanTalk

一个基于 Go 的局域网聊天工具，支持 Win10/Win11 免安装运行，消息端到端加密。

## 技术栈

- GUI: `fyne`（主流 Go 跨平台桌面库）
- 网络: UDP 组播发现 + TCP 点对点消息
- 加密: X25519 + HKDF-SHA256 + AES-256-GCM
- 存储: 程序目录下本地 JSON 文件

## 关键特性

- 免安装：构建后直接运行 `LanTalk.exe`
- 数据本地化：所有配置和聊天记录都在程序目录 `data/`
- 端到端加密：仅通信双方可解密聊天内容
- 自动发现：同一局域网下自动显示在线联系人
- UI 结构对标老版 QQ：左侧联系人、右侧会话

## 目录与数据

运行后自动创建：
- `data/config.json`：本机身份与密钥
- `data/chats/*.json`：聊天记录

## 本地运行

```bash
go run .
```

## 本地构建 Windows 可执行文件

```bash
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-s -w -H=windowsgui" -o LanTalk.exe .
```

## GitHub Actions 打包

工作流：`.github/workflows/build-win.yml`

触发后会：
- 在 `windows-latest` 上编译 `dist/LanTalk/LanTalk.exe`
- 生成便携目录 `dist/LanTalk/`
- 直接上传该目录为 artifact（避免双层压缩）
