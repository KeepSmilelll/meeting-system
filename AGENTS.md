# AGENTS.md — Plasma-Hawking 视频会议系统

> 此文件是 Codex 项目级上下文入口。Codex 每次启动任务时**自动读取**此文件。

## 1. 项目概况

跨平台视频会议系统。Go 信令服务器 + C++ SFU 媒体服务器 + Qt 6.9 客户端。

## 2. 目录结构

```
D:\meeting\
├── AGENTS.md                           ← 本文件 (Codex 自动加载)
├── development_progress.md             ← 当前进度跟踪 (每次任务完成后更新)
├── meeting-server/
│   ├── proto/                          ← Protobuf 定义 (Go + C++ 共用源)
│   │   ├── signaling.proto
│   │   ├── sfu_rpc.proto
│   │   └── generate.ps1 / generate.sh
│   ├── signaling/                      ← Go 信令服务器
│   │   ├── main.go
│   │   ├── config/                     ← Viper 配置
│   │   ├── server/                     ← TCP + Session + SessionManager
│   │   ├── handler/                    ← 信令 Handler (Auth/Meeting/Chat/Media)
│   │   ├── auth/                       ← JWT + RateLimiter
│   │   ├── model/                      ← GORM 模型
│   │   ├── store/                      ← MySQL + Redis 仓储
│   │   ├── protocol/                   ← 二进制帧编解码
│   │   ├── pkg/                        ← Logger + Snowflake
│   │   └── sfu/                        ← SFU RPC 客户端
│   ├── sfu/                            ← C++ SFU 媒体服务器
│   │   ├── server/                     ← UDP + RPC + MediaIngress
│   │   ├── room/                       ← Room + Publisher + Subscriber
│   │   ├── rtp/                        ← RtpParser + RtpRouter + RtcpHandler + NackBuffer
│   │   ├── bwe/                        ← BandwidthEstimator
│   │   └── proto/                      ← SFU proto 生成代码
│   ├── sql/                            ← MySQL 建表脚本
│   ├── tests/                          ← 集成测试
│   └── docker-compose.yml
├── plasma-hawking/                     ← Qt 6.9 客户端
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   ├── app/                        ← MeetingController + UserManager + AppStateMachine
│   │   ├── av/
│   │   │   ├── capture/                ← AudioCapture + CameraCapture + ScreenCapture
│   │   │   ├── codec/                  ← AudioEncoder/Decoder + VideoEncoder/Decoder
│   │   │   ├── process/                ← AEC + NS + AGC
│   │   │   ├── render/                 ← VideoRenderer + NV12Shader + AudioPlayer
│   │   │   └── sync/                   ← AVSync
│   │   ├── net/
│   │   │   ├── signaling/              ← SignalingClient + SignalProtocol + Reconnector
│   │   │   ├── media/                  ← RTPSender/Receiver + JitterBuffer + RTCPHandler
│   │   │   └── ice/                    ← StunClient + TurnClient
│   │   ├── storage/                    ← DatabaseManager + *Repository
│   │   ├── security/                   ← TokenManager + CryptoUtils
│   │   └── common/                     ← FFmpegUtils + RingBuffer + Logger + Types
│   ├── qml/                            ← QML UI 组件
│   ├── shaders/                        ← GLSL 着色器
│   ├── proto/                          ← 客户端 proto 生成代码
│   └── tests/                          ← 单元测试
└── docs/                               ← 设计文档 (在 Antigravity workspace)
	└── audit/                          ← 审计文档
```

## 3. 技术栈 (严格版本)

| 模块 | 技术 | 版本 |
|------|------|------|
| 客户端 | Qt Quick / QML + C++ | Qt 6.9, C++17 |
| 音视频 | FFmpeg | 7.1 |
| 渲染 | OpenGL Core Profile | 4.6 |
| 信令 | Go + goroutine + go-redis + GORM | Go 1.22+ |
| SFU | C++ + Boost.Asio UDP | C++17 |
| 协议 | Protobuf + 自定义二进制帧 | proto3 |
| 数据库 | MySQL 8 (服务端) + SQLite3 (客户端) | — |
| 中间件 | Redis + Nginx + coturn | — |

## 4. 铁律 — 违反即拒绝 PR

> [!CAUTION]
> 以下规则**无例外**。任何生成的代码如果违反，必须在提交前修复。

