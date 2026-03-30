具体分工：

| 代码库              | 开发环境                               | 部署/运行环境           | 原因                                                         |
| :------------------ | :------------------------------------- | :---------------------- | :----------------------------------------------------------- |
| **Qt 客户端** (C++) | **Windows** (Qt Creator / CLion)       | Windows / macOS / Linux | 你的主力机就是 Windows，Qt 6.9 跨平台，直接在 Windows 上开发调试 |
| **Go 信令服务器**   | **Windows** (VSCode / GoLand)          | Ubuntu 服务器           | Go 完美跨平台编译，Windows 写代码 → `GOOS=linux go build` 交叉编译 → `scp` 到 Ubuntu 运行 |
| **C++ SFU**         | **Windows** 开发 + **Ubuntu** 编译运行 | Ubuntu 服务器           | Boost.Asio + UDP 高性能调试需在 Linux 下完成，Windows 上写代码，SSH 到 Ubuntu 编译测试 |
| **Protobuf**        | **Windows**                            | 双端共用                | `.proto` 文件在 Windows 编辑，`protoc` 一次生成 Go + C++ 代码 |



### 日常开发工作流

你的 Windows 主机                    Ubuntu 24.04 服务器
┌──────────────────┐                ┌──────────────────────┐
│                  │                │                      │
│  Qt Creator      │   本地运行      │  Redis / MariaDB     │
│  └─ 客户端开发    │ ◄────────────► │  Nginx / coturn      │
│                  │   TCP 信令连接   │                      │
│  VSCode / GoLand │                │  Go 信令服务器 (运行)   │
│  └─ Go 信令开发   │ ── scp/rsync → │  C++ SFU (编译+运行)  │
│  └─ SFU 代码编辑  │                │                      │
│                  │                │                      │
└──────────────────┘                └──────────────────────┘
        ▲ 写代码                           ▲ 跑服务