# p2pchat

`p2pchat` 包含两部分：

- `worker/`：Cloudflare Worker + Durable Object，提供信令、在线状态、消息兜底（WS/HTTP拉取）。
- `pages/`：Cloudflare Pages 前端，UI 对齐桌面版风格，支持文本聊天，不支持文件发送。

部署顺序：

1. 先部署 `worker/`，拿到 `workers.dev` 地址。
2. 在 `pages/` 配置中填写信令服务器地址并部署。
3. 桌面版与网页版都把同一信令服务器地址加入设置即可互通文本消息。
