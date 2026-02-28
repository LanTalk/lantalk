# LanTalk

一个基于 Go 的局域网聊天工具，支持 Win10/Win11 免安装运行，消息端到端加密，支持文件传输。

## 技术栈

- GUI: `fyne`（主流 Go 跨平台桌面库）
- 网络: UDP 自动发现 + TCP 点对点通信
- 加密: X25519 + HKDF-SHA256 + AES-256-GCM
- 存储: 程序目录下本地 JSON 文件

## 关键特性

- 免安装：构建后直接运行 `LanTalk.exe`
- 数据本地化：所有配置和聊天记录都在程序目录 `data/`
- 端到端加密：聊天文本与文件内容都经过端到端加密
- 自动发现：
  - 组播发现
  - 广播发现（包含定向广播）
  - 自动端口探测（覆盖可互通的跨子网场景）
- 文件传输：支持发送文件（当前单文件最大 8MB）
- UI 结构对标老版 QQ：左侧联系人、右侧会话

## 目录与数据

运行后自动创建：
- `data/config.json`：本机身份与密钥
- `data/chats/*.json`：聊天记录
- `data/downloads/`：接收文件目录

## 如何自测

1. 同一台机器双开程序（同一目录直接启动两次即可）。
2. 两个窗口会以不同“实例 ID”互相发现（同昵称会显示为本机不同实例）。
3. 互发文本消息、发送文件，确认：
   - 聊天区都有记录
   - 接收端 `data/downloads/` 出现文件

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