### C++ / FFmpeg
1. **零裸指针**：FFmpeg 结构体 (`AVFrame`, `AVPacket`, `AVCodecContext`, `AVBufferRef`) **必须**用 `std::unique_ptr` + 自定义 Deleter 管理。参考 `src/common/FFmpegUtils.h`。
2. **禁止旧版声道 API**：使用 `AVChannelLayout`，**禁止** `codec_ctx->channel_layout` 或 `codec_ctx->channels`。
3. **禁止实时 swscale**：NV12 硬解帧直接上传 GPU 纹理 (`GL_RED` + `GL_RG`)，用 Shader 做 YUV→RGB。
4. **OpenGL Core Profile**：Shader 首行 `#version 460 core`；绘制前**必须**绑定 VAO。

### QML / Qt
5. **禁止旧版 import**：写 `import QtQuick`，**禁止** `import QtQuick 2.x`。
6. **禁止 QWidget**：全部使用 Qt Quick / QML。
7. **组件化**：`Main.qml` 不超过 80 行，按职责拆分子组件。
8. **响应式布局**：使用 `Layouts` + `SplitView`，**禁止**写死绝对宽高。

### Go
9. **Session 模型**：每连接 3 个 goroutine (read/write/heartbeat)，写操作通过 `chan []byte` 投递。**禁止**在 readLoop 中直接 `conn.Write`。
10. **错误包装**：`fmt.Errorf("模块: %w", err)` 保留错误链。
11. **Handler 测试**：每个 `*_handler.go` 必须有对应的 `*_handler_test.go`。

### 通用
12. **GUI 线程零阻塞**：主线程**禁止** FFmpeg 操作、文件 I/O、重计算。
13. **注释业务意图**：注释解释"为什么"，而非"做了什么"。
14. **防死锁**：`QWaitCondition::wait` 必须带超时 + 停机时 `wakeAll()`。

## 5. 二进制帧协议

```
[0xAB 0xCD] [0x01] [Type 2B BE] [Length 4B BE] [Protobuf Payload]
 Magic       Ver    信令类型      Payload长度     消息体 (≤1MB)
```

## 6. 信令类型速查

```
认证: 0x0101~0x0107    会议: 0x0201~0x020E    媒体: 0x0301~0x0305
聊天: 0x0401~0x0403    文件: 0x0501~0x0505
```

## 7. Redis Key 规范

```
session:{user_id}               → Hash (node_id, session_id, meeting_id, status)
room:{meeting_id}               → Hash (host_id, sfu_node_id, status)
room_members:{meeting_id}       → Set (user_id...)
room_member:{meeting_id}:{uid}  → Hash (display_name, role, media_state)
sfu_route:{meeting_id}          → String (sfu_addr)
login_fail:{ip}                 → Counter (TTL 600s)
```

## 8. 测试命令

```bash
# Go 信令
cd meeting-server/signaling && go test ./... -v -count=1

# C++ SFU
cd meeting-server/sfu/build && cmake --build . && ctest --output-on-failure

# Qt 客户端
cd plasma-hawking/build && cmake --build . && ctest --output-on-failure
```

## 9. Subagent 分工

| Agent | 代码范围 | 核心约束 |
|-------|---------|---------|
| **Proto** | `meeting-server/proto/*` + 所有生成代码 | 向后兼容，编号范围不冲突 |
| **Go-Sig** | `meeting-server/signaling/**` | goroutine+channel, Argon2id, 每 handler 有测试 |
| **Qt-UI** | `plasma-hawking/src/app/**` + `qml/**` + `storage/**` | 组件化 QML, Q_PROPERTY, 独立 DB 线程 |
| **AV-Eng** | `plasma-hawking/src/av/**` + `src/net/media/**` | RAII, 硬件优先, 零拷贝渲染, Pull-Model 同步 |
| **Cpp-SFU** | `meeting-server/sfu/**` | 零转码, NACK 缓冲 500 包, SSRC 重写 |

## 10. 验证清单 (每个任务完成后执行)

- [ ] `go test ./...` 全部通过
- [ ] `ctest` 全部通过 (如有 C++ 改动)
- [ ] 无 FFmpeg 裸指针 (`grep -rn "AVFrame\s*\*" --include="*.h" --include="*.cpp" | grep -v unique_ptr`)
- [ ] 无旧版 QML import (`grep -rn "import QtQuick [0-9]" --include="*.qml"`)
- [ ] 无旧版 channel_layout (`grep -rn "->channel_layout\|->channels\s*=" --include="*.cpp"`)
- [ ] 新 Handler 有对应 `_test.go`
- [ ] `development_progress.md` 已更新，把 Current Snapshot 里的已完成步骤移回对应 Phase，快照改只保留当前阶段性结论。
